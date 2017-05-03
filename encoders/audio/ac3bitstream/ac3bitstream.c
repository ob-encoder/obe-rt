/*****************************************************************************
 * ac3bitstream.c : AC3/A52 compressed bitstream passthrough.
 * Use a SMPTE337 slicer to extract bitstream payload from an audio channel.
 * Analyze that audio, if we detect AC3 then forward it to the MUX, else
 * discard it.
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
#include "input/sdi/smpte337_detector.h"

#ifdef HAVE_LIBKLMONITORING_KLMONITORING_H
#include <libklmonitoring/klmonitoring.h>
#endif

#define LOCAL_DEBUG 0
#if LOCAL_DEBUG
#include "hexdump.h"
#endif

static int64_t cur_pts = -1;

/* Polynomial table for AC3/A52 checksums 16+15+1+1 */
static const uint16_t crc_tab[] =
{
	0x0000, 0x8005, 0x800F, 0x000A, 0x801B, 0x001E, 0x0014, 0x8011,
	0x8033, 0x0036, 0x003C, 0x8039, 0x0028, 0x802D, 0x8027, 0x0022,
	0x8063, 0x0066, 0x006C, 0x8069, 0x0078, 0x807D, 0x8077, 0x0072,
	0x0050, 0x8055, 0x805F, 0x005A, 0x804B, 0x004E, 0x0044, 0x8041,
	0x80C3, 0x00C6, 0x00CC, 0x80C9, 0x00D8, 0x80DD, 0x80D7, 0x00D2,
	0x00F0, 0x80F5, 0x80FF, 0x00FA, 0x80EB, 0x00EE, 0x00E4, 0x80E1,
	0x00A0, 0x80A5, 0x80AF, 0x00AA, 0x80BB, 0x00BE, 0x00B4, 0x80B1,
	0x8093, 0x0096, 0x009C, 0x8099, 0x0088, 0x808D, 0x8087, 0x0082,
	0x8183, 0x0186, 0x018C, 0x8189, 0x0198, 0x819D, 0x8197, 0x0192,
	0x01B0, 0x81B5, 0x81BF, 0x01BA, 0x81AB, 0x01AE, 0x01A4, 0x81A1,
	0x01E0, 0x81E5, 0x81EF, 0x01EA, 0x81FB, 0x01FE, 0x01F4, 0x81F1,
	0x81D3, 0x01D6, 0x01DC, 0x81D9, 0x01C8, 0x81CD, 0x81C7, 0x01C2,
	0x0140, 0x8145, 0x814F, 0x014A, 0x815B, 0x015E, 0x0154, 0x8151,
	0x8173, 0x0176, 0x017C, 0x8179, 0x0168, 0x816D, 0x8167, 0x0162,
	0x8123, 0x0126, 0x012C, 0x8129, 0x0138, 0x813D, 0x8137, 0x0132,
	0x0110, 0x8115, 0x811F, 0x011A, 0x810B, 0x010E, 0x0104, 0x8101,
	0x8303, 0x0306, 0x030C, 0x8309, 0x0318, 0x831D, 0x8317, 0x0312,
	0x0330, 0x8335, 0x833F, 0x033A, 0x832B, 0x032E, 0x0324, 0x8321,
	0x0360, 0x8365, 0x836F, 0x036A, 0x837B, 0x037E, 0x0374, 0x8371,
	0x8353, 0x0356, 0x035C, 0x8359, 0x0348, 0x834D, 0x8347, 0x0342,
	0x03C0, 0x83C5, 0x83CF, 0x03CA, 0x83DB, 0x03DE, 0x03D4, 0x83D1,
	0x83F3, 0x03F6, 0x03FC, 0x83F9, 0x03E8, 0x83ED, 0x83E7, 0x03E2,
	0x83A3, 0x03A6, 0x03AC, 0x83A9, 0x03B8, 0x83BD, 0x83B7, 0x03B2,
	0x0390, 0x8395, 0x839F, 0x039A, 0x838B, 0x038E, 0x0384, 0x8381,
	0x0280, 0x8285, 0x828F, 0x028A, 0x829B, 0x029E, 0x0294, 0x8291,
	0x82B3, 0x02B6, 0x02BC, 0x82B9, 0x02A8, 0x82AD, 0x82A7, 0x02A2,
	0x82E3, 0x02E6, 0x02EC, 0x82E9, 0x02F8, 0x82FD, 0x82F7, 0x02F2,
	0x02D0, 0x82D5, 0x82DF, 0x02DA, 0x82CB, 0x02CE, 0x02C4, 0x82C1,
	0x8243, 0x0246, 0x024C, 0x8249, 0x0258, 0x825D, 0x8257, 0x0252,
	0x0270, 0x8275, 0x827F, 0x027A, 0x826B, 0x026E, 0x0264, 0x8261,
	0x0220, 0x8225, 0x822F, 0x022A, 0x823B, 0x023E, 0x0234, 0x8231,
	0x8213, 0x0216, 0x021C, 0x8219, 0x0208, 0x820D, 0x8207, 0x0202
};

static uint16_t crc_calc(const uint16_t *words, uint32_t wordCount)
{
	uint16_t crc = 0;
	for (uint32_t i = 0; i < wordCount; i++) {
		uint8_t t = (uint8_t)((words[i] >> 8) & 0xFF);
		crc = (crc << 8) ^ crc_tab[((crc >> 8) & 0xFF) ^ t];

		t = (uint8_t)(words[i] & 0xFF);
		crc = (crc << 8) ^ crc_tab[((crc >> 8 ) & 0xFF) ^ t];
	}
	return crc; 
}

