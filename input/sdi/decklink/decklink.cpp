/*****************************************************************************
 * decklink.cpp: BlackMagic DeckLink SDI input module
 *****************************************************************************
 * Copyright (C) 2010 Steinar H. Gunderson
 *
 * Authors: Steinar H. Gunderson <steinar+vlc@gunderson.no>
 *
 * SCTE35 / SCTE104 and general code hardening, debugging features et al.
 * Copyright (C) 2015-2017 Kernel Labs Inc.
 * Authors: Steven Toth <stoth@kernellabs.com>
 * Authors: Devin J Heitmueller <dheitmueller@kernellabs.com>
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

#define __STDC_FORMAT_MACROS   1
#define __STDC_CONSTANT_MACROS 1

#define WRITE_OSD_VALUE 0
#define READ_OSD_VALUE 0

#if HAVE_LIBKLMONITORING_KLMONITORING_H
#define KL_PRBS_INPUT 0

#if KL_PRBS_INPUT
#include <libklmonitoring/kl-prbs.h>
#endif

#endif

extern "C"
{
#include "common/common.h"
#include "common/lavc.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include "input/sdi/ancillary.h"
#include "input/sdi/vbi.h"
#include "input/sdi/x86/sdi.h"
#include "input/sdi/smpte337_detector.h"
#include <libavresample/avresample.h>
#include <libavutil/opt.h>
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>
}

#include <assert.h>
#include <include/DeckLinkAPI.h>
#include "include/DeckLinkAPIDispatch.cpp"

#ifdef HAVE_LIBKLMONITORING_KLMONITORING_H
#include <libklmonitoring/klmonitoring.h>
static struct kl_histogram frame_interval;
static int histogram_dump = 0;
#endif

#define DECKLINK_VANC_LINES 100

struct obe_to_decklink
{
    int obe_name;
    uint32_t bmd_name;
};

struct obe_to_decklink_video
{
    int obe_name;
    uint32_t bmd_name;
    int timebase_num;
    int timebase_den;
};

const static struct obe_to_decklink video_conn_tab[] =
{
    { INPUT_VIDEO_CONNECTION_SDI,         bmdVideoConnectionSDI },
    { INPUT_VIDEO_CONNECTION_HDMI,        bmdVideoConnectionHDMI },
    { INPUT_VIDEO_CONNECTION_OPTICAL_SDI, bmdVideoConnectionOpticalSDI },
    { INPUT_VIDEO_CONNECTION_COMPONENT,   bmdVideoConnectionComponent },
    { INPUT_VIDEO_CONNECTION_COMPOSITE,   bmdVideoConnectionComposite },
    { INPUT_VIDEO_CONNECTION_S_VIDEO,     bmdVideoConnectionSVideo },
    { -1, 0 },
};

const static struct obe_to_decklink audio_conn_tab[] =
{
    { INPUT_AUDIO_EMBEDDED,               bmdAudioConnectionEmbedded },
    { INPUT_AUDIO_AES_EBU,                bmdAudioConnectionAESEBU },
    { INPUT_AUDIO_ANALOGUE,               bmdAudioConnectionAnalog },
    { -1, 0 },
};

const static struct obe_to_decklink_video video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_PAL,             bmdModePAL,           1,    25 },
    { INPUT_VIDEO_FORMAT_NTSC,            bmdModeNTSC,          1001, 30000 },
    { INPUT_VIDEO_FORMAT_720P_50,         bmdModeHD720p50,      1,    50 },
    { INPUT_VIDEO_FORMAT_720P_5994,       bmdModeHD720p5994,    1001, 60000 },
    { INPUT_VIDEO_FORMAT_720P_60,         bmdModeHD720p60,      1,    60 },
    { INPUT_VIDEO_FORMAT_1080I_50,        bmdModeHD1080i50,     1,    25 },
    { INPUT_VIDEO_FORMAT_1080I_5994,      bmdModeHD1080i5994,   1001, 30000 },
    { INPUT_VIDEO_FORMAT_1080I_60,        bmdModeHD1080i6000,   1,    60 },
    { INPUT_VIDEO_FORMAT_1080P_2398,      bmdModeHD1080p2398,   1001, 24000 },
    { INPUT_VIDEO_FORMAT_1080P_24,        bmdModeHD1080p24,     1,    24 },
    { INPUT_VIDEO_FORMAT_1080P_25,        bmdModeHD1080p25,     1,    25 },
    { INPUT_VIDEO_FORMAT_1080P_2997,      bmdModeHD1080p2997,   1001, 30000 },
    { INPUT_VIDEO_FORMAT_1080P_30,        bmdModeHD1080p30,     1,    30 },
    { INPUT_VIDEO_FORMAT_1080P_50,        bmdModeHD1080p50,     1,    50 },
    { INPUT_VIDEO_FORMAT_1080P_5994,      bmdModeHD1080p5994,   1001, 60000 },
    { INPUT_VIDEO_FORMAT_1080P_60,        bmdModeHD1080p6000,   1,    60 },
    { -1, 0, -1, -1 },
};

#if WRITE_OSD_VALUE || READ_OSD_VALUE
#define  y_white 0x3ff
#define  y_black 0x000
#define cr_white 0x200
#define cb_white 0x200

/* Six pixels */
static uint32_t white[] = {
	 cr_white << 20 |  y_white << 10 | cb_white,
	  y_white << 20 | cb_white << 10 |  y_white,
	 cb_white << 20 |  y_white << 10 | cr_white,
	  y_white << 20 | cr_white << 10 |  y_white,
};

static uint32_t black[] = {
	 cr_white << 20 |  y_black << 10 | cb_white,
	  y_black << 20 | cb_white << 10 |  y_black,
	 cb_white << 20 |  y_black << 10 | cr_white,
	  y_black << 20 | cr_white << 10 |  y_black,
};

/* KL paint 6 pixels in a single point */
__inline__ void V210_draw_6_pixels(uint32_t *addr, uint32_t *coloring)
{
	for (int i = 0; i < 5; i++) {
		addr[0] = coloring[0];
		addr[1] = coloring[1];
		addr[2] = coloring[2];
		addr[3] = coloring[3];
		addr += 4;
	}
}

__inline__ void V210_draw_box(uint32_t *frame_addr, uint32_t stride, int color)
{
	uint32_t *coloring;
	if (color == 1)
		coloring = white;
	else
		coloring = black;

	for (uint32_t l = 0; l < 30; l++) {
		uint32_t *addr = frame_addr + (l * (stride / 4));
		V210_draw_6_pixels(addr, coloring);
	}
}

__inline__ void V210_draw_box_at(uint32_t *frame_addr, uint32_t stride, int color, int x, int y)
{
	uint32_t *addr = frame_addr + (y * (stride / 4));
	addr += ((x / 6) * 4);
	V210_draw_box(addr, stride, color);
}

__inline__ void V210_write_32bit_value(void *frame_bytes, uint32_t stride, uint32_t value, uint32_t lineNr)
{
	for (int p = 31, sh = 0; p >= 0; p--, sh++) {
		V210_draw_box_at(((uint32_t *)frame_bytes), stride,
			(value & (1 << sh)) == (uint32_t)(1 << sh), p * 30, lineNr);
	}
}

__inline__ uint32_t V210_read_32bit_value(void *frame_bytes, uint32_t stride, uint32_t lineNr)
{
	int xpos = 0;
	uint32_t bits = 0;
	for (int i = 0; i < 32; i++) {
		xpos = (i * 30) + 8;
		/* Sample the pixel eight lines deeper than the initial line, and eight pixels in from the left */
		uint32_t *addr = ((uint32_t *)frame_bytes) + ((lineNr + 8) * (stride / 4));
		addr += ((xpos / 6) * 4);

		bits <<= 1;

		/* Sample the pixel.... Compressor will decimate, we'll need a luma threshold for production. */
		if ((addr[1] & 0x3ff) > 0x080)
			bits |= 1;
	}
	return bits;
}
#endif

class DeckLinkCaptureDelegate;

struct audio_pair_s {
    int    nr; /* 0 - 7 */
    struct smpte337_detector_s *smpte337_detector;
    int    smpte337_detected_ac3;
    int    smpte337_frames_written;
    void  *decklink_ctx;
    int    input_stream_id; /* We need this during capture, so we can forward the payload to the right output encoder. */
};

