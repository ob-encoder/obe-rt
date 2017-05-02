/*
 * Copyright (c) 2017 Kernel Labs Inc. All Rights Reserved
 *
 * Address: Kernel Labs Inc., PO Box 745, St James, NY. 11780
 * Contact: sales@kernellabs.com
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
 */

/* A tool to take PCM bitstreams from blackmagic cards, and reconstruct AC3 syncframes() blobs.
 * When a full synframe has been detected, trigger a callback with a properly aligned frame.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include "audiobitstream_slicer.h"
#include "hexdump.h"

#define AUDIO_SLICER_SYNC_CODE_AC3 0x0b77

#define U16_SWAP(n) ((n) >> 8 | ((n) & 0xff) << 8)

int dodump = 0;

/* Polynomial table for AC3/AC5 checksums 16+15+1+1 */
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
	for(uint32_t i = 0; i < wordCount; i++) {
		uint8_t t = (uint8_t)((words[i] >> 8) & 0xFF);
		crc = (crc << 8) ^ crc_tab[((crc >> 8) & 0xFF) ^ t];

		t = (uint8_t)(words[i] & 0xFF);
		crc = (crc << 8) ^ crc_tab[((crc >> 8 ) & 0xFF) ^ t];
	}
	// printf("crc = 0x%04x\n", crc);
	return crc; 
}

/* See Table 5.18. For a given AC3 stream, a collection of word lengths etc. */
static const struct fsct_s {
	uint32_t frmsizecod;
	uint32_t n_br;
	uint32_t fs32;
	uint32_t fs44;
	uint32_t fs48;
} fsct[] = {
	{ 0x00, 32, 96, 69, 64 },
	{ 0x01, 32, 96, 70, 64 },
	{ 0x02, 40, 120, 87, 80 },
	{ 0x03, 40, 120, 88, 80 },
	{ 0x04, 48, 144, 104, 96 },
	{ 0x05, 48, 144, 105, 96 },
	{ 0x06, 56, 168, 121, 112 },
	{ 0x07, 56, 168, 122, 112 },
	{ 0x08, 64, 192, 139, 128 },
	{ 0x09, 64, 192, 140, 128 },
	{ 0x0a, 80, 240, 174, 160 },
	{ 0x0b, 80, 240, 175, 160 },
	{ 0x0c, 96, 288, 208, 192 },
	{ 0x0d, 96, 288, 209, 192 },
	{ 0x0e, 112, 336, 243, 224 },
	{ 0x0f, 112, 336, 244, 224 },
	{ 0x10, 128, 384, 278, 256 },
	{ 0x11, 128, 384, 279, 256 },
	{ 0x12, 160, 480, 348, 320 },
	{ 0x13, 160, 480, 349, 320 },
	{ 0x14, 192, 576, 417, 384 },
	{ 0x15, 192, 576, 418, 384 },
	{ 0x16, 224, 672, 487, 448 },
	{ 0x17, 224, 672, 488, 448 },
	{ 0x18, 256, 768, 557, 512 },
	{ 0x19, 256, 768, 558, 512 },
	{ 0x1a, 320, 960, 696, 640 },
	{ 0x1b, 320, 960, 697, 640 },
	{ 0x1c, 384, 1152, 835, 768 },
	{ 0x1d, 384, 1152, 836, 768 },
	{ 0x1e, 448, 1344, 975, 896 },
	{ 0x1f, 448, 1344, 976, 896 },
	{ 0x20, 512, 1536, 1114, 1024 },
	{ 0x21, 512, 1536, 1115, 1024 },
	{ 0x22, 576, 1728, 1253, 1152 },
	{ 0x23, 576, 1728, 1254, 1152 },
	{ 0x24, 640, 1920, 1393, 1280 },
	{ 0x25, 640, 1920, 1394, 1280 },
};

static const struct fsct_s *lookupTable518(uint32_t frmsizecod)
{
	for (int i = 0; i < sizeof(fsct) / sizeof(struct fsct_s); i++) {
		if (fsct[i].frmsizecod == frmsizecod)
			return &fsct[i];
	}

	return 0;
}