/* Endian swap an entire buffer of words */
static void swap_buffer_16b(uint8_t *buf, int numberWords)
{
        for (int j = 0; j < (numberWords * 2); j += 2) {
                uint8_t a = *(buf + j + 0);
                uint8_t b = *(buf + j + 1);
                *(buf + j + 0) = b;
                *(buf + j + 1) = a;
        }
}

/* Check the AC3 frame checksums, if they're broken report to console. */
static int validateCRC(uint8_t *buf, uint32_t buflen)
{
	int ret = 1; /* Success */

	/* Validate the CRC when its in word format. */
	uint32_t framesize = buflen / sizeof(uint16_t);
	uint32_t framesize58 = (framesize / 2) + (framesize / 8);

	/* TODO: We can skip CRC1 given that CRC2 covers the entire packet. */
	uint16_t crc = crc_calc(((const uint16_t *)buf) + 1, framesize58 - 1);
	if (crc != 0) {
		fprintf(stdout, "[AC3] CRC1 failure, dropping frame, framesize = %d, framesize58 = %d.\n", framesize, framesize58);
		ret = 0;
	}

	uint16_t crc2 = crc_calc(((const uint16_t *)buf) + 1, framesize - 1);
	if (crc2 != 0) {
		fprintf(stdout, "[AC3] CRC2 failure, dropping frame, framesize = %d, framesize58 = %d.\n", framesize, framesize58);
		ret = 0;
	}

	return ret;
}

/* We're going to be handed a SMPTE337 bitstream, including the header.
 * It might be AC3, or it could very well be something else, check it.
 * Assuming its AC3, repackage and forward it to the muxer.
 */
static void * detector_callback(void *user_context,
        struct smpte337_detector_s *ctx,
        uint8_t datamode, uint8_t datatype, uint32_t payload_bitCount, uint8_t *payload)
{
	obe_aud_enc_params_t *enc_params = user_context;
	obe_t *h = enc_params->h;
	obe_encoder_t *encoder = enc_params->encoder;
	uint32_t payload_byteCount = payload_bitCount / 8;

#if LOCAL_DEBUG
	printf("[AC3] ac3encoder:%s(%d) --\n", __func__, payload_byteCount);
	hexdump(payload, 32, 32);

        printf("[AC3] ac3encoder:%s() datamode = %d [%sbit], datatype = %d [payload: %s]"
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

        if (datatype != 1 /* AC3 */) {
                fprintf(stderr, "[AC3] Detected SMPTE337 datamode %d, we don't support it.", datamode);
		return 0;
	}

	/* The SMPTE337 slicers hands us a bitstream, not a word stream.
	 * AC3 checksums are word based, so, yeah, not nice, switch the
	 * endian, check the checksum and then flip it back.
	 * TODO: improve this.
	 */
	swap_buffer_16b(payload, payload_byteCount / 2);
	if (validateCRC(payload, payload_byteCount) != 1) {
		return 0;
	}

	/* The muxer wants the stream in its original byte format,
	 * swap the endian back. */
	swap_buffer_16b(payload, payload_byteCount / 2);

	/* A full syncfame(), verified ass accurate, forward this to the MUX for PES encapsulation. */
	obe_coded_frame_t *cf = new_coded_frame(encoder->output_stream_id, payload_byteCount);
	if (!cf) {
		syslog(LOG_ERR, "[AC3] ac3encoder: Malloc failed\n");
		return 0;
	}

	cf->pts = cur_pts;
	cf->random_access = 1; /* Every frame output is a random access point */
	memcpy(cf->data, payload, payload_byteCount);
	cf->len = payload_byteCount;

	add_to_queue(&h->mux_queue, cf);

        return 0;
}

static void *start_encoder_ac3bitstream(void *ptr)
{
#if LOCAL_DEBUG
	printf("%s()\n", __func__);
#endif

	/* We need a bitstream SMPTE337 slicer to do our bidding.... */
        struct smpte337_detector_s *smpte337_detector = smpte337_detector_alloc((smpte337_detector_callback)detector_callback, ptr);

#ifdef HAVE_LIBKLMONITORING_KLMONITORING_H
	struct kl_histogram audio_passthrough;
	int histogram_dump = 0;
	kl_histogram_reset(&audio_passthrough, "audio ac3 syncframe passthrough", KL_BUCKET_VIDEO);
#endif
	obe_aud_enc_params_t *enc_params = ptr;
	obe_encoder_t *encoder = enc_params->encoder;

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

		/* Cache the latest PTS */
		cur_pts = frm->pts;

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

		/* Channel span is always two according to the spec. We've written the code so it can vary. */
		int span = 2; /* span from group 1 audio channels 1/2 */
		int channels = 16; /* TODO: Channels = 16, will this be valid for original decklink cards with 8 channels? */

		/* Fixed at 32b, as the decklink cards are hardcoded for 32. */
		/* TODO: OBE only runs in the32bit audio mode. If we switch to 16bit mode (probably never),
		 * then this will need to be adjusted.
		 */
		int depth = 32; /* 32 bit samples, data in LSB 16 bits */

#ifdef HAVE_LIBKLMONITORING_KLMONITORING_H
		kl_histogram_sample_begin(&audio_passthrough);
#endif
		size_t l = smpte337_detector_write(smpte337_detector, (uint8_t *)frm->audio_frame.audio_data[0],
			frm->audio_frame.num_samples,
			depth,
			channels,
			channels * (depth / 8),
			span);
		if (l <= 0) {
			syslog(LOG_ERR, "[AC3] AC3Bitstream write() failed\n");
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

	if (smpte337_detector)
		smpte337_detector_free(smpte337_detector);

	free(enc_params);

	return NULL;
}

const obe_aud_enc_func_t ac3bitstream_encoder = { start_encoder_ac3bitstream };