typedef struct
{
    IDeckLink *p_card;
    IDeckLinkInput *p_input;
    DeckLinkCaptureDelegate *p_delegate;

    /* we need to hold onto the IDeckLinkConfiguration object, or our settings will not apply.
       see section 2.4.15 of the blackmagic decklink sdk documentation. */
    IDeckLinkConfiguration *p_config;

    /* Video */
    AVCodec         *dec;
    AVCodecContext  *codec;

    /* Audio - Sample Rate Conversion. We convert S32 interleaved into S32P planer. */
    AVAudioResampleContext *avr;

    int64_t last_frame_time;

    /* VBI */
    int has_setup_vbi;

    /* Ancillary */
    void (*unpack_line) ( uint32_t *src, uint16_t *dst, int width );
    void (*downscale_line) ( uint16_t *src, uint8_t *dst, int lines );
    void (*blank_line) ( uint16_t *dst, int width );
    obe_sdi_non_display_data_t non_display_parser;

    obe_device_t *device;
    obe_t *h;
    BMDDisplayMode enabled_mode_id;

    /* LIBKLVANC handle / context */
    struct vanc_context_s *vanchdl;
#define VANC_CACHE_DUMP_INTERVAL 60
    time_t last_vanc_cache_dump;

    BMDTimeValue stream_time;

    /* SMPTE2038 packetizer */
    struct smpte2038_packetizer_s *smpte2038_ctx;

#if KL_PRBS_INPUT
    struct prbs_context_s prbs;
#endif
#define MAX_AUDIO_PAIRS 8
    struct audio_pair_s audio_pairs[MAX_AUDIO_PAIRS];
} decklink_ctx_t;

typedef struct
{
    decklink_ctx_t decklink_ctx;

    /* Input */
    int card_idx;
    int video_conn;
    int audio_conn;

    int video_format;
    int num_channels;
    int probe;
#define OPTION_ENABLED(opt) (decklink_opts->enable_##opt)
#define OPTION_ENABLED_(opt) (decklink_opts_->enable_##opt)
    int enable_smpte2038;
    int enable_scte35;
    int enable_vanc_cache;
    int enable_bitstream_audio;

    /* Output */
    int probe_success;

    int width;
    int coded_height;
    int height;

    int timebase_num;
    int timebase_den;

    int interlaced;
    int tff;
} decklink_opts_t;

struct decklink_status
{
    obe_input_params_t *input;
    decklink_opts_t *decklink_opts;
};

void kllog(const char *category, const char *format, ...)
{
    char buf[2048] = { 0 };
    struct timeval tv;
    gettimeofday(&tv, 0);

    //sprintf(buf, "%08d.%03d : OBE : ", (unsigned int)tv.tv_sec, (unsigned int)tv.tv_usec / 1000);
    sprintf(buf, "OBE-%s : ", category);

    va_list vl;
    va_start(vl,format);
    vsprintf(&buf[strlen(buf)], format, vl);
    va_end(vl);

    syslog(LOG_INFO | LOG_LOCAL4, "%s", buf);
}

static int transmit_pes_to_muxer(decklink_ctx_t *decklink_ctx, uint8_t *buf, uint32_t byteCount);

/* Take one line of V210 from VANC, colorspace convert and feed it to the
 * VANC parser. We'll expect our VANC message callbacks to happen on this
 * same calling thread.
 */
static void convert_colorspace_and_parse_vanc(decklink_ctx_t *decklink_ctx, struct vanc_context_s *vanchdl, unsigned char *buf, unsigned int uiWidth, unsigned int lineNr)
{
	/* Convert the vanc line from V210 to CrCB422, then vanc parse it */

	/* We need two kinds of type pointers into the source vbi buffer */
	/* TODO: What the hell is this, two ptrs? */
	const uint32_t *src = (const uint32_t *)buf;

	/* Convert Blackmagic pixel format to nv20.
	 * src pointer gets mangled during conversion, hence we need its own
	 * ptr instead of passing vbiBufferPtr.
	 * decoded_words should be atleast 2 * uiWidth.
	 */
	uint16_t decoded_words[16384];

	/* On output each pixel will be decomposed into three 16-bit words (one for Y, U, V) */
	assert(uiWidth * 6 < sizeof(decoded_words));

	memset(&decoded_words[0], 0, sizeof(decoded_words));
	uint16_t *p_anc = decoded_words;
	if (klvanc_v210_line_to_nv20_c(src, p_anc, sizeof(decoded_words), (uiWidth / 6) * 6) < 0)
		return;

    if (decklink_ctx->smpte2038_ctx)
        smpte2038_packetizer_begin(decklink_ctx->smpte2038_ctx);

	if (decklink_ctx->vanchdl) {
		int ret = vanc_packet_parse(vanchdl, lineNr, decoded_words, sizeof(decoded_words) / (sizeof(unsigned short)));
		if (ret < 0) {
      	  /* No VANC on this line */
		}
	}

    if (decklink_ctx->smpte2038_ctx) {
        if (smpte2038_packetizer_end(decklink_ctx->smpte2038_ctx,
                                     decklink_ctx->stream_time / 300) == 0) {

            if (transmit_pes_to_muxer(decklink_ctx, decklink_ctx->smpte2038_ctx->buf,
                                      decklink_ctx->smpte2038_ctx->bufused) < 0) {
                fprintf(stderr, "%s() failed to xmit PES to muxer\n", __func__);
            }
        }
    }

}

static void setup_pixel_funcs( decklink_opts_t *decklink_opts )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts->decklink_ctx;

    int cpu_flags = av_get_cpu_flags();

    /* Setup VBI and VANC unpack functions */
    if( IS_SD( decklink_opts->video_format ) )
    {
        decklink_ctx->unpack_line = obe_v210_line_to_uyvy_c;
        decklink_ctx->downscale_line = obe_downscale_line_c;
        decklink_ctx->blank_line = obe_blank_line_uyvy_c;

        if( cpu_flags & AV_CPU_FLAG_MMX )
            decklink_ctx->downscale_line = obe_downscale_line_mmx;

        if( cpu_flags & AV_CPU_FLAG_SSE2 )
            decklink_ctx->downscale_line = obe_downscale_line_sse2;
    }
    else
    {
        decklink_ctx->unpack_line = obe_v210_line_to_nv20_c;
        decklink_ctx->blank_line = obe_blank_line_nv20_c;
    }
}

static void get_format_opts( decklink_opts_t *decklink_opts, IDeckLinkDisplayMode *p_display_mode )
{
    decklink_opts->width = p_display_mode->GetWidth();
    decklink_opts->coded_height = p_display_mode->GetHeight();

    switch( p_display_mode->GetFieldDominance() )
    {
        case bmdProgressiveFrame:
            decklink_opts->interlaced = 0;
            decklink_opts->tff        = 0;
            break;
        case bmdProgressiveSegmentedFrame:
            /* Assume tff interlaced - this mode should not be used in broadcast */
            decklink_opts->interlaced = 1;
            decklink_opts->tff        = 1;
            break;
        case bmdUpperFieldFirst:
            decklink_opts->interlaced = 1;
            decklink_opts->tff        = 1;
            break;
        case bmdLowerFieldFirst:
            decklink_opts->interlaced = 1;
            decklink_opts->tff        = 0;
            break;
        case bmdUnknownFieldDominance:
        default:
            /* Assume progressive */
            decklink_opts->interlaced = 0;
            decklink_opts->tff        = 0;
            break;
    }

    decklink_opts->height = decklink_opts->coded_height;
    if( decklink_opts->coded_height == 486 )
        decklink_opts->height = 480;
}

