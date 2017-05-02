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
#include "smpte337_detector.h"

struct smpte337_detector_s *smpte337_detector_alloc(smpte337_detector_callback cb, void *cbContext)
{
	struct smpte337_detector_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->cb = cb;
	ctx->cbContext = cbContext;
	ctx->rb = rb_new_threadsafe(32 * 1024, 256 * 1024);
	if (!ctx->rb) {
		free(ctx);
		return NULL;
	}

	return ctx;
}

void smpte337_detector_free(struct smpte337_detector_s *ctx)
{
	rb_free(ctx->rb);
	free(ctx);
}

/* For the existing ringbuffer, figure out whether its
 * contents are valid (check crcs), and pass a byte stream
 * to the caller.
 */
static void handleCallback(struct smpte337_detector_s *ctx, uint8_t datamode, uint8_t datatype,
	uint32_t payload_bitCount, uint8_t *payload)
{
	ctx->cb(ctx->cbContext, ctx, datamode, datatype, payload_bitCount, payload);
}

static size_t smpte337_detector_write_16b(struct smpte337_detector_s *ctx, uint8_t *buf,
	uint32_t audioFrames, uint32_t sampleDepth, uint32_t channelsPerFrame,
	uint32_t frameStrideBytes,
	uint32_t spanCount)
{
	size_t consumed = 0;

	//printf("Writing %d frames, span = %d\n", audioFrames, spanCount);
	uint16_t *p = (uint16_t *)buf;
	for (int i = 0; i < audioFrames; i++) {

		//printf("\nf %d -- ", i);
		uint16_t *q = p;
		for (int k = 0; k < spanCount; k++) {
			/* Sample in N words into a byte orientied buffer */
			//printf("0x%08x ", *q);
			uint8_t *x = (uint8_t *)q;

			/* Flush the word into the fifo MSB first */
			int didOverflow = 0;
			rb_write_with_state(ctx->rb, ((const char *)x) + 1, 1, &didOverflow);
			if (didOverflow) {
				fprintf(stderr, "overflow occured.\n");
			}
			rb_write_with_state(ctx->rb, ((const char *)x) + 0, 1, &didOverflow);
			if (didOverflow) {
				fprintf(stderr, "overflow occured.\n");
			}
			q++;
			consumed += 2;
		}

		p += (frameStrideBytes / sizeof(uint16_t));
	}
	return consumed;
}

static size_t smpte337_detector_write_32b(struct smpte337_detector_s *ctx, uint8_t *buf,
	uint32_t audioFrames, uint32_t sampleDepth, uint32_t channelsPerFrame,
	uint32_t frameStrideBytes,
	uint32_t spanCount)
{
	size_t consumed = 0;

	//printf("%s() Writing %d frames, span = %d, sampleDepth = %d\n", __func__, audioFrames, spanCount, sampleDepth);
	uint32_t *p = (uint32_t *)buf;
	for (int i = 0; i < audioFrames; i++) {

		//printf("\nf %d -- ", i);
		uint32_t *q = p;
		for (int k = 0; k < spanCount; k++) {
			/* Sample in N words into a byte orientied buffer */
			//printf("0x%08x ", *q);
			uint8_t *x = (uint8_t *)q;

			/* Flush the word into the fifo MSB first */
			int didOverflow = 0;
			rb_write_with_state(ctx->rb, ((const char *)x) + 3, 1, &didOverflow);
			if (didOverflow) {
				fprintf(stderr, "overflow occured.\n");
			}
			rb_write_with_state(ctx->rb, ((const char *)x) + 2, 1, &didOverflow);
			if (didOverflow) {
				fprintf(stderr, "overflow occured.\n");
			}
			q++;

			consumed += 2;
		}

		p += (frameStrideBytes / sizeof(uint32_t));
	}
	return consumed;
}

