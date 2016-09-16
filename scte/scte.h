/*****************************************************************************
 * Copyright (c) 2016 Kernel Labs Inc.
 *
 * Authors:
 *   Steven Toth <stoth@kernellabs.com>
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
 *****************************************************************************/

struct scte35_context_s
{
	/* User visible fields */
	uint8_t pkt[188]; /* Assumption, sections always < this array size */
	uint8_t section[4096];
	uint16_t section_length;

	/* Private content, caller should not modify or inspect. */
	int verbose;
	uint16_t outputPid;
	uint8_t cc;
	uint32_t eventId;
	uint16_t uniqueProgramId;
};

void scte35_initialize(struct scte35_context_s *ctx, uint16_t outputPid);

/* Go into Ad, switch away from the network.
 * Return the number of TS packets generated in ctx->pkt, typically 1, or
 * < 0 on error.
 */
int scte35_generate_immediate_out_of_network(struct scte35_context_s *ctx, uint16_t uniqueProgramId);

/* Go out of Ad break, return back to the network.
 * Return the number of TS packets generated in ctx->pkt, typically 1, or
 * < 0 on error.
 */
int scte35_generate_immediate_in_to_network(struct scte35_context_s *ctx, uint16_t uniqueProgramId);

/* Generate a splice_null() heartbeat packet. This typically keeps the
 * downstream slicer alive, not specifically in the spec. 
 * Return the number of TS packets generated in ctx->pkt, typically 1, or
 * < 0 on error.
 */
int scte35_generate_heartbeat(struct scte35_context_s *ctx);

/* Allow the next event ID to be set, so that SCTE104 translated
 * packets, that contain their own eventID, we will to honor.
 * Return the number of TS packets generated in ctx->pkt, typically 1, or
 * < 0 on error.
 */
int scte35_set_next_event_id(struct scte35_context_s *ctx, uint32_t eventId);