class DeckLinkCaptureDelegate : public IDeckLinkInputCallback
{
public:
    DeckLinkCaptureDelegate( decklink_opts_t *decklink_opts ) : decklink_opts_(decklink_opts)
    {
        pthread_mutex_init( &ref_mutex_, NULL );
        pthread_mutex_lock( &ref_mutex_ );
        ref_ = 1;
        pthread_mutex_unlock( &ref_mutex_ );
    }
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }

    virtual ULONG STDMETHODCALLTYPE AddRef(void)
    {
        uintptr_t new_ref;
        pthread_mutex_lock( &ref_mutex_ );
        new_ref = ++ref_;
        pthread_mutex_unlock( &ref_mutex_ );
        return new_ref;
    }

    virtual ULONG STDMETHODCALLTYPE Release(void)
    {
        uintptr_t new_ref;
        pthread_mutex_lock( &ref_mutex_ );
        new_ref = --ref_;
        pthread_mutex_unlock( &ref_mutex_ );
        if ( new_ref == 0 )
            delete this;
        return new_ref;
    }

    virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *p_display_mode, BMDDetectedVideoInputFormatFlags)
    {
        decklink_ctx_t *decklink_ctx = &decklink_opts_->decklink_ctx;
        int i = 0;
        if( events & bmdVideoInputDisplayModeChanged )
        {
            BMDDisplayMode mode_id = p_display_mode->GetDisplayMode();
            syslog( LOG_WARNING, "Video input format changed" );

            if( decklink_ctx->last_frame_time == -1 )
            {
                for( i = 0; video_format_tab[i].obe_name != -1; i++ )
                {
                    if( video_format_tab[i].bmd_name == mode_id )
                        break;
                }

                if( video_format_tab[i].obe_name == -1 )
                {
                    syslog( LOG_WARNING, "Unsupported video format" );
                    return S_OK;
                }

                decklink_opts_->video_format = video_format_tab[i].obe_name;
                decklink_opts_->timebase_num = video_format_tab[i].timebase_num;
                decklink_opts_->timebase_den = video_format_tab[i].timebase_den;

		if (decklink_opts_->video_format == INPUT_VIDEO_FORMAT_1080P_2997)
		{
		   if (p_display_mode->GetFieldDominance() == bmdProgressiveSegmentedFrame)
		   {
		       /* HACK: The transport is structurally interlaced, so we need
			  to treat it as such in order for VANC processing to
			  work properly (even if the actual video really may be
			  progressive).  This also coincidentally works around a bug
			  in VLC where 1080i/59 content gets put out as 1080psf/29, and
			  that's a much more common use case in the broadcast world
			  than real 1080 progressive video at 30 FPS. */
		       fprintf(stderr, "Treating 1080psf/30 as interlaced\n");
		       decklink_opts_->video_format = INPUT_VIDEO_FORMAT_1080I_5994;
		   }
		}

                get_format_opts( decklink_opts_, p_display_mode );
                setup_pixel_funcs( decklink_opts_ );

                decklink_ctx->p_input->PauseStreams();
                decklink_ctx->p_input->EnableVideoInput( p_display_mode->GetDisplayMode(), bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection );
                decklink_ctx->enabled_mode_id = mode_id;
                decklink_ctx->p_input->FlushStreams();
                decklink_ctx->p_input->StartStreams();
            } else {
                syslog(LOG_ERR, "Decklink card index %i: Resolution changed from %08x to %08x, aborting.",
                    decklink_opts_->card_idx, decklink_ctx->enabled_mode_id, mode_id);
                printf("Decklink card index %i: Resolution changed from %08x to %08x, aborting.\n",
                    decklink_opts_->card_idx, decklink_ctx->enabled_mode_id, mode_id);
                //exit(0); /* Take an intensional hard exit */
            }
        }
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);

private:
    pthread_mutex_t ref_mutex_;
    uintptr_t ref_;
    decklink_opts_t *decklink_opts_;
};

static void _vanc_cache_dump(decklink_ctx_t *ctx)
{
    if (ctx->vanchdl)
        return;

    for (int d = 0; d <= 0xff; d++) {
        for (int s = 0; s <= 0xff; s++) {
            struct vanc_cache_s *e = vanc_cache_lookup(ctx->vanchdl, d, s);
            if (!e)
                continue;

            if (e->activeCount == 0)
                continue;

            for (int l = 0; l < 2048; l++) {
                if (e->lines[l].active) {
                    kllog("VANC", "->did/sdid = %02x / %02x: %s [%s] via SDI line %d (%" PRIu64 " packets)\n",
                        e->did, e->sdid, e->desc, e->spec, l, e->lines[l].count);
                }
            }
        }
    }
}

#if KL_PRBS_INPUT
static void dumpAudio(uint16_t *ptr, int fc, int num_channels)
{
        fc = 4;
        uint32_t *p = (uint32_t *)ptr;
        for (int i = 0; i < fc; i++) {
                printf("%d.", i);
                for (int j = 0; j < num_channels; j++)
                        printf("%08x ", *p++);
                printf("\n");
        }
}
static int prbs_inited = 0;
#endif

static int processAudio(decklink_ctx_t *decklink_ctx, decklink_opts_t *decklink_opts_, IDeckLinkAudioInputPacket *audioframe)
{
    obe_raw_frame_t *raw_frame = NULL;
    void *frame_bytes;
    audioframe->GetBytes(&frame_bytes);
    int hasSentAudioBuffer = 0;

        for (int i = 0; i < MAX_AUDIO_PAIRS; i++) {
            struct audio_pair_s *pair = &decklink_ctx->audio_pairs[i];

            if (!pair->smpte337_detected_ac3 && hasSentAudioBuffer == 0) {
                /* PCM audio, forward to compressors */
                raw_frame = new_raw_frame();
                if (!raw_frame) {
                    syslog(LOG_ERR, "Malloc failed\n");
                    goto end;
                }

                raw_frame->audio_frame.num_samples = audioframe->GetSampleFrameCount();
                raw_frame->audio_frame.num_channels = decklink_opts_->num_channels;
                raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;
#if KL_PRBS_INPUT
/* ST: This code is optionally compiled in, and hasn't been validated since we refactored a little. */
            {
            uint32_t *p = (uint32_t *)frame_bytes;
            //dumpAudio((uint16_t *)p, audioframe->GetSampleFrameCount(), raw_frame->audio_frame.num_channels);

            if (prbs_inited == 0) {
                for (int i = 0; i < audioframe->GetSampleFrameCount(); i++) {
                    for (int j = 0; j < raw_frame->audio_frame.num_channels; j++) {
                        if (i == (audioframe->GetSampleFrameCount() - 1)) {
                            if (j == (raw_frame->audio_frame.num_channels - 1)) {
                                printf("Seeding audio PRBS sequence with upstream value 0x%08x\n", *p >> 16);
                                prbs15_init_with_seed(&decklink_ctx->prbs, *p >> 16);
                            }
                        }
			p++;
                    }
                }
                prbs_inited = 1;
            } else {
                for (int i = 0; i < audioframe->GetSampleFrameCount(); i++) {
                    for (int j = 0; j < raw_frame->audio_frame.num_channels; j++) {
                        uint32_t a = *p++ >> 16;
                        uint32_t b = prbs15_generate(&decklink_ctx->prbs);
                        if (a != b) {
                            char t[160];
                            time_t now = time(0);
                            sprintf(t, "%s", ctime(&now));
                            t[strlen(t) - 1] = 0;
                            fprintf(stderr, "%s: KL PRSB15 Audio frame discontinuity, expected %08" PRIx32 " got %08" PRIx32 "\n", t, b, a);
                            prbs_inited = 0;

                            // Break the sample frame loop i
                            i = audioframe->GetSampleFrameCount();
                            break;
                        }
                    }
                }
            }

            }
#endif

                /* Allocate a samples buffer for num_samples samples, and fill data pointers and linesize accordingly. */
                if( av_samples_alloc( raw_frame->audio_frame.audio_data, &raw_frame->audio_frame.linesize, decklink_opts_->num_channels,
                              raw_frame->audio_frame.num_samples, (AVSampleFormat)raw_frame->audio_frame.sample_fmt, 0 ) < 0 )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    return -1;
                }

                /* Convert input samples from S32 interleaved into S32P planer. */
                if (avresample_convert(decklink_ctx->avr,
                        raw_frame->audio_frame.audio_data,
                        raw_frame->audio_frame.linesize,
                        raw_frame->audio_frame.num_samples,
                        (uint8_t**)&frame_bytes,
                        0,
                        raw_frame->audio_frame.num_samples) < 0)
                {
                    syslog( LOG_ERR, "[decklink] Sample format conversion failed\n" );
                    return -1;
                }

                BMDTimeValue packet_time;
                audioframe->GetPacketTime(&packet_time, OBE_CLOCK);
                raw_frame->pts = packet_time;
                raw_frame->release_data = obe_release_audio_data;
                raw_frame->release_frame = obe_release_frame;
                raw_frame->input_stream_id = pair->input_stream_id;
                if (add_to_filter_queue(decklink_ctx->h, raw_frame) < 0)
                    goto fail;
                hasSentAudioBuffer++;

            } /* !pair->smpte337_detected_ac3 */

            if (pair->smpte337_detected_ac3) {

                /* Ship the buffer + offset into it, down to the encoders. The encoders will look at offset 0. */
                int depth = 32;
                int span = 2;
                int offset = i * ((depth / 8) * span);
                raw_frame = new_raw_frame();
                raw_frame->audio_frame.num_samples = audioframe->GetSampleFrameCount();
                raw_frame->audio_frame.num_channels = decklink_opts_->num_channels;
                raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P; /* No specific format. The audio filter will play passthrough. */

                int l = audioframe->GetSampleFrameCount() * decklink_opts_->num_channels * (depth / 8);
                raw_frame->audio_frame.audio_data[0] = (uint8_t *)malloc(l);
                raw_frame->audio_frame.linesize = raw_frame->audio_frame.num_channels * (depth / 8);

                memcpy(raw_frame->audio_frame.audio_data[0], (uint8_t *)frame_bytes + offset, l - offset);

                raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_NONE;

                BMDTimeValue packet_time;
                audioframe->GetPacketTime(&packet_time, OBE_CLOCK);
                raw_frame->pts = packet_time;
                raw_frame->release_data = obe_release_audio_data;
                raw_frame->release_frame = obe_release_frame;
                raw_frame->input_stream_id = pair->input_stream_id;
//printf("frame for pair %d input %d at offset %d\n", pair->nr, raw_frame->input_stream_id, offset);

                add_to_filter_queue(decklink_ctx->h, raw_frame);
            }
        } /* For all audio pairs... */