struct audiobitstream_slicer_c *audiobitstream_slicer_alloc(enum audio_slicer_type_e type,
	audiobitstream_slicer_callback cb,
	void *cbContext)
{
	struct audiobitstream_slicer_c *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->type = type;
	ctx->state = S_SEARCHING_SYNC;
	ctx->cb = cb;
	ctx->cbContext = cbContext;
	ctx->rb = rb_new(32 * 1024, 256 * 1024);
	if (!ctx->rb) {
		free(ctx);
		return NULL;
	}

	return ctx;
}

void audiobitstream_slicer_free(struct audiobitstream_slicer_c *ctx)
{
	rb_free(ctx->rb);
	free(ctx);
}

static void swap_buffer_16b(uint8_t *buf, int numberWords)
{
	for (int j = 0; j < (numberWords * 2); j += 2) {
		uint8_t a = *(buf + j + 0);
		uint8_t b = *(buf + j + 1);
		*(buf + j + 0) = b;
		*(buf + j + 1) = a;
	}
}

/* For the existing ringbuffer, figure out whether its
 * contents are valid (check crcs), and pass a byte stream
 * to the caller.
 */
static int handleCallback(struct audiobitstream_slicer_c *ctx)
{
	int ret = 0;
	if (ctx->cb) {
		size_t buflen = rb_used(ctx->rb);
		uint8_t *buf = malloc(buflen);
		if (buf) {
			/* Send the entire AC3 syncframe() to the callback. */
			/* Receiving party is responsible for freeing the alloc */
			size_t l = rb_read(ctx->rb, (char *)buf, buflen);
			if (l != buflen) {
				fprintf(stdout, "[AC3] Warning, short read, you should never see this, l = %ld buflen = %ld\n", l, buflen);
				buflen = l;
			}

			/* Validate the CRC when its in word format. */
			uint32_t framesize = buflen / sizeof(uint16_t);
			uint32_t framesize58 = (framesize / 2) + (framesize / 8);

			/* TODO: We can skip CRC1 given that CRC2 covers the entire packet. */
			uint16_t crc = crc_calc(((const uint16_t *)buf) + 1, framesize58 - 1);
			if (crc != 0) {
				fprintf(stdout, "[AC3] CRC1 failure, dropping frame, framesize = %d, framesize58 = %d.\n", framesize, framesize58);
			}

			uint16_t crc2 = crc_calc(((const uint16_t *)buf) + 1, framesize - 1);
			if (crc2 != 0) {
				fprintf(stdout, "[AC3] CRC2 failure, dropping frame, framesize = %d, framesize58 = %d.\n", framesize, framesize58);
				//hexdump(buf, buflen, 32);
			}

			if (!crc && !crc2) {
				/* Swap endian and return to caller as a byte stream rather than
				 * a word stream. */
				swap_buffer_16b(buf, buflen / 2);
				ctx->cb(ctx->cbContext, ctx, (uint8_t *)buf, (uint32_t)buflen);
				ret = 0;
			} else {
#if 1
				fprintf(stderr, "CRC buflen = %d\n", buflen);
				swap_buffer_16b(buf, buflen / 2);
				static int fcnt = 0;
				char fn[64];
				sprintf(fn, "/tmp/crc%08d.bin", fcnt++);
				FILE *fh = fopen(fn, "wb");
				if (fh) {
					fwrite(buf, 1, buflen, fh);
					fclose(fh);
				}
				printf("crc fcnt = %d\n", fcnt);
#endif
				free(buf); /* User can't free the buffer if we haven't given it to them. */
				ret = -1;
			}
		}
	}
	rb_empty(ctx->rb);
	ctx->words_per_syncframe = 0;
	return ret;
}

