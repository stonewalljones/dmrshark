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

// This SMS TX buffer takes care of sending SMSes and retrying when send
// fails. It's a FIFO, and only one (the first) element is tried to be
// sent at a time.

#include DEFAULTCONFIG

#include "smstxbuf.h"
#include "dmr-data.h"
#include "smsrtbuf.h"
#include "data-packet-txbuf.h"

#include <libs/daemon/console.h>
#include <libs/daemon/daemon-poll.h>
#include <libs/comm/repeaters.h>
#include <libs/config/config.h>
#include <libs/remotedb/remotedb.h>
#include <libs/remotedb/userdb.h>

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static pthread_mutex_t smstxbuf_mutex = PTHREAD_MUTEX_INITIALIZER;

static smstxbuf_t *smstxbuf_first_entry = NULL;
static smstxbuf_t *smstxbuf_last_entry = NULL;

void smstxbuf_print_entry(smstxbuf_t *entry) {
	char added_at_str[20];

	strftime(added_at_str, sizeof(added_at_str), "%F %T", gmtime(&entry->added_at));
	if (entry->repeater == NULL)
		console_log("  repeater: all ");
	else {
		console_log("  repeater: %s ts: %u ",
			repeaters_get_display_string_for_ip(&entry->repeater->ipaddr),
			entry->ts+1);
	}
	if (entry->aprs_msg)
		console_log("from aprs callsign: %s ", entry->aprs_msg->src_callsign);
	console_log("dst id: %u type: %s added at: %s send tries: %u type: %s dbid: %u delay: %u msg: %s\n",
		entry->dst_id, dmr_get_readable_call_type(entry->call_type), added_at_str, entry->send_tries, dmr_get_readable_data_type(entry->data_type), entry->db_id, entry->delay_before_send_sec, entry->msg);
}

void smstxbuf_print(void) {
	smstxbuf_t *entry;

	pthread_mutex_lock(&smstxbuf_mutex);
	entry = smstxbuf_first_entry;

	if (entry == NULL)
		console_log("smstxbuf: empty\n");
	else {
		console_log("smstxbuf:\n");
		while (entry) {
			smstxbuf_print_entry(entry);
			entry = entry->next;
		}
	}
	pthread_mutex_unlock(&smstxbuf_mutex);
}

// In case of repeater is 0, the SMS will be sent broadcast.
void smstxbuf_add(uint8_t delay_before_send_sec, repeater_t *repeater, dmr_timeslot_t ts, dmr_call_type_t calltype, dmr_id_t dstid, dmr_data_type_t data_type, char *msg, unsigned int db_id, aprs_msg_t *aprs_msg) {
	smstxbuf_t *new_smstxbuf_entry;

	if (msg == NULL)
		return;

	pthread_mutex_lock(&smstxbuf_mutex);
	if (smstxbuf_last_entry != NULL &&
		smstxbuf_last_entry->dst_id == dstid &&
		smstxbuf_last_entry->data_type == data_type &&
		smstxbuf_last_entry->repeater == repeater &&
		smstxbuf_last_entry->ts == ts &&
		smstxbuf_last_entry->call_type == calltype &&
		strncmp(smstxbuf_last_entry->msg, msg, sizeof(smstxbuf_last_entry->msg)) == 0) {
			pthread_mutex_unlock(&smstxbuf_mutex);
			return; // We won't add duplicate entries.
	}

	new_smstxbuf_entry = (smstxbuf_t *)calloc(1, sizeof(smstxbuf_t));
	if (new_smstxbuf_entry == NULL) {
		console_log("  error: can't allocate memory for new sms buffer entry\n");
		pthread_mutex_unlock(&smstxbuf_mutex);
		return;
	}

	strncpy(new_smstxbuf_entry->msg, msg, DMRPACKET_MAX_FRAGMENTSIZE);
	new_smstxbuf_entry->delay_before_send_sec = delay_before_send_sec;
	new_smstxbuf_entry->added_at = time(NULL);
	new_smstxbuf_entry->data_type = data_type;
	new_smstxbuf_entry->call_type = calltype;
	new_smstxbuf_entry->dst_id = dstid;
	new_smstxbuf_entry->repeater = repeater;
	new_smstxbuf_entry->ts = ts;
	new_smstxbuf_entry->db_id = db_id;
	if (aprs_msg) {
		new_smstxbuf_entry->aprs_msg = (aprs_msg_t *)calloc(1, sizeof(aprs_msg_t));
		memcpy(new_smstxbuf_entry->aprs_msg, aprs_msg, sizeof(aprs_msg_t));
	}

	console_log("smstxbuf: adding new sms:\n");
	smstxbuf_print_entry(new_smstxbuf_entry);

	if (smstxbuf_last_entry == NULL)
		smstxbuf_last_entry = smstxbuf_first_entry = new_smstxbuf_entry;
	else {
		// Putting the new entry to the end of the linked list.
		smstxbuf_last_entry->next = new_smstxbuf_entry;
		smstxbuf_last_entry = new_smstxbuf_entry;
	}
	daemon_poll_setmaxtimeout(0);
	pthread_mutex_unlock(&smstxbuf_mutex);
}