end:

    return S_OK;

fail:

    if( raw_frame )
    {
        if (raw_frame->release_data)
            raw_frame->release_data( raw_frame );
        if (raw_frame->release_frame)
            raw_frame->release_frame( raw_frame );
    }

    return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived( IDeckLinkVideoInputFrame *videoframe, IDeckLinkAudioInputPacket *audioframe )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts_->decklink_ctx;
    obe_raw_frame_t *raw_frame = NULL;
    AVPacket pkt;
    AVFrame *frame = NULL;
    void *frame_bytes, *anc_line;
    obe_t *h = decklink_ctx->h;
    int finished = 0, ret, num_anc_lines = 0, anc_line_stride,
    lines_read = 0, first_line = 0, last_line = 0, line, num_vbi_lines, vii_line;
    uint32_t *frame_ptr;
    uint16_t *anc_buf, *anc_buf_pos;
    uint8_t *vbi_buf;
    int anc_lines[DECKLINK_VANC_LINES];
    IDeckLinkVideoFrameAncillary *ancillary;
    BMDTimeValue frame_duration;

    if( decklink_opts_->probe_success )
        return S_OK;

    if (OPTION_ENABLED_(vanc_cache)) {
        if (decklink_ctx->last_vanc_cache_dump + VANC_CACHE_DUMP_INTERVAL <= time(0)) {
            decklink_ctx->last_vanc_cache_dump = time(0);
            _vanc_cache_dump(decklink_ctx);
        }
    }


    av_init_packet( &pkt );

    if( videoframe )
    {
#ifdef HAVE_LIBKLMONITORING_KLMONITORING_H
        kl_histogram_update(&frame_interval);
        if (histogram_dump++ > 240) {
                histogram_dump = 0;
#if PRINT_HISTOGRAMS
                kl_histogram_printf(&frame_interval);
#endif
        }
#endif

        if( videoframe->GetFlags() & bmdFrameHasNoInputSource )
        {
            syslog( LOG_ERR, "Decklink card index %i: No input signal detected", decklink_opts_->card_idx );
            return S_OK;
        }
        else if (decklink_opts_->probe && decklink_ctx->audio_pairs[0].smpte337_frames_written > 6)
            decklink_opts_->probe_success = 1;

        /* use SDI ticks as clock source */
        videoframe->GetStreamTime(&decklink_ctx->stream_time, &frame_duration, OBE_CLOCK);
        obe_clock_tick(h, (int64_t)decklink_ctx->stream_time);

        if( decklink_ctx->last_frame_time == -1 )
            decklink_ctx->last_frame_time = obe_mdate();
        else
        {
            int64_t cur_frame_time = obe_mdate();
            if( cur_frame_time - decklink_ctx->last_frame_time >= SDI_MAX_DELAY )
            {
                syslog( LOG_WARNING, "Decklink card index %i: No frame received for %"PRIi64" ms", decklink_opts_->card_idx,
                       (cur_frame_time - decklink_ctx->last_frame_time) / 1000 );
                pthread_mutex_lock( &h->drop_mutex );
                h->encoder_drop = h->mux_drop = 1;
                pthread_mutex_unlock( &h->drop_mutex );
            }

            decklink_ctx->last_frame_time = cur_frame_time;
        }

        const int width = videoframe->GetWidth();
        const int height = videoframe->GetHeight();
        const int stride = videoframe->GetRowBytes();

        videoframe->GetBytes( &frame_bytes );

#if WRITE_OSD_VALUE
	static uint32_t xxx = 0;
	V210_write_32bit_value(frame_bytes, stride, xxx++, 100);
#endif
#if READ_OSD_VALUE
	{
		static uint32_t xxx = 0;
		uint32_t val = V210_read_32bit_value(frame_bytes, stride, 210);
		if (xxx + 1 != val) {
                        char t[160];
                        time_t now = time(0);
                        sprintf(t, "%s", ctime(&now));
                        t[strlen(t) - 1] = 0;
                        fprintf(stderr, "%s: KL OSD counter discontinuity, expected %08" PRIx32 " got %08" PRIx32 "\n", t, xxx + 1, val);
		}
		xxx = val;
	}
#endif

        /* TODO: support format switching (rare in SDI) */
        int j;
        for( j = 0; first_active_line[j].format != -1; j++ )
        {
            if( decklink_opts_->video_format == first_active_line[j].format )
                break;
        }

        videoframe->GetAncillaryData( &ancillary );

        /* NTSC starts on line 4 */
        line = decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC ? 4 : 1;
        anc_line_stride = FFALIGN( (width * 2 * sizeof(uint16_t)), 16 );

        /* Overallocate slightly for VANC buffer
         * Some VBI services stray into the active picture so allocate some extra space */
        anc_buf = anc_buf_pos = (uint16_t*)av_malloc( DECKLINK_VANC_LINES * anc_line_stride );
        if( !anc_buf )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto end;
        }

        while( 1 )
        {
            /* Some cards have restrictions on what lines can be accessed so try them all
             * Some buggy decklink cards will randomly refuse access to a particular line so
             * work around this issue by blanking the line */
            if( ancillary->GetBufferForVerticalBlankingLine( line, &anc_line ) == S_OK ) {

                /* Give libklvanc a chance to parse all vanc, and call our callbacks (same thread) */
                convert_colorspace_and_parse_vanc(decklink_ctx, decklink_ctx->vanchdl,
                                                  (unsigned char *)anc_line, width, line);

                decklink_ctx->unpack_line( (uint32_t*)anc_line, anc_buf_pos, width );
            } else
                decklink_ctx->blank_line( anc_buf_pos, width );

            anc_buf_pos += anc_line_stride / 2;
            anc_lines[num_anc_lines++] = line;

            if( !first_line )
                first_line = line;
            last_line = line;

            lines_read++;
            line = sdi_next_line( decklink_opts_->video_format, line );

            if( line == first_active_line[j].line )
                break;
        }

        ancillary->Release();

        if( !decklink_opts_->probe )
        {
            raw_frame = new_raw_frame();
            if( !raw_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }
        }

        anc_buf_pos = anc_buf;
        for( int i = 0; i < num_anc_lines; i++ )
        {
            parse_vanc_line( h, &decklink_ctx->non_display_parser, raw_frame, anc_buf_pos, width, anc_lines[i] );
            anc_buf_pos += anc_line_stride / 2;
        }

        if( IS_SD( decklink_opts_->video_format ) && first_line != last_line )
        {
            /* Add a some VBI lines to the ancillary buffer */
            frame_ptr = (uint32_t*)frame_bytes;

            /* NTSC starts from line 283 so add an extra line */
            num_vbi_lines = NUM_ACTIVE_VBI_LINES + ( decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC );
            for( int i = 0; i < num_vbi_lines; i++ )
            {
                decklink_ctx->unpack_line( frame_ptr, anc_buf_pos, width );
                anc_buf_pos += anc_line_stride / 2;
                frame_ptr += stride / 4;
                last_line = sdi_next_line( decklink_opts_->video_format, last_line );
            }
            num_anc_lines += num_vbi_lines;

            vbi_buf = (uint8_t*)av_malloc( width * 2 * num_anc_lines );
            if( !vbi_buf )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }

            /* Scale the lines from 10-bit to 8-bit */
            decklink_ctx->downscale_line( anc_buf, vbi_buf, num_anc_lines );
            anc_buf_pos = anc_buf;

            /* Handle Video Index information */
            int tmp_line = first_line;
            vii_line = decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC ? NTSC_VIDEO_INDEX_LINE : PAL_VIDEO_INDEX_LINE;
            while( tmp_line < vii_line )
            {
                anc_buf_pos += anc_line_stride / 2;
                tmp_line++;
            }

            if( decode_video_index_information( h, &decklink_ctx->non_display_parser, anc_buf_pos, raw_frame, vii_line ) < 0 )
                goto fail;

            if( !decklink_ctx->has_setup_vbi )
            {
                vbi_raw_decoder_init( &decklink_ctx->non_display_parser.vbi_decoder );

                decklink_ctx->non_display_parser.ntsc = decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC;
                decklink_ctx->non_display_parser.vbi_decoder.start[0] = first_line;
                decklink_ctx->non_display_parser.vbi_decoder.start[1] = sdi_next_line( decklink_opts_->video_format, first_line );
                decklink_ctx->non_display_parser.vbi_decoder.count[0] = last_line - decklink_ctx->non_display_parser.vbi_decoder.start[1] + 1;
                decklink_ctx->non_display_parser.vbi_decoder.count[1] = decklink_ctx->non_display_parser.vbi_decoder.count[0];

                if( setup_vbi_parser( &decklink_ctx->non_display_parser ) < 0 )
                    goto fail;

                decklink_ctx->has_setup_vbi = 1;
            }

            if( decode_vbi( h, &decklink_ctx->non_display_parser, vbi_buf, raw_frame ) < 0 )
                goto fail;

            av_free( vbi_buf );
        }

        av_free( anc_buf );

        if( !decklink_opts_->probe )
        {
            frame = avcodec_alloc_frame();
            if( !frame )
            {
                syslog( LOG_ERR, "[decklink]: Could not allocate video frame\n" );
                goto end;
            }
            decklink_ctx->codec->width = width;
            decklink_ctx->codec->height = height;

            pkt.data = (uint8_t*)frame_bytes;
            pkt.size = stride * height;

            ret = avcodec_decode_video2( decklink_ctx->codec, frame, &finished, &pkt );
            if( ret < 0 || !finished )
            {
                syslog( LOG_ERR, "[decklink]: Could not decode video frame\n" );
                goto end;
            }

            raw_frame->release_data = obe_release_video_data;
            raw_frame->release_frame = obe_release_frame;

            memcpy( raw_frame->alloc_img.stride, frame->linesize, sizeof(raw_frame->alloc_img.stride) );
            memcpy( raw_frame->alloc_img.plane, frame->data, sizeof(raw_frame->alloc_img.plane) );
            avcodec_free_frame( &frame );
            raw_frame->alloc_img.csp = (int)decklink_ctx->codec->pix_fmt;
            raw_frame->alloc_img.planes = av_pix_fmt_descriptors[raw_frame->alloc_img.csp].nb_components;
            raw_frame->alloc_img.width = width;
            raw_frame->alloc_img.height = height;
            raw_frame->alloc_img.format = decklink_opts_->video_format;
            raw_frame->timebase_num = decklink_opts_->timebase_num;
            raw_frame->timebase_den = decklink_opts_->timebase_den;

            memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->alloc_img) );
