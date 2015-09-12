/*
 * This file is part of dmrshark.
 *
 * dmrshark is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dmrshark is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dmrshark.  If not, see <http://www.gnu.org/licenses/>.
**/

#include <config/defaults.h>

#include "voicestreams.h"

#include <libs/config/config-voicestreams.h>
#include <libs/base/base.h>
#include <libs/base/dmr.h>
#include <libs/daemon/console.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void voicestreams_savetorawfile(uint8_t *voice_bytes, uint8_t voice_bytes_count, char *voicestream_name) {
	FILE *f;
	char fn[255];
	char *dir;

	if (voice_bytes == NULL || voice_bytes_count == 0 || voicestream_name == NULL || strlen(voicestream_name) == 0)
		return;

	dir = config_voicestreams_get_savefiledir(voicestream_name);
	if (dir == NULL || strlen(dir) == 0)
		dir = ".";
	snprintf(fn, sizeof(fn), "%s/%s.raw", dir, voicestream_name);

	f = fopen(fn, "a");
	if (!f)
		return;

#if 0
	console_log("voicestreams raw: written %d bytes\n", fwrite(voice_bytes, 1, voice_bytes_count, f));
#else
	fwrite(voice_bytes, 1, voice_bytes_count, f);
#endif
	fclose(f);
}

void voicestreams_list(void) {
	char **streamnames = config_voicestreams_streamnames_get();
	char *host;
	char *dir;

	if (streamnames == NULL) {
		console_log("no voice streams defined in config file.\n");
		return;
	}
	console_log("voice streams:\n");

	while (*streamnames != NULL) {
		host = config_voicestreams_get_repeaterhost(*streamnames);
		dir = config_voicestreams_get_savefiledir(*streamnames);

		console_log("%s: enabled: %u rptrhost: %s ts: %u savedir: %s saveraw: %u\n", *streamnames,
			config_voicestreams_get_enabled(*streamnames),
			host,
			config_voicestreams_get_timeslot(*streamnames),
			(strlen(dir) == 0 ? "." : dir),
			config_voicestreams_get_savetorawfile(*streamnames));

		free(host);
		free(dir);

		streamnames++;
	}
}

void voicestreams_processpacket(ipscpacket_t *ipscpacket, repeater_t *repeater) {
	char *voicestream_name;
	dmrpacket_payload_voice_bits_t *voice_bits;
	uint8_t i;
	uint8_t voice_bytes[sizeof(dmrpacket_payload_voice_bits_t)/8];

	if (ipscpacket == NULL || repeater == NULL)
		return;

	switch (ipscpacket->slot_type) {
		case IPSCPACKET_SLOT_TYPE_VOICE_DATA_A: // Only processing voice data packets.
		case IPSCPACKET_SLOT_TYPE_VOICE_DATA_B:
		case IPSCPACKET_SLOT_TYPE_VOICE_DATA_C:
		case IPSCPACKET_SLOT_TYPE_VOICE_DATA_D:
		case IPSCPACKET_SLOT_TYPE_VOICE_DATA_E:
			break;
		default:
			return;
	}

	voicestream_name = repeater->slot[ipscpacket->timeslot-1].voicestream_name;

	if (voicestream_name == NULL) // No voice stream defined for given repeater & timeslot?
		return;

	if (!config_voicestreams_get_enabled(voicestream_name))
		return;

	voice_bits = dmrpacket_extract_voice_bits(&ipscpacket->payload_bits);
	for (i = 0; i < sizeof(voice_bits->bits); i += 8)
		voice_bytes[i/8] = base_bitstobyte(&voice_bits->bits[i]);

	if (config_voicestreams_get_savetorawfile(voicestream_name))
		voicestreams_savetorawfile(voice_bytes, sizeof(voice_bytes), voicestream_name);

	// TODO: streaming
}