static void smstxbuf_remove_first_entry(void) {
	smstxbuf_t *nextentry;
	loglevel_t loglevel;

	pthread_mutex_lock(&smstxbuf_mutex);
	if (smstxbuf_first_entry == NULL) {
		pthread_mutex_unlock(&smstxbuf_mutex);
		return;
	}

	loglevel = console_get_loglevel();
	if (loglevel.flags.dataq && loglevel.flags.debug) {
		console_log(LOGLEVEL_DATAQ LOGLEVEL_DEBUG "smstxbuf: removing first entry:\n");
		smstxbuf_print_entry(smstxbuf_first_entry);
	}

	nextentry = smstxbuf_first_entry->next;
	smstxbuf_free_entry(smstxbuf_first_entry);
	smstxbuf_first_entry = nextentry;
	if (smstxbuf_first_entry == NULL)
		smstxbuf_last_entry = NULL;

	pthread_mutex_unlock(&smstxbuf_mutex);
}

void smstxbuf_first_entry_sent_successfully(repeater_t *repeater) {
	smsrtbuf_t *smsrtbuf_entry;
	char *aprs_ack_msg;
	uint8_t aprs_ack_msg_length;

	pthread_mutex_lock(&smstxbuf_mutex);
	if (smstxbuf_first_entry == NULL) {
		pthread_mutex_unlock(&smstxbuf_mutex);
		return;
	}

	console_log(LOGLEVEL_DATAQ "smstxbuf: first entry sent successfully\n");
	if (smstxbuf_first_entry->db_id)
		remotedb_msgqueue_updateentry(smstxbuf_first_entry->db_id, 1);
	if (smstxbuf_first_entry->aprs_msg != NULL) {
		console_log(LOGLEVEL_DATAQ "  the entry is an incoming aprs message\n");
		if (smstxbuf_first_entry->aprs_msg->ackpart[0] == 0)
			console_log(LOGLEVEL_DATAQ "    no aprs message ack requested\n");
		else {
			console_log(LOGLEVEL_DATAQ "    aprs message ack requested\n");
			if (repeater == NULL || repeater->callsign[0] == 0)
				console_log(LOGLEVEL_DATAQ "      repeater is not defined, can't send aprs message ack\n");
			else {
				aprs_ack_msg_length = sizeof(smstxbuf_first_entry->aprs_msg->ackpart)+4;
				aprs_ack_msg = (char *)calloc(1, aprs_ack_msg_length);
				snprintf(aprs_ack_msg, aprs_ack_msg_length, "ack%s}", smstxbuf_first_entry->aprs_msg->ackpart);
				aprs_add_to_queue_msg(smstxbuf_first_entry->aprs_msg->src_callsign, smstxbuf_first_entry->aprs_msg->dst_callsign, aprs_ack_msg, repeater->callsign);
				free(aprs_ack_msg);
			}
		}
	}

	smsrtbuf_entry = smsrtbuf_find_entry(smstxbuf_first_entry->dst_id, smstxbuf_first_entry->msg);
	pthread_mutex_unlock(&smstxbuf_mutex);
	if (smsrtbuf_entry != NULL)
		smsrtbuf_entry_sent_successfully(smsrtbuf_entry);

	smstxbuf_remove_first_entry();
}

static void smstxbuf_first_entry_send_unsuccessful(void) {
	smsrtbuf_t *smsrtbuf_entry;
	int db_id;

	pthread_mutex_lock(&smstxbuf_mutex);
	if (smstxbuf_first_entry == NULL) {
		pthread_mutex_unlock(&smstxbuf_mutex);
		return;
	}

	console_log(LOGLEVEL_DATAQ "smstxbuf: first entry send unsuccessful\n");
	db_id = smstxbuf_first_entry->db_id;

	smsrtbuf_entry = smsrtbuf_find_entry(smstxbuf_first_entry->dst_id, smstxbuf_first_entry->msg);
	pthread_mutex_unlock(&smstxbuf_mutex);
	if (smsrtbuf_entry != NULL)
		smsrtbuf_entry_send_unsuccessful(smsrtbuf_entry);

	smstxbuf_remove_first_entry();

	pthread_mutex_lock(&smstxbuf_mutex);
	if (db_id && (smstxbuf_first_entry == NULL || (smstxbuf_first_entry != NULL && smstxbuf_first_entry->db_id != db_id)))
		remotedb_msgqueue_updateentry(db_id, 0);
	pthread_mutex_unlock(&smstxbuf_mutex);
}

void smstxbuf_first_entry_waiting_for_tms_ack_started(void) {
	if (smstxbuf_first_entry)
		smstxbuf_first_entry->waiting_for_tms_ack_started_at = time(NULL);
}