//PRINT_OBE_IMAGE(&raw_frame->img      , "      DECK->img");
//PRINT_OBE_IMAGE(&raw_frame->alloc_img, "DECK->alloc_img");
            if( IS_SD( decklink_opts_->video_format ) )
            {
                raw_frame->img.first_line = first_active_line[j].line;
                if( decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC )
                {
                    raw_frame->img.height = 480;
                    while( raw_frame->img.first_line != NTSC_FIRST_CODED_LINE )
                    {
                        for( int i = 0; i < raw_frame->img.planes; i++ )
                            raw_frame->img.plane[i] += raw_frame->img.stride[i];

                        raw_frame->img.first_line = sdi_next_line( INPUT_VIDEO_FORMAT_NTSC, raw_frame->img.first_line );
                    }
                }
            }

            /* If AFD is present and the stream is SD this will be changed in the video filter */
            raw_frame->sar_width = raw_frame->sar_height = 1;
            raw_frame->pts = decklink_ctx->stream_time;

            for( int i = 0; i < decklink_ctx->device->num_input_streams; i++ )
            {
                if( decklink_ctx->device->streams[i]->stream_format == VIDEO_UNCOMPRESSED )
                    raw_frame->input_stream_id = decklink_ctx->device->streams[i]->input_stream_id;
            }

            if( add_to_filter_queue( h, raw_frame ) < 0 )
                goto fail;

            if( send_vbi_and_ttx( h, &decklink_ctx->non_display_parser, raw_frame->pts ) < 0 )
                goto fail;

            decklink_ctx->non_display_parser.num_vbi = 0;
            decklink_ctx->non_display_parser.num_anc_vbi = 0;
        }
    } /* if video frame */

    if (audioframe) {
        if(OPTION_ENABLED_(bitstream_audio)) {
            for (int i = 0; i < MAX_AUDIO_PAIRS; i++) {
                audioframe->GetBytes(&frame_bytes);

                /* Look for bitstream in audio channels 0 and 1 */
                /* TODO: Examine other channels. */
                /* TODO: Kinda pointless caching a successful find, because those
                 * values held in decklink_ctx are thrown away when the probe completes. */
                int depth = 32;
                int span = 2;
                struct audio_pair_s *pair = &decklink_ctx->audio_pairs[i];
                if (pair->smpte337_detector) {
                    pair->smpte337_frames_written++;

                    /* Figure out the offset in the line, where this channel pair begins. */
                    int offset = i * ((depth / 8) * span);
                    smpte337_detector_write(pair->smpte337_detector, (uint8_t *)frame_bytes + offset,
                        audioframe->GetSampleFrameCount(),
                        depth,
                        decklink_opts_->num_channels,
                        decklink_opts_->num_channels * (depth / 8),
                        span);

                }
            }
        }
    }

    if( audioframe && !decklink_opts_->probe )
    {
        processAudio(decklink_ctx, decklink_opts_, audioframe);
    }

end:
    if( frame )
        avcodec_free_frame( &frame );

    av_free_packet( &pkt );

    return S_OK;

fail:

    if( raw_frame )
    {
        if (raw_frame->release_data)
            raw_frame->release_data( raw_frame );
        if (raw_frame->release_frame)
            raw_frame->release_frame( raw_frame );
    }

    return S_OK;
}

static void close_card( decklink_opts_t *decklink_opts )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts->decklink_ctx;

    if (decklink_ctx->vanchdl) {
        vanc_context_destroy(decklink_ctx->vanchdl);
        decklink_ctx->vanchdl = 0;
    }

    if (decklink_ctx->smpte2038_ctx) {
        smpte2038_packetizer_free(&decklink_ctx->smpte2038_ctx);
        decklink_ctx->smpte2038_ctx = 0;
    }

    for (int i = 0; i < MAX_AUDIO_PAIRS; i++) {
        struct audio_pair_s *pair = &decklink_ctx->audio_pairs[i];
        if (pair->smpte337_detector) {
            smpte337_detector_free(pair->smpte337_detector);
            pair->smpte337_detector = 0;
        }
    }

    if( decklink_ctx->p_config )
        decklink_ctx->p_config->Release();

    if( decklink_ctx->p_input )
    {
        decklink_ctx->p_input->StopStreams();
        decklink_ctx->p_input->Release();
    }

    if( decklink_ctx->p_card )
        decklink_ctx->p_card->Release();

    if( decklink_ctx->p_delegate )
        decklink_ctx->p_delegate->Release();

    if( decklink_ctx->codec )
    {
        avcodec_close( decklink_ctx->codec );
        av_free( decklink_ctx->codec );
    }

    if( IS_SD( decklink_opts->video_format ) )
        vbi_raw_decoder_destroy( &decklink_ctx->non_display_parser.vbi_decoder );

    if( decklink_ctx->avr )
        avresample_free( &decklink_ctx->avr );

}

/* VANC Callbacks */
static int cb_PAYLOAD_INFORMATION(void *callback_context, struct vanc_context_s *ctx, struct packet_payload_information_s *pkt)
{
	decklink_ctx_t *decklink_ctx = (decklink_ctx_t *)callback_context;
	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		dump_PAYLOAD_INFORMATION(ctx, pkt); /* vanc lib helper */
	}

	return 0;
}

static int cb_EIA_708B(void *callback_context, struct vanc_context_s *ctx, struct packet_eia_708b_s *pkt)
{
	decklink_ctx_t *decklink_ctx = (decklink_ctx_t *)callback_context;
	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		dump_EIA_708B(ctx, pkt); /* vanc lib helper */
	}

	return 0;
}

static int cb_EIA_608(void *callback_context, struct vanc_context_s *ctx, struct packet_eia_608_s *pkt)
{
	decklink_ctx_t *decklink_ctx = (decklink_ctx_t *)callback_context;
	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		dump_EIA_608(ctx, pkt); /* vanc library helper */
	}

	return 0;
}

static int findOutputStreamIdByFormat(decklink_ctx_t *decklink_ctx, enum stream_type_e stype, enum stream_formats_e fmt)
{
	if (decklink_ctx && decklink_ctx->device == NULL)
		return -1;

	for(int i = 0; i < decklink_ctx->device->num_input_streams; i++) {
		if ((decklink_ctx->device->streams[i]->stream_type == stype) &&
			(decklink_ctx->device->streams[i]->stream_format == fmt))
			return i;
        }

	return -1;
}
 
static int transmit_scte35_section_to_muxer(decklink_ctx_t *decklink_ctx, uint8_t *section, uint32_t section_length)
{
	int streamId = findOutputStreamIdByFormat(decklink_ctx, STREAM_TYPE_MISC, DVB_TABLE_SECTION);
	if (streamId < 0)
		return 0;

	/* Now send the constructed frame to the mux */
	obe_coded_frame_t *coded_frame = new_coded_frame(streamId, section_length);
	if (!coded_frame) {
		syslog(LOG_ERR, "Malloc failed during %s, needed %d bytes\n", __func__, section_length);
		return -1;
	}
	coded_frame->pts = decklink_ctx->stream_time;
	coded_frame->random_access = 1; /* ? */
	memcpy(coded_frame->data, section, section_length);
	add_to_queue(&decklink_ctx->h->mux_queue, coded_frame);

	return 0;
}

