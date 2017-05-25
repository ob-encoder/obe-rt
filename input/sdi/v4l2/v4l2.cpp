/*****************************************************************************
 * v4l2.cpp: Video4Linux2 Far frame grabber input module
 *****************************************************************************
 * Copyright (C) 2016 Kernel Labs Inc.
 *
 * Authors: Steven Toth <stoth@kernellabs.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

/* Status 2016-06-03:
   Developmed for use specifically with the H727/C027 HDMI capture card - and KL device driver.
   Fixed at 720p/YUYV/48KHz audio. A handful of assumptions, hopefully mostly documented, that
   would need to be addressed for 1080i, 44.1Khz, non YUYV colorspaces, etc.
 */

#define __STDC_FORMAT_MACROS   1
#define __STDC_CONSTANT_MACROS 1

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/videodev2.h>

extern "C"
{
#include "common/common.h"
#include "common/lavc.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include "input/sdi/ancillary.h"
#include "input/sdi/vbi.h"
#include "input/sdi/x86/sdi.h"
#include <libavresample/avresample.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/bswap.h>
#include <libavresample/avresample.h>
#include <libavutil/opt.h>
#include <libyuv/convert.h>
#if 0
#include <pulse/simple.h>
#include <pulse/error.h>
#endif
#include <alsa/asoundlib.h>
}

#define DO_60 1
struct obe_to_v4l2
{
    int obe_name;
    uint32_t bmd_name;
};

struct obe_to_v4l2_video
{
    int obe_name;
    int width, height;
    uint32_t bmd_name;
    int timebase_num;
    int timebase_den;
};

const static struct obe_to_v4l2_video video_format_tab[] =
{
#if DO_60
    { INPUT_VIDEO_FORMAT_720P_60, 1280, 720, 0 /* bmdModeHD720p60 */, 1001, 60000 },
#else
    { INPUT_VIDEO_FORMAT_720P_60, 1280, 720, 0 /* bmdModeHD720p60 */, 1001, 30000 },
#endif
    { -1, 0, 0, 0, -1, -1 },
};

struct capture_buffer_s
{
	struct v4l2_buffer vidbuf;
	void *data;
	int length;
	int free;
};

typedef struct
{
	/* V4L2 */
	int fd;
	pthread_t vthreadId;
	int vthreadTerminate, vthreadRunning, vthreadComplete;

	/* ALSA */
	pthread_t athreadId;
	int athreadTerminate, athreadRunning, athreadComplete;

#define MAX_BUFFERS 16
	struct capture_buffer_s buffers[MAX_BUFFERS];
	int numFrames;

	int64_t last_frame_time;

	obe_device_t *device;
	obe_t *h;

	unsigned long long v_counter;
	unsigned long long a_counter;
	AVRational   v_timebase;
} v4l2_ctx_t;

typedef struct
{
    v4l2_ctx_t v4l2_ctx;

    /* Input */
    int card_idx;

    int video_format;
    int num_channels;

    /* True if we're problem, else false during normal streaming. */
    int probe;

    /* Output */
    int probe_success;

    int width;
    int height;
    int timebase_num;
    int timebase_den;

    int interlaced;
    int tff;
} v4l2_opts_t;

static int wait_for_frame_v4l2(int fd)
{
	struct timeval timeout;
	fd_set rdset;

	FD_ZERO(&rdset);
	FD_SET(fd, &rdset);

	timeout.tv_sec = 0;
	timeout.tv_usec = 100 * 1000;

	/* TODO: restart if you get back -EINTR. */
	int ret = select(fd + 1, &rdset, 0, 0, &timeout);
	if (ret == -1) {
		fprintf(stderr, "[v4l2] videoinput: Error waiting for frame: %s\n", strerror(errno));
	}

	return ret;
}

static int enqueue_buffer(v4l2_opts_t *v4l2_opts, int bufferNr)
{
	v4l2_ctx_t *v4l2_ctx = &v4l2_opts->v4l2_ctx;

	v4l2_ctx->buffers[bufferNr].free = 1;
	int ret = ioctl(v4l2_ctx->fd, VIDIOC_QBUF, &v4l2_ctx->buffers[bufferNr].vidbuf);
	if (ret < 0)
		syslog(LOG_ERR, "[v4l2]: Could not enq frame %d, ret = %d\n", bufferNr, ret);

	return ret;
}