// The returned buffer entry must be freed after use with smstxbuf_free_entry().
smstxbuf_t *smstxbuf_get_first_entry(void) {
	smstxbuf_t *result = NULL;

	pthread_mutex_lock(&smstxbuf_mutex);
	if (smstxbuf_first_entry) {
		result = (smstxbuf_t *)malloc(sizeof(smstxbuf_t));
		if (result == NULL)
			console_log("smstxbuf error: can't get first entry, not enough memory\n");
		else {
			memcpy(result, smstxbuf_first_entry, sizeof(smstxbuf_t));
			if (smstxbuf_first_entry->aprs_msg != NULL) {
				result->aprs_msg = (aprs_msg_t *)calloc(1, sizeof(aprs_msg_t));
				memcpy(result->aprs_msg, smstxbuf_first_entry->aprs_msg, sizeof(aprs_msg_t));
			}
		}
	}
	pthread_mutex_unlock(&smstxbuf_mutex);
	return result;
}

void smstxbuf_free_entry(smstxbuf_t *entry) {
	if (entry == NULL)
		return;

	if (entry->aprs_msg)
		free(entry->aprs_msg);
	free(entry);
}

void smstxbuf_process(void) {
	loglevel_t loglevel;

	pthread_mutex_lock(&smstxbuf_mutex);
	if (smstxbuf_first_entry == NULL) {
		pthread_mutex_unlock(&smstxbuf_mutex);
		return;
	}

	if (data_packet_txbuf_get_first_entry() != NULL) { // Only sending an SMS if data packet TX buffer is empty.
		pthread_mutex_unlock(&smstxbuf_mutex);
		return;
	}

	// We allow some time for the TMS ack to arrive.
	if (smstxbuf_first_entry->waiting_for_tms_ack_started_at != 0 && time(NULL)-smstxbuf_first_entry->waiting_for_tms_ack_started_at < 10) {
		pthread_mutex_unlock(&smstxbuf_mutex);
		return;
	}

	if (time(NULL) < smstxbuf_first_entry->added_at+smstxbuf_first_entry->delay_before_send_sec) {
		pthread_mutex_unlock(&smstxbuf_mutex);
		return;
	}

	if (smstxbuf_first_entry->send_tries >= config_get_smssendmaxretrycount()) {
		console_log(LOGLEVEL_DATAQ "smstxbuf: all tries of sending the first entry has failed\n");
		smstxbuf_print_entry(smstxbuf_first_entry);
		pthread_mutex_unlock(&smstxbuf_mutex);
		smstxbuf_first_entry_send_unsuccessful();
		pthread_mutex_lock(&smstxbuf_mutex);
		if (smstxbuf_first_entry == NULL) {
			pthread_mutex_unlock(&smstxbuf_mutex);
			return;
		}
	}

	smstxbuf_first_entry->selective_ack_tries = 0;
	loglevel = console_get_loglevel();
	if (loglevel.flags.dataq) {
		console_log(LOGLEVEL_DATAQ "smstxbuf: sending entry:\n");
		smstxbuf_print_entry(smstxbuf_first_entry);
	}

	switch (smstxbuf_first_entry->data_type) {
		case DMR_DATA_TYPE_MOTOROLA_TMS_SMS:
			dmr_data_send_motorola_tms_sms((smstxbuf_first_entry->repeater == NULL), smstxbuf_first_entry->repeater, smstxbuf_first_entry->ts, smstxbuf_first_entry->call_type, smstxbuf_first_entry->dst_id, DMRSHARK_DEFAULT_DMR_ID, smstxbuf_first_entry->msg);
			break;
		case DMR_DATA_TYPE_NORMAL_SMS:
			dmr_data_send_sms((smstxbuf_first_entry->repeater == NULL), smstxbuf_first_entry->repeater, smstxbuf_first_entry->ts, smstxbuf_first_entry->call_type, smstxbuf_first_entry->dst_id, DMRSHARK_DEFAULT_DMR_ID, smstxbuf_first_entry->msg);
			break;
		default:
			break;
	}

	if (smstxbuf_first_entry->call_type == DMR_CALL_TYPE_GROUP) { // Group messages are unconfirmed, so we send them only once.
		pthread_mutex_unlock(&smstxbuf_mutex);
		smstxbuf_remove_first_entry();
	} else {
		smstxbuf_first_entry->send_tries++;
		pthread_mutex_unlock(&smstxbuf_mutex);
	}
	daemon_poll_setmaxtimeout(0);
}

void smstxbuf_deinit(void) {
	smstxbuf_t *next_entry;

	pthread_mutex_lock(&smstxbuf_mutex);
	while (smstxbuf_first_entry != NULL) {
		next_entry = smstxbuf_first_entry->next;
		smstxbuf_free_entry(smstxbuf_first_entry);
		smstxbuf_first_entry = next_entry;
	}
	smstxbuf_last_entry = NULL;
	pthread_mutex_unlock(&smstxbuf_mutex);
}