static int transmit_pes_to_muxer(decklink_ctx_t *decklink_ctx, uint8_t *buf, uint32_t byteCount)
{
	int streamId = findOutputStreamIdByFormat(decklink_ctx, STREAM_TYPE_MISC, SMPTE2038);
	if (streamId < 0)
		return 0;

	/* Now send the constructed frame to the mux */
	obe_coded_frame_t *coded_frame = new_coded_frame(streamId, byteCount);
	if (!coded_frame) {
		syslog(LOG_ERR, "Malloc failed during %s, needed %d bytes\n", __func__, byteCount);
		return -1;
	}
	coded_frame->pts = decklink_ctx->stream_time;
	coded_frame->random_access = 1; /* ? */
	memcpy(coded_frame->data, buf, byteCount);
	add_to_queue(&decklink_ctx->h->mux_queue, coded_frame);

	return 0;
}

static int cb_SCTE_104(void *callback_context, struct vanc_context_s *ctx, struct packet_scte_104_s *pkt)
{
	/* It should be impossible to get here until the user has asked to enable SCTE35 */

	decklink_ctx_t *decklink_ctx = (decklink_ctx_t *)callback_context;
	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		dump_SCTE_104(ctx, pkt); /* vanc library helper */
	}

	if (vanc_packetType1(&pkt->hdr)) {
		/* Silently discard type 1 SCTE104 packets, as per SMPTE 291 section 6.3 */
		return 0;
	}

	struct single_operation_message *m = &pkt->so_msg;

	if (m->opID == 0xFFFF /* Multiple Operation Message */) {
		struct splice_entries results;
		/* Note, we add 10 second to the PTS to compensate for TS_START added by libmpegts */
		int r = scte35_generate_from_scte104(pkt, &results,
						     decklink_ctx->stream_time / 300 + (10 * 90000));
		if (r != 0) {
			fprintf(stderr, "Generation of SCTE-35 sections failed\n");
		}

		for (size_t i = 0; i < results.num_splices; i++) {
			transmit_scte35_section_to_muxer(decklink_ctx, results.splice_entry[i],
							 results.splice_size[i]);
			free(results.splice_entry[i]);
		}
	} else {
		/* Unsupported single_operation_message type */
	}

	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_SCTE104) {
		static time_t lastErrTime = 0;
		time_t now = time(0);
		if (lastErrTime != now) {
			lastErrTime = now;

			char t[64];
			sprintf(t, "%s", ctime(&now));
			t[ strlen(t) - 1] = 0;
			syslog(LOG_INFO, "[decklink] SCTE104 frames present");
			fprintf(stdout, "[decklink] SCTE104 frames present  @ %s", t);
			printf("\n");
			fflush(stdout);

		}
	}

	return 0;
}

static int cb_all(void *callback_context, struct vanc_context_s *ctx, struct packet_header_s *pkt)
{
	decklink_ctx_t *decklink_ctx = (decklink_ctx_t *)callback_context;
	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s()\n", __func__);
	}

	/* We've been called with a VANC frame. Pass it to the SMPTE2038 packetizer.
	 * We'll be called here from the thread handing the VideoFrameArrived
	 * callback, which calls vanc_packet_parse for each ANC line.
	 * Push the pkt into the SMPTE2038 layer, its collecting VANC data.
	 */
	if (decklink_ctx->smpte2038_ctx) {
		if (smpte2038_packetizer_append(decklink_ctx->smpte2038_ctx, pkt) < 0) {
		}
	}

	return 0;
}

static int cb_VANC_TYPE_KL_UINT64_COUNTER(void *callback_context, struct vanc_context_s *ctx, struct packet_kl_u64le_counter_s *pkt)
{
        /* Have the library display some debug */
	static uint64_t lastGoodKLFrameCounter = 0;
        if (lastGoodKLFrameCounter && lastGoodKLFrameCounter + 1 != pkt->counter) {
                char t[160];
                time_t now = time(0);
                sprintf(t, "%s", ctime(&now));
                t[strlen(t) - 1] = 0;

                fprintf(stderr, "%s: KL VANC frame counter discontinuity was %" PRIu64 " now %" PRIu64 "\n",
                        t,
                        lastGoodKLFrameCounter, pkt->counter);
        }
        lastGoodKLFrameCounter = pkt->counter;

        return 0;
}

static struct vanc_callbacks_s callbacks = 
{
	.payload_information	= cb_PAYLOAD_INFORMATION,
	.eia_708b		= cb_EIA_708B,
	.eia_608		= cb_EIA_608,
	.scte_104		= cb_SCTE_104,
	.all			= cb_all,
	.kl_i64le_counter       = cb_VANC_TYPE_KL_UINT64_COUNTER,
};
/* End: VANC Callbacks */

static void * detector_callback(void *user_context,
        struct smpte337_detector_s *ctx,
        uint8_t datamode, uint8_t datatype, uint32_t payload_bitCount, uint8_t *payload)
{
	struct audio_pair_s *pair = (struct audio_pair_s *)user_context;
#if 0
	decklink_ctx_t *decklink_ctx = pair->decklink_ctx;
        printf("%s() datamode = %d [%sbit], datatype = %d [payload: %s]"
                ", payload_bitcount = %d, payload = %p\n",
                __func__,
                datamode,
                datamode == 0 ? "16" :
                datamode == 1 ? "20" :
                datamode == 2 ? "24" : "Reserved",
                datatype,
                datatype == 1 ? "SMPTE338 / AC-3 (audio) data" : "TBD",
                payload_bitCount,
		payload);
#endif

	if (datatype == 1 /* AC3 */) {
		pair->smpte337_detected_ac3 = 1;
	} else
		fprintf(stderr, "[decklink] Detected datamode %d on pair %d, we don't support it.",
			pair->nr,
			datamode);

        return 0;
}