static size_t audiobitstream_slicer_write_16b(struct audiobitstream_slicer_c *ctx, uint8_t *buf,
	uint32_t audioFrames, uint32_t sampleDepth, uint32_t channelsPerFrame, uint32_t frameStrideBytes,
	uint32_t spanCount)
{
	size_t consumed = 0;

	//printf("Writing %d frames, span = %d\n", audioFrames, spanCount);
	uint16_t *p = (uint16_t *)buf;
	for (int i = 0; i < audioFrames; i++) {

		if (ctx->words_per_syncframe == 0 && rb_used(ctx->rb) > 6) {
			/* Look at the header, figure out how many words we should get in
			 * the sync frame, so we know the length of the AC3 total data to collect.
			 */
			uint8_t x[6];
			if (rb_peek(ctx->rb, (char *)x, sizeof(x)) == 6) {

				/* The buffer contains LE words, don't forget this. */
				uint32_t fscod = x[5] >> 6;
				uint32_t frmsizecod = x[5] & 0x3f;

				const struct fsct_s *s = lookupTable518(frmsizecod);
				if (s) {
					if (fscod == 0 /* 48 */)
						ctx->words_per_syncframe = s->fs48;
					else if (fscod == 1 /* 44.1 */)
						ctx->words_per_syncframe = s->fs44;
					else if (fscod == 2 /* 32 */)
						ctx->words_per_syncframe = s->fs32;
					// printf("words_per_syncframe = %d\n", ctx->words_per_syncframe);
				}
			}
		}

		//printf("\nf %d -- ", i);
		uint16_t *q = p;
		for (int k = 0; k < spanCount; k++) {
			/* Sample in N words into a byte orientied buffer */
			//printf("0x%04x ", *q);
			if (ctx->state == S_SEARCHING_SYNC && *q == AUDIO_SLICER_SYNC_CODE_AC3) {
				ctx->state = S_ACQUIRED_SYNC1;
				rb_write(ctx->rb, (const char *)q, 2);
				q++;
				continue;
			}
			if (ctx->state == S_ACQUIRED_SYNC1 && *q == AUDIO_SLICER_SYNC_CODE_AC3 &&
				rb_used(ctx->rb) * 2 == ctx->words_per_syncframe) {
				//printf("CB %d bytes\n", rb_used(ctx->rb));
				if (rb_used(ctx->rb)) {
					handleCallback(ctx);
				}
				rb_write(ctx->rb, (const char *)q, 2);
				q++;
				ctx->state = S_ACQUIRED_SYNC1;
				continue;
			}
			if (ctx->state == S_ACQUIRED_SYNC1 && *q == AUDIO_SLICER_SYNC_CODE_AC3 &&
				rb_used(ctx->rb) * 2 != ctx->words_per_syncframe) {
				rb_write(ctx->rb, ((const char *)q) + 2, 2);
				q++;
				continue;
			}
			if (ctx->state == S_ACQUIRED_SYNC1 && *q != AUDIO_SLICER_SYNC_CODE_AC3) {
				rb_write(ctx->rb, (const char *)q, 2);
				q++;

				if (ctx->words_per_syncframe * 2 == rb_used(ctx->rb)) {
					handleCallback(ctx);
					ctx->state = S_SEARCHING_SYNC;
				}
				continue;
			}
		}

		p += (frameStrideBytes / sizeof(uint16_t));
	}
	return consumed;
}