static void *audioThreadFunc(void *p)
{
	v4l2_opts_t *v4l2_opts = (v4l2_opts_t *)p;
	v4l2_ctx_t *v4l2_ctx = &v4l2_opts->v4l2_ctx;
	obe_raw_frame_t *raw_frame = NULL;

	snd_pcm_t *capture_handle;
	snd_pcm_hw_params_t *hw_params;
	int err;
#define AUDIO_READ_SIZE ((48000 * 2 * 2) / 10)
	uint8_t buf[AUDIO_READ_SIZE];

	if ((err = snd_pcm_open(&capture_handle, "hw:1,0", SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		fprintf(stderr, "cannot open audio device, %s. Do you have audio group permissions?\n", snd_strerror(err));
		exit(1);
	}
		   
	if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
		fprintf(stderr, "cannot allocate hardware parameter structure (%s)\n", snd_strerror (err));
		exit(1);
	}
				 
	if ((err = snd_pcm_hw_params_any(capture_handle, hw_params)) < 0) {
		fprintf(stderr, "cannot initialize hardware parameter structure (%s)\n", snd_strerror (err));
		exit(1);
	}
	
	if ((err = snd_pcm_hw_params_set_access(capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		fprintf(stderr, "cannot set access type (%s)\n", snd_strerror (err));
		exit(1);
	}
	
	if ((err = snd_pcm_hw_params_set_format(capture_handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
		fprintf(stderr, "cannot set sample format (%s)\n", snd_strerror (err));
		exit(1);
	}

	unsigned int rate = 48000;
	int dir;
	if ((err = snd_pcm_hw_params_set_rate_near(capture_handle, hw_params, &rate, &dir)) < 0) {
		fprintf(stderr, "cannot set sample rate (%s)\n", snd_strerror (err));
		exit(1);
	}

	if ((err = snd_pcm_hw_params_set_channels(capture_handle, hw_params, 2)) < 0) {
		fprintf(stderr, "cannot set channel count (%s)\n", snd_strerror (err));
		exit(1);
	}
	
	if ((err = snd_pcm_hw_params(capture_handle, hw_params)) < 0) {
		fprintf(stderr, "cannot set parameters (%s)\n", snd_strerror (err));
		exit(1);
	}
	
	snd_pcm_hw_params_free(hw_params);

	if ((err = snd_pcm_prepare(capture_handle)) < 0) {
		fprintf(stderr, "cannot prepare audio interface for use (%s)\n", snd_strerror (err));
		exit(1);
	}

	v4l2_ctx->athreadRunning = 1;
	v4l2_ctx->athreadComplete = 0;
	v4l2_ctx->athreadTerminate = 0;

	/* Open the audio Device */
	while (!v4l2_ctx->athreadTerminate && v4l2_opts->probe == 0) {

		err = snd_pcm_readi(capture_handle, buf, AUDIO_READ_SIZE / 4);
		if (err != (AUDIO_READ_SIZE / 4)) {
			//fprintf (stderr, "[v4l2] alsa: read from audio interface failed (%s)\n", snd_strerror(err));
			//usleep(10 * 1000);
			continue;
		}

//		printf("audio: %02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3]);
// MMM
		raw_frame = new_raw_frame();
		if (!raw_frame) {
			syslog(LOG_ERR, "[v4l2]: Could not allocate raw audio frame\n" );
			break;
		}
		raw_frame->release_data = obe_release_audio_data;
		raw_frame->release_frame = obe_release_frame;
	        raw_frame->audio_frame.num_samples = AUDIO_READ_SIZE / 4;
		raw_frame->audio_frame.num_channels = 2;
		raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S16;
		raw_frame->audio_frame.linesize = 4;

		raw_frame->audio_frame.audio_data[0] = (uint8_t*)malloc(AUDIO_READ_SIZE);
		memcpy(raw_frame->audio_frame.audio_data[0], buf, AUDIO_READ_SIZE);

//		int64_t pts = av_rescale_q(v4l2_ctx->a_counter++, v4l2_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
		//obe_clock_tick(v4l2_ctx->h, pts);
		raw_frame->pts = 0;

		for (int i = 0; i < v4l2_ctx->device->num_input_streams; i++) {
			if (v4l2_ctx->device->streams[i]->stream_format == AUDIO_PCM) {
				raw_frame->input_stream_id = v4l2_ctx->device->streams[i]->input_stream_id;
			}
		}

		if (add_to_filter_queue(v4l2_ctx->h, raw_frame) < 0 ) {
		}
	}
	printf("Shutting down\n");

	/* Shutdown */
	snd_pcm_close(capture_handle);

	v4l2_ctx->athreadComplete = 1;
	pthread_exit(0);
	return 0;
}

static void *videoThreadFunc(void *p)
{
	v4l2_opts_t *v4l2_opts = (v4l2_opts_t *)p;
	v4l2_ctx_t *v4l2_ctx = &v4l2_opts->v4l2_ctx;
//	int frame_counter = 0;

	obe_raw_frame_t *raw_frame = NULL;
	int bufferType = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (ioctl(v4l2_ctx->fd, VIDIOC_STREAMON, &bufferType) < 0 ) {
		syslog(LOG_ERR, "[v4l2]: Could not transition to STREAMON\n" );
	}

	/* Place all buffers on the h/w */
	for (int i = 0; i < v4l2_ctx->numFrames; i++) {
		enqueue_buffer(v4l2_opts, i);
	}

	v4l2_ctx->vthreadRunning = 1;
	v4l2_ctx->vthreadComplete = 0;
	v4l2_ctx->vthreadTerminate = 0;
	v4l2_ctx->v_counter = 0;
	v4l2_ctx->a_counter = 0;
	while (!v4l2_ctx->vthreadTerminate && v4l2_opts->probe == 0) {

		if (wait_for_frame_v4l2(v4l2_ctx->fd) <= 0)
			continue;

		/* Dequeue a video frame */
		struct v4l2_buffer buf;
		buf.type = bufferType;
		if (ioctl(v4l2_ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
			syslog(LOG_ERR, "[v4l2]: Could not dq frame\n" );
			continue;
		}
//printf("[%08lld] dq idx %d\n", v4l2_ctx->v_counter, buf.index);

		v4l2_ctx->buffers[ buf.index ].free = 0;

#if DO_60
#else
		/* TODO: SLower platforms can't keep up with 60fps.
		 * frame drop for development purposes.
		 */
		if (frame_counter++ & 1) {
			/* Drop the frame */
			enqueue_buffer(v4l2_opts, buf.index);
			continue;
		}
#endif
		raw_frame = new_raw_frame();
		if (!raw_frame) {
			syslog(LOG_ERR, "[v4l2]: Could not allocate raw video frame\n" );
			break;
		}

//printf("data %p len = %d\n", frame_bytes, frame_len);

		/* TODO: YUYV only. Drivers will non YUYV colorspaces won't work reliably. */
		raw_frame->alloc_img.csp = (int)PIX_FMT_YUV420P;
		raw_frame->alloc_img.format = v4l2_opts->video_format;
		raw_frame->alloc_img.width = v4l2_opts->width;
		raw_frame->alloc_img.height = v4l2_opts->height;
		raw_frame->alloc_img.first_line = 1;
		raw_frame->alloc_img.planes = av_pix_fmt_descriptors[raw_frame->alloc_img.csp].nb_components;

		raw_frame->alloc_img.stride[0] = v4l2_opts->width;
		raw_frame->alloc_img.stride[1] = v4l2_opts->width / 2;
		raw_frame->alloc_img.stride[2] = v4l2_opts->width / 2;
		raw_frame->alloc_img.stride[3] = 0;

		int64_t pts = av_rescale_q(v4l2_ctx->v_counter++, v4l2_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
		obe_clock_tick(v4l2_ctx->h, pts);
		raw_frame->pts = pts;

//printf("pts = %lld\n", raw_frame->pts);
		raw_frame->timebase_num = v4l2_opts->timebase_num;
		raw_frame->timebase_den = v4l2_opts->timebase_den;

		raw_frame->alloc_img.plane[0] = (uint8_t *)calloc(1, v4l2_opts->width * v4l2_opts->height * 2);
		raw_frame->alloc_img.plane[1] = raw_frame->alloc_img.plane[0] + (v4l2_opts->width * v4l2_opts->height);
		raw_frame->alloc_img.plane[2] = raw_frame->alloc_img.plane[1] + ((v4l2_opts->width * v4l2_opts->height) / 4);
		raw_frame->alloc_img.plane[3] = 0;
		memcpy(&raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->alloc_img));

		raw_frame->release_data = obe_release_video_data;
		raw_frame->release_frame = obe_release_frame;

		/* Convert YUY2 to I420. */
		libyuv::YUY2ToI420((unsigned char *)v4l2_ctx->buffers[ buf.index ].data,
			v4l2_opts->width * 2,
			raw_frame->alloc_img.plane[0], v4l2_opts->width,
			raw_frame->alloc_img.plane[1], v4l2_opts->width / 2,
			raw_frame->alloc_img.plane[2], v4l2_opts->width / 2,
			v4l2_opts->width, v4l2_opts->height);

		if (add_to_filter_queue(v4l2_ctx->h, raw_frame) < 0 ) {
		}

		enqueue_buffer(v4l2_opts, buf.index);
	}

	if (ioctl(v4l2_ctx->fd, VIDIOC_STREAMOFF, &bufferType) < 0 ) {
		syslog(LOG_ERR, "[v4l2]: Could not transition to STREAMOFF\n" );
	}

	for (int i = 0; i < v4l2_ctx->numFrames; i++)
		munmap(v4l2_ctx->buffers[i].data, v4l2_ctx->buffers[i].length);

	v4l2_ctx->vthreadComplete = 1;
	pthread_exit(0);
	return 0;
}

static void close_device(v4l2_opts_t *v4l2_opts)
{
	v4l2_ctx_t *v4l2_ctx = &v4l2_opts->v4l2_ctx;

	if (v4l2_ctx->athreadRunning) {
		v4l2_ctx->athreadTerminate = 1;
		while (!v4l2_ctx->athreadComplete)
			usleep(50 * 1000);
	}

	if (v4l2_ctx->vthreadRunning) {
		v4l2_ctx->vthreadTerminate = 1;
		while (!v4l2_ctx->vthreadComplete)
			usleep(50 * 1000);
	}

	/* close(fd) */
}

static int open_device(v4l2_opts_t *v4l2_opts)
{
    v4l2_ctx_t *v4l2_ctx = &v4l2_opts->v4l2_ctx;
    int ret = 0;
    int l = 0;
    unsigned int video_input_nr = 0;

    char devfn[16];
    sprintf(devfn, "/dev/video%d", v4l2_opts->card_idx);
    v4l2_ctx->fd = open(devfn, O_RDWR);
    if (v4l2_ctx->fd < 0) {
        fprintf( stderr, "[v4l2] Could not open %s\n", devfn);
        ret = -1;
        goto finish;
    }

    struct v4l2_capability caps_v4l2;
    if (ioctl(v4l2_ctx->fd, VIDIOC_QUERYCAP, &caps_v4l2) < 0 ) {
        fprintf( stderr, "[v4l2] Could not query device capabilities\n" );
        ret = -1;
        goto finish;
    }

    fprintf(stderr, "[v4l2] Using video4linux2 driver '%s', card '%s' (bus %s).\n",
       caps_v4l2.driver, caps_v4l2.card, caps_v4l2.bus_info);

    fprintf(stderr, "[v4l2] Version is %u, capabilities %x.\n",
       caps_v4l2.version, caps_v4l2.capabilities);

    /* Select the video input */
    if (ioctl(v4l2_ctx->fd, VIDIOC_S_INPUT, &video_input_nr) < 0) {
        fprintf( stderr, "[v4l2] Could not select video input\n" );
        ret = -1;
        goto finish;
    }

    /* Confirm input selection */
    struct v4l2_input input;
    if (ioctl(v4l2_ctx->fd, VIDIOC_G_INPUT, &input.index) < 0) {
        fprintf(stderr, "[v4l2] Could not query video input\n" );
        ret = -1;
        goto finish;
    }

    if (video_input_nr != input.index) {
        fprintf(stderr, "[v4l2] Driver did not accept out video input selection\n" );
        ret = -1;
        goto finish;
    }

    /* check the detected resolution */
    while (l++ < 16) {
	usleep(100 * 1000);

       	fprintf(stderr, "[v4l2] Detecting\n");
        struct v4l2_format fmt;
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        int ret = ioctl(v4l2_ctx->fd, VIDIOC_G_FMT, &fmt);
	if (ret < 0) {
        	printf("ret = %d\n", ret);
		continue;
	}

       	fprintf(stderr, "[v4l2] Detected resolution %d x %d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);
	if (fmt.fmt.pix.width && fmt.fmt.pix.height) {
		v4l2_opts->width = fmt.fmt.pix.width;
		v4l2_opts->height = fmt.fmt.pix.height;
		if (fmt.fmt.pix.field == V4L2_FIELD_NONE)
			v4l2_opts->interlaced = 0;
		else
			v4l2_opts->interlaced = 1;
		/* TODO: hardcoded 720p reference */
		v4l2_opts->video_format = INPUT_VIDEO_FORMAT_720P_60;

		v4l2_ctx->v_timebase.num = 1001;
#if DO_60
		v4l2_ctx->v_timebase.den = 60000;
#else
		v4l2_ctx->v_timebase.den = 30000;
#endif

		break;
	}
    }


    /* Now setup the videobuf buffers kernel to userspace memory mappings */
    struct v4l2_requestbuffers req;
    req.count = 8;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(v4l2_ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "[v4l2] Driver error during _REQBUFS\n" );
        ret = -1;
        goto finish;
    }

    /* Preserve how many buffers the driver wants to use */
    v4l2_ctx->numFrames = req.count;

    /* Query each buffer and map it to the video device */
    for (int i = 0; i < v4l2_ctx->numFrames; i++) {
        struct v4l2_buffer *vidbuf = &v4l2_ctx->buffers[i].vidbuf;

        vidbuf->index = i;
        vidbuf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vidbuf->memory = V4L2_MEMORY_MMAP;
        if (ioctl(v4l2_ctx->fd, VIDIOC_QUERYBUF, vidbuf) < 0 ) {
            fprintf(stderr, "[v4l2]: Can't get information about buffer %d: %s.\n", i, strerror(errno));
            ret = -1;
            goto finish;
        }

        v4l2_ctx->buffers[i].data = mmap(0, vidbuf->length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2_ctx->fd, vidbuf->m.offset);
        if (v4l2_ctx->buffers[i].data == MAP_FAILED) {
            fprintf(stderr, "[v4l2]: Can't map buffer %d: %s.\n", i, strerror(errno));
            ret = -1;
            goto finish;
        }
        v4l2_ctx->buffers[i].length = vidbuf->length;
    }

    avcodec_register_all();

    syslog( LOG_INFO, "Opened V4L2 PCI card /dev/video%d", v4l2_opts->card_idx);

    v4l2_opts->timebase_num = video_format_tab[0].timebase_num;
    v4l2_opts->timebase_den = video_format_tab[0].timebase_den;

    if(!v4l2_opts->probe )
    {
    }

    pthread_create(&v4l2_ctx->vthreadId, 0, videoThreadFunc, v4l2_opts);
    pthread_create(&v4l2_ctx->athreadId, 0, audioThreadFunc, v4l2_opts);

    ret = 0;

finish:

    if (ret)
        close_device(v4l2_opts);

    return ret;
}

/* Called from open_input() */
static void close_thread(void *handle)
{
	if (!handle)
		return;

	v4l2_opts_t *v4l2_opts = (v4l2_opts_t*)handle;
	close_device(v4l2_opts);
	free(v4l2_opts);
}

static void *probe_stream(void *ptr)
{
    obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;
    obe_device_t *device;
    obe_int_input_stream_t *streams[MAX_STREAMS];
    int num_streams = 2;
    v4l2_ctx_t *v4l2_ctx;

    v4l2_opts_t *v4l2_opts = (v4l2_opts_t*)calloc( 1, sizeof(*v4l2_opts) );
    if (!v4l2_opts) {
        fprintf(stderr, "[v4l2] Unable to malloc opts\n" );
        goto finish;
    }

    /* TODO: support multi-channel */
    v4l2_opts->num_channels = 16;
    v4l2_opts->card_idx = user_opts->card_idx;
    v4l2_opts->video_format = user_opts->video_format;
    v4l2_opts->probe = 1;

    v4l2_ctx = &v4l2_opts->v4l2_ctx;
    v4l2_ctx->h = h;
    v4l2_ctx->last_frame_time = -1;

    /* Open device */
    if (open_device(v4l2_opts) < 0) {
        fprintf(stderr, "[v4l2] Unable to open the device\n" );
        goto finish;
    }

    sleep(1);

    close_device(v4l2_opts);

    v4l2_opts->probe_success = 1;
    fprintf(stderr, "[v4l2] Probe success\n" );

    if (!v4l2_opts->probe_success) {
        fprintf( stderr, "[v4l2] No valid frames received - check connection and input format\n" );
        goto finish;
    }

    /* TODO: probe for SMPTE 337M */
    /* TODO: factor some of the code below out */

    for( int i = 0; i < 2; i++ )
    {
        streams[i] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[i]) );
        if (!streams[i])
            goto finish;

        /* TODO: make it take a continuous set of stream-ids */
        pthread_mutex_lock( &h->device_list_mutex );
        streams[i]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock( &h->device_list_mutex );

        if (i == 0) {
            streams[i]->stream_type = STREAM_TYPE_VIDEO;
            streams[i]->stream_format = VIDEO_UNCOMPRESSED;
            streams[i]->width  = v4l2_opts->width;
            streams[i]->height = v4l2_opts->height;
            streams[i]->timebase_num = v4l2_opts->timebase_num;
            streams[i]->timebase_den = v4l2_opts->timebase_den;
            streams[i]->csp    = PIX_FMT_YUV420P;
            streams[i]->interlaced = v4l2_opts->interlaced;
            streams[i]->tff = 1; /* NTSC is bff in baseband but coded as tff */
            streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */
        }
        else if( i == 1 ) {
            /* TODO: various v4l2 assumptions about audio being 48KHz need resolved.
                     Some sources could be 44.1 and this module will fall down badly.
             */
            streams[i]->stream_type = STREAM_TYPE_AUDIO;
            streams[i]->stream_format = AUDIO_PCM;
            streams[i]->num_channels  = 2;
            streams[i]->sample_format = AV_SAMPLE_FMT_S16;
            streams[i]->sample_rate = 48000;
        }
    }

    device = new_device();

    if (!device)
        goto finish;

    device->num_input_streams = num_streams;
    memcpy(device->streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**));
    device->device_type = INPUT_DEVICE_V4L2;
    memcpy(&device->user_opts, user_opts, sizeof(*user_opts) );

    /* add device */
    add_device(h, device);

finish:
    v4l2_opts->probe = 0;
    if (v4l2_opts)
        free(v4l2_opts);

    free(probe_ctx);

    return NULL;
}

static void *open_input( void *ptr )
{
    obe_input_params_t *input = (obe_input_params_t*)ptr;
    obe_t *h = input->h;
    obe_device_t *device = input->device;
    obe_input_t *user_opts = &device->user_opts;
    v4l2_ctx_t *v4l2_ctx;

    v4l2_opts_t *v4l2_opts = (v4l2_opts_t*)calloc( 1, sizeof(*v4l2_opts) );
    if( !v4l2_opts )
    {
        fprintf( stderr, "Malloc failed\n" );
        return NULL;
    }

    pthread_cleanup_push(close_thread, (void*)v4l2_opts);

    v4l2_opts->num_channels = 16;
    v4l2_opts->card_idx = user_opts->card_idx;
    v4l2_opts->video_format = user_opts->video_format;

    v4l2_ctx = &v4l2_opts->v4l2_ctx;

    v4l2_ctx->device = device;
    v4l2_ctx->h = h;
    v4l2_ctx->last_frame_time = -1;
    v4l2_ctx->v_counter = 0;
    v4l2_ctx->a_counter = 0;

    /* TODO: wait for encoder */

    if (open_device(v4l2_opts) < 0)
        return NULL;

    sleep(INT_MAX);

    pthread_cleanup_pop(1);

    return NULL;
}

const obe_input_func_t v4l2_input = { probe_stream, open_input };