static int open_card( decklink_opts_t *decklink_opts )
{
#ifdef HAVE_LIBKLMONITORING_KLMONITORING_H
    kl_histogram_reset(&frame_interval, "video frame intervals", KL_BUCKET_VIDEO);
#endif
    decklink_ctx_t *decklink_ctx = &decklink_opts->decklink_ctx;
    int         found_mode;
    int         ret = 0;
    int         i;
    const int   sample_rate = 48000;
    const char *model_name;
    BMDDisplayMode wanted_mode_id;
    IDeckLinkAttributes *decklink_attributes = NULL;
    uint32_t    flags = 0;
    bool        supported;

    IDeckLinkDisplayModeIterator *p_display_iterator = NULL;
    IDeckLinkIterator *decklink_iterator = NULL;
    HRESULT result;

    if (vanc_context_create(&decklink_ctx->vanchdl) < 0) {
        fprintf(stderr, "[decklink] Error initializing VANC library context\n");
    } else {
        decklink_ctx->vanchdl->verbose = 0;
        decklink_ctx->vanchdl->callbacks = &callbacks;
        decklink_ctx->vanchdl->callback_context = decklink_ctx;
        decklink_ctx->last_vanc_cache_dump = 0;

        if (OPTION_ENABLED(vanc_cache)) {
            /* Turn on the vanc cache, we'll want to query it later. */
            decklink_ctx->last_vanc_cache_dump = 1;
            fprintf(stdout, "Enabling option VANC CACHE, interval %d seconds\n", VANC_CACHE_DUMP_INTERVAL);
            vanc_context_enable_cache(decklink_ctx->vanchdl);
        }
    }

    if (OPTION_ENABLED(scte35)) {
        fprintf(stdout, "Enabling option SCTE35\n");
    } else
	callbacks.scte_104 = NULL;

    if (OPTION_ENABLED(smpte2038)) {
        fprintf(stdout, "Enabling option SMPTE2038\n");
        if (smpte2038_packetizer_alloc(&decklink_ctx->smpte2038_ctx) < 0) {
            fprintf(stderr, "Unable to allocate a SMPTE2038 context.\n");
        }
    } else
	callbacks.all = NULL;

    for (int i = 0; i < MAX_AUDIO_PAIRS; i++) {
        struct audio_pair_s *pair = &decklink_ctx->audio_pairs[i];

        pair->nr = i;
        pair->smpte337_detected_ac3 = 0;
        pair->decklink_ctx = decklink_ctx;
        pair->input_stream_id = i + 1; /* Video is zero, audio onwards. */

        if (OPTION_ENABLED(bitstream_audio)) {
            pair->smpte337_detector = smpte337_detector_alloc((smpte337_detector_callback)detector_callback, pair);
        } else {
            pair->smpte337_frames_written = 256;
        }
    }

#if 1
#pragma message "SCTE104 verbose debugging enabled."
    decklink_ctx->h->verbose_bitmask = INPUTSOURCE__SDI_VANC_DISCOVERY_SCTE104;
#endif

    avcodec_register_all();
    decklink_ctx->dec = avcodec_find_decoder( AV_CODEC_ID_V210 );
    if( !decklink_ctx->dec )
    {
        fprintf( stderr, "[decklink] Could not find v210 decoder\n" );
        goto finish;
    }

    decklink_ctx->codec = avcodec_alloc_context3( decklink_ctx->dec );
    if( !decklink_ctx->codec )
    {
        fprintf( stderr, "[decklink] Could not allocate AVCodecContext\n" );
        goto finish;
    }

    decklink_ctx->codec->get_buffer = obe_get_buffer;
    decklink_ctx->codec->release_buffer = obe_release_buffer;
    decklink_ctx->codec->reget_buffer = obe_reget_buffer;
    decklink_ctx->codec->flags |= CODEC_FLAG_EMU_EDGE;

    /* TODO: setup custom strides */
    if( avcodec_open2( decklink_ctx->codec, decklink_ctx->dec, NULL ) < 0 )
    {
        fprintf( stderr, "[decklink] Could not open libavcodec\n" );
        goto finish;
    }

    decklink_iterator = CreateDeckLinkIteratorInstance();
    if( !decklink_iterator )
    {
        fprintf( stderr, "[decklink] DeckLink drivers not found\n" );
        ret = -1;
        goto finish;
    }

    if( decklink_opts->card_idx < 0 )
    {
        fprintf( stderr, "[decklink] Invalid card index %d \n", decklink_opts->card_idx );
        ret = -1;
        goto finish;
    }

    for( i = 0; i <= decklink_opts->card_idx; ++i )
    {
        if( decklink_ctx->p_card )
            decklink_ctx->p_card->Release();
        result = decklink_iterator->Next( &decklink_ctx->p_card );
        if( result != S_OK )
            break;
    }

    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] DeckLink PCI card %d not found\n", decklink_opts->card_idx );
        ret = -1;
        goto finish;
    }

    result = decklink_ctx->p_card->GetModelName( &model_name );

    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Could not get model name\n" );
        ret = -1;
        goto finish;
    }

    syslog( LOG_INFO, "Opened DeckLink PCI card %d (%s)", decklink_opts->card_idx, model_name );
    free( (char *)model_name );

    if( decklink_ctx->p_card->QueryInterface( IID_IDeckLinkInput, (void**)&decklink_ctx->p_input ) != S_OK )
    {
        fprintf( stderr, "[decklink] Card has no inputs\n" );
        ret = -1;
        goto finish;
    }

    /* Set up the video and audio sources. */
    if( decklink_ctx->p_card->QueryInterface( IID_IDeckLinkConfiguration, (void**)&decklink_ctx->p_config ) != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to get configuration interface\n" );
        ret = -1;
        goto finish;
    }

    /* Setup video connection */
    for( i = 0; video_conn_tab[i].obe_name != -1; i++ )
    {
        if( video_conn_tab[i].obe_name == decklink_opts->video_conn )
            break;
    }

    if( video_conn_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[decklink] Unsupported video input connection\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_ctx->p_config->SetInt( bmdDeckLinkConfigVideoInputConnection, video_conn_tab[i].bmd_name );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to set video input connection\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_ctx->p_card->QueryInterface(IID_IDeckLinkAttributes, (void**)&decklink_attributes );
    if( result != S_OK )
    {
        fprintf(stderr, "[decklink] Could not obtain the IDeckLinkAttributes interface\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_attributes->GetFlag( BMDDeckLinkSupportsInputFormatDetection, &supported );
    if( result != S_OK )
    {
        fprintf(stderr, "[decklink] Could not query card for format detection\n" );
        ret = -1;
        goto finish;
    }

    if( supported )
        flags = bmdVideoInputEnableFormatDetection;

    /* Get the list of display modes. */
    result = decklink_ctx->p_input->GetDisplayModeIterator( &p_display_iterator );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enumerate display modes\n" );
        ret = -1;
        goto finish;
    }

    for( i = 0; video_format_tab[i].obe_name != -1; i++ )
    {
        if( video_format_tab[i].obe_name == decklink_opts->video_format )
            break;
    }

    if( video_format_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[decklink] Unsupported video format\n" );
        ret = -1;
        goto finish;
    }

    wanted_mode_id = video_format_tab[i].bmd_name;
    found_mode = false;
    decklink_opts->timebase_num = video_format_tab[i].timebase_num;
    decklink_opts->timebase_den = video_format_tab[i].timebase_den;

    for (;;)
    {
        IDeckLinkDisplayMode *p_display_mode;
        result = p_display_iterator->Next( &p_display_mode );
        if( result != S_OK || !p_display_mode )
            break;

        BMDDisplayMode mode_id = p_display_mode->GetDisplayMode();

        BMDTimeValue frame_duration, time_scale;
        result = p_display_mode->GetFrameRate( &frame_duration, &time_scale );
        if( result != S_OK )
        {
            fprintf( stderr, "[decklink] Failed to get frame rate\n" );
            ret = -1;
            p_display_mode->Release();
            goto finish;
        }

        if( wanted_mode_id == mode_id )
        {
            found_mode = true;
            get_format_opts( decklink_opts, p_display_mode );
            setup_pixel_funcs( decklink_opts );
        }

        p_display_mode->Release();
    }

    if( !found_mode )
    {
        fprintf( stderr, "[decklink] Unsupported video mode\n" );
        ret = -1;
        goto finish;
    }

    /* Setup audio connection */
    for( i = 0; audio_conn_tab[i].obe_name != -1; i++ )
    {
        if( audio_conn_tab[i].obe_name == decklink_opts->audio_conn )
            break;
    }

    if( audio_conn_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[decklink] Unsupported audio input connection\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_ctx->p_config->SetInt( bmdDeckLinkConfigAudioInputConnection, audio_conn_tab[i].bmd_name );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to set audio input connection\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_ctx->p_input->EnableVideoInput( wanted_mode_id, bmdFormat10BitYUV, flags );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enable video input\n" );
        ret = -1;
        goto finish;
    }
    decklink_ctx->enabled_mode_id = wanted_mode_id;

    /* Set up audio. */
    result = decklink_ctx->p_input->EnableAudioInput( sample_rate, bmdAudioSampleType32bitInteger, decklink_opts->num_channels );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enable audio input\n" );
        ret = -1;
        goto finish;
    }

    if( !decklink_opts->probe )
    {
        decklink_ctx->avr = avresample_alloc_context();
        if( !decklink_ctx->avr )
        {
            fprintf( stderr, "[decklink-sdiaudio] couldn't setup sample rate conversion \n" );
            ret = -1;
            goto finish;
        }

        /* Give libavresample a made up channel map */
        av_opt_set_int( decklink_ctx->avr, "in_channel_layout",   (1 << decklink_opts->num_channels) - 1, 0 );
        av_opt_set_int( decklink_ctx->avr, "in_sample_fmt",       AV_SAMPLE_FMT_S32, 0 );
        av_opt_set_int( decklink_ctx->avr, "in_sample_rate",      48000, 0 );
        av_opt_set_int( decklink_ctx->avr, "out_channel_layout",  (1 << decklink_opts->num_channels) - 1, 0 );
        av_opt_set_int( decklink_ctx->avr, "out_sample_fmt",      AV_SAMPLE_FMT_S32P, 0 );

        if( avresample_open( decklink_ctx->avr ) < 0 )
        {
            fprintf( stderr, "Could not open AVResample\n" );
            goto finish;
        }
    }

    decklink_ctx->p_delegate = new DeckLinkCaptureDelegate( decklink_opts );
    decklink_ctx->p_input->SetCallback( decklink_ctx->p_delegate );

    result = decklink_ctx->p_input->StartStreams();
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Could not start streaming from card\n" );
        ret = -1;
        goto finish;
    }

    ret = 0;

finish:
    if( decklink_iterator )
        decklink_iterator->Release();

    if( p_display_iterator )
        p_display_iterator->Release();

    if( decklink_attributes )
        decklink_attributes->Release();

    if( ret )
        close_card( decklink_opts );

    return ret;
}

static void close_thread( void *handle )
{
    struct decklink_status *status = (decklink_status *)handle;

    if( status->decklink_opts )
    {
        close_card( status->decklink_opts );
        free( status->decklink_opts );
    }

    free( status->input );
}