static size_t audiobitstream_slicer_write_32b(struct audiobitstream_slicer_c *ctx, uint8_t *buf,
	uint32_t audioFrames, uint32_t sampleDepth, uint32_t channelsPerFrame, uint32_t frameStrideBytes,
	uint32_t spanCount)
{
	size_t consumed = 0;

	//printf("Writing %d frames, span = %d\n", audioFrames, spanCount);
	uint32_t *p = (uint32_t *)buf;
	for (int i = 0; i < audioFrames; i++) {

		if (ctx->words_per_syncframe == 0 && rb_used(ctx->rb) > 6) {
			/* Look at the header, figure out how many words we should get in
			 * the sync frame, so we know the length of the AC3 total data to collect.
			 */
			uint8_t x[6];
			if (rb_peek(ctx->rb, (char *)x, sizeof(x)) == 6) {

				/* The buffer contains LE words, don't forget this. */
				uint32_t fscod = x[5] >> 6;
				uint32_t frmsizecod = x[5] & 0x3f;

				const struct fsct_s *s = lookupTable518(frmsizecod);
				if (s) {
					if (fscod == 0 /* 48 */)
						ctx->words_per_syncframe = s->fs48;
					else if (fscod == 1 /* 44.1 */)
						ctx->words_per_syncframe = s->fs44;
					else if (fscod == 2 /* 32 */)
						ctx->words_per_syncframe = s->fs32;

					printf("words_per_syncframe = %d fscod = %02x frmsizecod = %02x\n", ctx->words_per_syncframe, fscod, frmsizecod);
				}
			}
		}

//		printf("f %d -- ", i);
		uint32_t *q = p;
		for (int k = 0; k < spanCount; k++) {
			/* Sample in N words into a byte orientied buffer */
			if (dodump)
				printf("0x%08x [%s] used=%d , ", *q,
					ctx->state == S_ACQUIRED_SYNC1 ? "SYNC  " :
					ctx->state == S_SEARCHING_SYNC ? "SEARCH" : "Undefined",
					(rb_used(ctx->rb) + 2) / 2);
			if (ctx->state == S_ACQUIRED_SYNC1 && *q == AUDIO_SLICER_SYNC_CODE_AC3 << 16 &&
				((rb_used(ctx->rb) * 2) == ctx->words_per_syncframe)) {
//printf("a\n");
				if (rb_used(ctx->rb)) {
					handleCallback(ctx);
				}
				ctx->state = S_SEARCHING_SYNC;
			}
			if (ctx->state == S_SEARCHING_SYNC && *q == AUDIO_SLICER_SYNC_CODE_AC3 << 16) {
//printf("b\n");
				ctx->state = S_ACQUIRED_SYNC1;
				rb_write(ctx->rb, ((const char *)q) + 2, 2);
				q++;
				continue;
			}
			if (ctx->state == S_ACQUIRED_SYNC1 && *q == AUDIO_SLICER_SYNC_CODE_AC3 << 16 &&
				((rb_used(ctx->rb) * 2) != ctx->words_per_syncframe)) {
//printf("e ");
				rb_write(ctx->rb, ((const char *)q) + 2, 2);
				q++;
				if ((ctx->words_per_syncframe * 2) == rb_used(ctx->rb)) {
					handleCallback(ctx);
					ctx->state = S_SEARCHING_SYNC;
				}
				continue;
			}
			if (ctx->state == S_ACQUIRED_SYNC1 && *q != AUDIO_SLICER_SYNC_CODE_AC3 << 16) {
				rb_write(ctx->rb, ((const char *)q) + 2, 2);
				q++;

//printf("c swpf = %d ", ctx->words_per_syncframe);
				if ((ctx->words_per_syncframe * 2) == rb_used(ctx->rb)) {
					handleCallback(ctx);
					ctx->state = S_SEARCHING_SYNC;
				}
				continue;
			}
//printf("d ");
			if (ctx->state == S_SEARCHING_SYNC && rb_used(ctx->rb))
			{
				printf("something went wrong?\n");
				exit(0);
			}
		}
//		printf("\n");

		p += (frameStrideBytes / sizeof(uint32_t));
	}
	return consumed;
}

size_t audiobitstream_slicer_write(struct audiobitstream_slicer_c *ctx, uint8_t *buf,
	uint32_t audioFrames, uint32_t sampleDepth, uint32_t channelsPerFrame, uint32_t frameStrideBytes,
	uint32_t spanCount)
{
	if ((!buf) || (!audioFrames) || (!channelsPerFrame) || (!frameStrideBytes) ||
		((sampleDepth != 16) && (sampleDepth != 32)) ||
		(spanCount == 0) || (spanCount > channelsPerFrame)) {
		return 0;
	}

	size_t ret = 0;

#if 1
        static int fcnt = 0;
        char fn[64];
        sprintf(fn, "/tmp/write%08d.bin", fcnt++);
        FILE *fh = fopen(fn, "wb");
        if (fh) {
                fwrite(buf, 1, audioFrames * channelsPerFrame * (sampleDepth / 8), fh);
                fclose(fh);
        }

	if (fcnt >= 100) {
		sprintf(fn, "/tmp/write%08d.bin", fcnt - 100);
		unlink(fn);
	}

//printf("ac3 fcnt = %d\n", fcnt);
#endif

fprintf(stderr, "rbused = %d\n", rb_used(ctx->rb));
if (rb_used(ctx->rb) > 32768) {
fprintf(stderr, "Terminating due to bug.\n");

FILE *fh = fopen("/tmp/ringbuffer.bin", "wb");
if (fh) {
  rb_fwrite(ctx->rb, fh);
  fclose(fh);
}

exit(0);
}
	if (sampleDepth == 16) {
		ret = audiobitstream_slicer_write_16b(ctx, buf, audioFrames, sampleDepth,
			channelsPerFrame, frameStrideBytes, spanCount);
	} else
	if (sampleDepth == 32) {
		ret = audiobitstream_slicer_write_32b(ctx, buf, audioFrames, sampleDepth,
			channelsPerFrame, frameStrideBytes, spanCount);
	}

	return ret;
}
