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
 * Deal with checksuming. If the checksum fails, we don't pass to the callback.
 * Imeplemtation is ideally bistream agnostic, but in the absense of streams other than
 * AC3 its highly likely to have some ac3 specifics hard coded.
 */

#ifndef _AUDIOBITSTREAM_SLICER_H 
#define _AUDIOBITSTREAM_SLICER_H 

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include "klringbuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

enum audio_slicer_type_e
{
	TYPE_UNDEFINED = 0,
	TYPE_AC3,
};

enum audio_slicer_state_e
{
	S_UNDEFINED = 0,
	S_SEARCHING_SYNC,
	S_ACQUIRED_SYNC1,
};

struct audiobitstream_slicer_c;

typedef void (*audiobitstream_slicer_callback)(void *user_context, struct audiobitstream_slicer_c *ctx,
	uint8_t *buf, uint32_t buflen);

struct audiobitstream_slicer_c
{
	enum audio_slicer_type_e type;
	enum audio_slicer_state_e state;
	KLRingBuffer *rb;

	uint32_t words_per_syncframe;

	audiobitstream_slicer_callback cb;
	void *cbContext;
};

struct audiobitstream_slicer_c *audiobitstream_slicer_alloc(enum audio_slicer_type_e type,
	audiobitstream_slicer_callback cb,
	void *cbContext);

void audiobitstream_slicer_free(struct audiobitstream_slicer_c *ctx);

size_t audiobitstream_slicer_write(struct audiobitstream_slicer_c *ctx, uint8_t *buf,
	uint32_t audioFrames, uint32_t sampleDepth, uint32_t channelsPerFrame, uint32_t frameStrideBytes,
	uint32_t spanCount);

#ifdef __cplusplus
};
#endif

#endif /* _AUDIOBITSTREAM_SLICER_H */