static void *probe_stream( void *ptr )
{
    obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;
    obe_device_t *device;
    obe_int_input_stream_t *streams[MAX_STREAMS];
    int cur_stream = 0;
    obe_sdi_non_display_data_t *non_display_parser;
    decklink_ctx_t *decklink_ctx;

    decklink_opts_t *decklink_opts = (decklink_opts_t*)calloc( 1, sizeof(*decklink_opts) );
    if( !decklink_opts )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    non_display_parser = &decklink_opts->decklink_ctx.non_display_parser;

    /* TODO: support multi-channel */
    decklink_opts->num_channels = 16;
    decklink_opts->card_idx = user_opts->card_idx;
    decklink_opts->video_conn = user_opts->video_connection;
    decklink_opts->audio_conn = user_opts->audio_connection;
    decklink_opts->video_format = user_opts->video_format;
    decklink_opts->enable_smpte2038 = user_opts->enable_smpte2038;
    decklink_opts->enable_scte35 = user_opts->enable_scte35;
    decklink_opts->enable_vanc_cache = user_opts->enable_vanc_cache;
    decklink_opts->enable_bitstream_audio = user_opts->enable_bitstream_audio;

    decklink_opts->probe = non_display_parser->probe = 1;

    decklink_ctx = &decklink_opts->decklink_ctx;
    decklink_ctx->h = h;
    decklink_ctx->last_frame_time = -1;

    if( open_card( decklink_opts ) < 0 )
        goto finish;

    /* Wait for up to 10 seconds, checking for a probe success every 100ms.
     * Avoid issues with some links where probe takes an unusually long time.
     */
    for (int z = 0; z < 10 * 10; z++) {
        usleep(100 * 1000);
        if (decklink_opts->probe_success)
            break;
    }

    close_card( decklink_opts );

    if( !decklink_opts->probe_success )
    {
        fprintf( stderr, "[decklink] No valid frames received - check connection and input format\n" );
        goto finish;
    }

#define ALLOC_STREAM(nr) \
    streams[cur_stream] = (obe_int_input_stream_t*)calloc(1, sizeof(*streams[cur_stream])); \
    if (!streams[cur_stream]) goto finish;

    ALLOC_STREAM(cur_stream]);
    pthread_mutex_lock(&h->device_list_mutex);
    streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
    pthread_mutex_unlock(&h->device_list_mutex);

    streams[cur_stream]->stream_type = STREAM_TYPE_VIDEO;
    streams[cur_stream]->stream_format = VIDEO_UNCOMPRESSED;
    streams[cur_stream]->width  = decklink_opts->width;
    streams[cur_stream]->height = decklink_opts->height;
    streams[cur_stream]->timebase_num = decklink_opts->timebase_num;
    streams[cur_stream]->timebase_den = decklink_opts->timebase_den;
    streams[cur_stream]->csp    = PIX_FMT_YUV422P10;
    streams[cur_stream]->interlaced = decklink_opts->interlaced;
    streams[cur_stream]->tff = 1; /* NTSC is bff in baseband but coded as tff */
    streams[cur_stream]->sar_num = streams[cur_stream]->sar_den = 1; /* The user can choose this when encoding */

    if (add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_FRAME) < 0)
        goto finish;
    cur_stream++;

    for (int i = 0; i < MAX_AUDIO_PAIRS; i++) {
        struct audio_pair_s *pair = &decklink_ctx->audio_pairs[i];

        ALLOC_STREAM(cur_stream]);
        streams[cur_stream]->sdi_audio_pair = i + 1;

        pthread_mutex_lock(&h->device_list_mutex);
        streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock(&h->device_list_mutex);

        if (!pair->smpte337_detected_ac3)
        {
            streams[cur_stream]->stream_type = STREAM_TYPE_AUDIO;
            streams[cur_stream]->stream_format = AUDIO_PCM;
            streams[cur_stream]->num_channels  = 2;
            streams[cur_stream]->sample_format = AV_SAMPLE_FMT_S32P;
            /* TODO: support other sample rates */
            streams[cur_stream]->sample_rate = 48000;
        } else {

            streams[cur_stream]->stream_type = STREAM_TYPE_AUDIO;
            streams[cur_stream]->stream_format = AUDIO_AC_3_BITSTREAM;

            /* In reality, the muxer inspects the bistream for these details before constructing a descriptor.
             * We expose it here show the probe message on the console are a little more reasonable.
             * TODO: Fill out sample_rate and bitrate from the SMPTE337 detector.
             */
            streams[cur_stream]->sample_rate = 48000;
            streams[cur_stream]->bitrate = 384;
            streams[cur_stream]->pid = 0x124; /* TODO: hardcoded PID not currently used. */
            if (add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM) < 0)
                goto finish;
        }
        cur_stream++;
    } /* For all audio pairs.... */

    /* Add a new output stream type, a TABLE_SECTION mechanism.
     * We use this to pass DVB table sections direct to the muxer,
     * for SCTE35, and other sections in the future.
     */
    if (OPTION_ENABLED(scte35))
    {
        ALLOC_STREAM(cur_stream);

        pthread_mutex_lock(&h->device_list_mutex);
        streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock(&h->device_list_mutex);

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = DVB_TABLE_SECTION;
        streams[cur_stream]->pid = 0x123; /* TODO: hardcoded PID not currently used. */
        if(add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM) < 0 )
            goto finish;
        cur_stream++;
    }

    /* Add a new output stream type, a SCTE2038 mechanism.
     * We use this to pass PES direct to the muxer.
     */
    if (OPTION_ENABLED(smpte2038))
    {
        ALLOC_STREAM(cur_stream);

        pthread_mutex_lock(&h->device_list_mutex);
        streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock(&h->device_list_mutex);

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = SMPTE2038;
        streams[cur_stream]->pid = 0x124; /* TODO: hardcoded PID not currently used. */
        if(add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM) < 0 )
            goto finish;
        cur_stream++;
    }

    if( non_display_parser->has_vbi_frame )
    {
        ALLOC_STREAM(cur_stream);

        pthread_mutex_lock( &h->device_list_mutex );
        streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock( &h->device_list_mutex );

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = VBI_RAW;
        streams[cur_stream]->vbi_ntsc = decklink_opts->video_format == INPUT_VIDEO_FORMAT_NTSC;
        if( add_non_display_services( non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM ) < 0 )
            goto finish;
        cur_stream++;
    }

    if( non_display_parser->has_ttx_frame )
    {
        ALLOC_STREAM(cur_stream);

        pthread_mutex_lock( &h->device_list_mutex );
        streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock( &h->device_list_mutex );

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = MISC_TELETEXT;
        if( add_teletext_service( non_display_parser, streams[cur_stream] ) < 0 )
            goto finish;
        cur_stream++;
    }

    if( non_display_parser->num_frame_data )
        free( non_display_parser->frame_data );

    device = new_device();

    if( !device )
        goto finish;

    device->num_input_streams = cur_stream;
    memcpy( device->streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**) );
    device->device_type = INPUT_DEVICE_DECKLINK;
    memcpy( &device->user_opts, user_opts, sizeof(*user_opts) );

    /* Upstream is responsible for freeing streams[x] allocations */

    /* add device */
    add_device( h, device );

finish:
    if( decklink_opts )
        free( decklink_opts );

    free( probe_ctx );

    return NULL;
}

static void *open_input( void *ptr )
{
    obe_input_params_t *input = (obe_input_params_t*)ptr;
    obe_t *h = input->h;
    obe_device_t *device = input->device;
    obe_input_t *user_opts = &device->user_opts;
    decklink_ctx_t *decklink_ctx;
    obe_sdi_non_display_data_t *non_display_parser;
    struct decklink_status status;

    decklink_opts_t *decklink_opts = (decklink_opts_t*)calloc( 1, sizeof(*decklink_opts) );
    if( !decklink_opts )
    {
        fprintf( stderr, "Malloc failed\n" );
        return NULL;
    }

    status.input = input;
    status.decklink_opts = decklink_opts;
    pthread_cleanup_push( close_thread, (void*)&status );

    decklink_opts->num_channels = 16;
    decklink_opts->card_idx = user_opts->card_idx;
    decklink_opts->video_conn = user_opts->video_connection;
    decklink_opts->audio_conn = user_opts->audio_connection;
    decklink_opts->video_format = user_opts->video_format;
    decklink_opts->enable_smpte2038 = user_opts->enable_smpte2038;
    decklink_opts->enable_scte35 = user_opts->enable_scte35;
    decklink_opts->enable_vanc_cache = user_opts->enable_vanc_cache;
    decklink_opts->enable_bitstream_audio = user_opts->enable_bitstream_audio;

    decklink_ctx = &decklink_opts->decklink_ctx;

    decklink_ctx->device = device;
    decklink_ctx->h = h;
    decklink_ctx->last_frame_time = -1;

    non_display_parser = &decklink_ctx->non_display_parser;
    non_display_parser->device = device;

    /* TODO: wait for encoder */

    if( open_card( decklink_opts ) < 0 )
        return NULL;

    sleep( INT_MAX );

    pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_input_func_t decklink_input = { probe_stream, open_input };