static void run_detector(struct smpte337_detector_s *ctx)
{
	int skipped = 0;

	int skip_used_start = rb_used(ctx->rb);
	int skip_used_end = 0;

#define PEEK_LEN 16
	uint8_t dat[PEEK_LEN];
	while(1) {
//		dat[0] = 0x00;
//printf("rb_used = %d\n", rb_used(ctx->rb));
		if (rb_used(ctx->rb) < PEEK_LEN)
			break;

		if (rb_peek(ctx->rb, (char *)&dat[0], PEEK_LEN) < PEEK_LEN)
			break;
#if 0
		printf("%02x %02x %02x %02x\n",
			dat[0], dat[1], dat[2], dat[3]);
#endif
		/* Find the supported patterns */
		if (dat[0] == 0xF8 && dat[1] == 0x72 && dat[2] == 0x4e && dat[3] == 0x1f) {
			skip_used_end = rb_used(ctx->rb);
//printf("Skipped %d bytes looking for the SMPTE337 header, used %d bytes\n", skipped, skip_used_start - skip_used_end);
			/* pa = 16bit, pb = 16bit */
			if ((dat[5] & 0x1f) == 0x01) {
				/* Bits 0-4 datatype, 1 = AC3 */
				/* Bits 5-6 datamode, 0 = 16bit */
				/* Bits   7 errorflg, 0 = no error */
				uint32_t payload_bitCount = (dat[6] << 8) | dat[7];
				uint32_t payload_byteCount = payload_bitCount / 8;
				
				if (rb_used(ctx->rb) >= (8 + payload_byteCount)) {
					char *payload = NULL;
					size_t l = rb_read_alloc(ctx->rb, &payload, 8 + payload_byteCount);
					if (l != (8 + payload_byteCount)) {
						fprintf(stderr, "smpte337_detector: warning, rb read failure.\n");

						/* Intensionally flush the ring and start acquisition again. */
						rb_empty(ctx->rb);
					} else {
						handleCallback(ctx, (dat[5] >> 5) & 0x03, dat[5] & 0x1f, payload_bitCount, (uint8_t *)payload + 8);
					}
					if (payload)
						free(payload);
				} else {
					/* Not enough in the ring buffer, come back next time. */
					break;
				}

			} else {
				rb_discard(ctx->rb, 1); /* Pop a byte, and continue the search */
				skipped++;
			}
		} else {
			rb_discard(ctx->rb, 1); /* Pop a byte, and continue the search */
			skipped++;
		}
#if 0
		else
		if (dat[0] == 0x96 && dat[1] == 0xF8 && dat[2] == 0x72 &&
			dat[3] == 0xa5 && dat[4] == 0x4e && dat[5] == 0x1f) {
			/* pa = 24bit, pb = 24bit */
			if ((dat[8] & 0x1f) == 0x41) {
				/* Bits 0-4 datatype, 1 = AC3 */
				/* Bits 5-6 datamode, 2 = 24bit */
				/* Bits   7 errorflg, 0 = no error */
				handleCallback();
				break;
			}
		}
#endif
		
	} /* while */
}

size_t smpte337_detector_write(struct smpte337_detector_s *ctx, uint8_t *buf,
	uint32_t audioFrames, uint32_t sampleDepth, uint32_t channelsPerFrame,
	uint32_t frameStrideBytes, uint32_t spanCount)
{
	if ((!buf) || (!audioFrames) || (!channelsPerFrame) || (!frameStrideBytes) ||
		((sampleDepth != 16) && (sampleDepth != 32)) ||
		(spanCount == 0) || (spanCount > channelsPerFrame)) {
		return 0;
	}

	size_t ret = 0;
	if (sampleDepth == 16) {
		ret = smpte337_detector_write_16b(ctx, buf, audioFrames, sampleDepth,
			channelsPerFrame, frameStrideBytes, spanCount);
	} else
	if (sampleDepth == 32) {
		ret = smpte337_detector_write_32b(ctx, buf, audioFrames, sampleDepth,
			channelsPerFrame, frameStrideBytes, spanCount);
	}

	/* Now all the fifo contains byte stream re-ordered data, run the detector. */
	run_detector(ctx);

//	printf("%s() wrote %d bytes to ring, ring used %d unused %d\n", __func__, ret, rb_used(ctx->rb), rb_unused(ctx->rb));
	return ret;
}
