/*****************************************************************************
 * ac3bitstream.c : AC3/A52 compressed bitstream passthrough
 *****************************************************************************
 * Copyright (C) 2017 Kernel Labs Inc.
 *
 * Authors: Steven Toth <stoth@kernellabs.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 ******************************************************************************/

#include "common/common.h"
#include "encoders/audio/audio.h"
#include "audiobitstream_slicer.h"
#include "hexdump.h"

#ifdef HAVE_LIBKLMONITORING_KLMONITORING_H
#include <libklmonitoring/klmonitoring.h>
#endif

#define LOCAL_DEBUG 0

static int64_t cur_pts = -1;

/* This function needs to free the buffer.
 * We're passed aligned ac3 syncframe() structs, that have passed both CRC checks.
 */
static void * slicer_callback(void *user_context, struct audiobitstream_slicer_c *ctx, uint8_t *buf, uint32_t buflen)
{
	obe_aud_enc_params_t *enc_params = user_context;
	obe_t *h = enc_params->h;
	obe_encoder_t *encoder = enc_params->encoder;

#if LOCAL_DEBUG
	printf("%s(%d) --\n", __func__, buflen);
	hexdump(buf, buflen > 32 ? 32 : buflen, 32);
#endif

	/* A full formed AC3 syncframe just popped out of the slicer.
	 * Forward this to the MUX for PES encapsulation.
	 */
	obe_coded_frame_t *cf = new_coded_frame(encoder->output_stream_id, buflen);
	if (!cf) {
		syslog(LOG_ERR, "Malloc failed\n");
		free(buf);
		return 0;
	}

	cf->pts = cur_pts;
	cf->random_access = 1; /* Every frame output is a random access point */
	memcpy(cf->data, buf, buflen);
	cf->len = buflen;

	add_to_queue(&h->mux_queue, cf);

	free(buf);
	return 0;
}

static void *start_encoder_ac3bitstream(void *ptr)
{
#if LOCAL_DEBUG
	printf("%s()\n", __func__);
#endif
	struct audiobitstream_slicer_c *abs_slicer = audiobitstream_slicer_alloc(TYPE_AC3, (audiobitstream_slicer_callback)slicer_callback, ptr);

#ifdef HAVE_LIBKLMONITORING_KLMONITORING_H
	struct kl_histogram audio_passthrough;
	int histogram_dump = 0;
	kl_histogram_reset(&audio_passthrough, "audio ac3 syncframe passthrough", KL_BUCKET_VIDEO);
#endif
	obe_aud_enc_params_t *enc_params = ptr;
	obe_encoder_t *encoder = enc_params->encoder;
	obe_output_stream_t *stream = enc_params->stream;

	/* Lock the mutex until we verify parameters */
	pthread_mutex_lock(&encoder->queue.mutex);

	encoder->is_ready = 1;

	/* Broadcast because input and muxer can be stuck waiting for encoder */
	pthread_cond_broadcast(&encoder->queue.in_cv);
	pthread_mutex_unlock(&encoder->queue.mutex);

	while (1) {
		pthread_mutex_lock(&encoder->queue.mutex);

		while(!encoder->queue.size && !encoder->cancel_thread)
			pthread_cond_wait(&encoder->queue.in_cv, &encoder->queue.mutex);

		if (encoder->cancel_thread) {
			pthread_mutex_unlock(&encoder->queue.mutex);
			break;
		}

		obe_raw_frame_t *frm = encoder->queue.queue[0];
		pthread_mutex_unlock(&encoder->queue.mutex);

		if (stream->channel_layout == AV_CH_LAYOUT_STEREO) {
			/* TODO: Do something so the muxer knows to change the PMT descriptor? */
		}

		/* Cache the latest PTS */
		cur_pts = frm->pts;

#ifdef HAVE_LIBKLMONITORING_KLMONITORING_H
		kl_histogram_sample_begin(&audio_passthrough);
#endif
		/* frm->audio_frame.linesize
		 * frm->audio_frame.num_samples
		 * frm->audio_frame.audio_data
		 */

#if LOCAL_DEBUG
		/* Send any audio to the AC3 frame slicer.
		 * Push the buffer starting at the channel containing bitstream, and span 2 channels,
		 * we'll get called back with a completely aligned, crc'd and valid AC3 frame.
		 */
		printf("%s() linesize = %d, num_samples = %d, num_channels = %d, sample_fmt = %d, raw_frame->input_stream_id = %d\n",
			__func__,
			frm->audio_frame.linesize,
			frm->audio_frame.num_samples, frm->audio_frame.num_channels,
			frm->audio_frame.sample_fmt,
			frm->input_stream_id);
		hexdump((uint8_t *)frm->audio_frame.audio_data[0], 32, 32);
#endif

		/* TODO: Channel span is fixed at 2, shouldthis vary? */
		/* TODO: Channels = 16, will this be valid for original descklink cards with 8 channels? */
		int span = 2;
		int channels = 16; /* span from group 0 */

		/* Fixed at 32b, as the decklink cards are hardcoded for 32. */
		int depth = 32; /* 32 bit samples, data in LSB 16 bits */
		size_t l = audiobitstream_slicer_write(abs_slicer,
			(uint8_t *)frm->audio_frame.audio_data[0],
			frm->audio_frame.num_samples,
			depth, /* Bitdepth */
			channels, channels * (depth / 8), span);
		if (l <= 0) {
			syslog(LOG_ERR, "AC3Bitstream write() failed\n");
		}

#ifdef HAVE_LIBKLMONITORING_KLMONITORING_H
		kl_histogram_sample_complete(&audio_passthrough);
		if (histogram_dump++ > 240) {
			histogram_dump = 0;
#if PRINT_HISTOGRAMS
			kl_histogram_printf(&audio_passthrough);
#endif
		}
#endif
		frm->release_data(frm);
		frm->release_frame(frm);
		remove_from_queue(&encoder->queue);
	}

	if (abs_slicer)
		audiobitstream_slicer_free(abs_slicer);

	free(enc_params);

	return NULL;
}

const obe_aud_enc_func_t ac3bitstream_encoder = { start_encoder_ac3bitstream };
