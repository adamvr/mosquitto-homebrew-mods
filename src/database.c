/*
Copyright (c) 2009,2010, Roger Light <roger@atchoo.org>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of mosquitto nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

/* A note on matching topic subscriptions.
 *
 * Topics can be up to 32767 characters in length. The / character is used as a
 * hierarchy delimiter. Messages are published to a particular topic.
 * Clients may subscribe to particular topics directly, but may also use
 * wildcards in subscriptions.  The + and # characters are used as wildcards.
 * The # wildcard can be used at the end of a subscription only, and is a
 * wildcard for the level of hierarchy at which it is placed and all subsequent
 * levels.
 * The + wildcard may be used at any point within the subscription and is a
 * wildcard for only the level of hierarchy at which it is placed.
 * Neither wildcard may be used as part of a substring.
 * Valid:
 * 	a/b/+
 * 	a/+/c
 * 	a/#
 * 	a/b/#
 * 	#
 * 	+/b/c
 * 	+/+/+
 * Invalid:
 *	a/#/c
 *	a+/b/c
 * Valid but non-matching:
 *	a/b
 *	a/+
 *	+/b
 *	b/c/a
 *	a/b/d
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <config.h>
#include <mqtt3.h>
#include <memory_mosq.h>
#include <util_mosq.h>

static int max_inflight = 20;
static int max_queued = 100;

static int _mqtt3_db_cleanup(mosquitto_db *db);

int mqtt3_db_open(mqtt3_config *config, mosquitto_db *db)
{
	int rc = 0;
	struct _mosquitto_subhier *child;

	if(!config) return 1;

	db->last_db_id = 0;

	db->context_count = 1;
	db->contexts = _mosquitto_malloc(sizeof(mqtt3_context*)*db->context_count);
	if(!db->contexts) return MOSQ_ERR_NOMEM;
	db->contexts[0] = NULL;

	db->subs.next = NULL;
	db->subs.subs = NULL;
	db->subs.topic = "";

	child = _mosquitto_malloc(sizeof(struct _mosquitto_subhier));
	child->next = NULL;
	child->topic = _mosquitto_strdup("");
	child->subs = NULL;
	child->children = NULL;
	child->retained = NULL;
	db->subs.children = child;

	child = _mosquitto_malloc(sizeof(struct _mosquitto_subhier));
	child->next = NULL;
	child->topic = _mosquitto_strdup("/");
	child->subs = NULL;
	child->children = NULL;
	child->retained = NULL;
	db->subs.children->next = child;

	child = _mosquitto_malloc(sizeof(struct _mosquitto_subhier));
	child->next = NULL;
	child->topic = _mosquitto_strdup("$SYS");
	child->subs = NULL;
	child->children = NULL;
	child->retained = NULL;
	db->subs.children->next->next = child;

#ifdef WITH_PERSISTENCE
	if(config->persistence){
		if(config->persistence_location && strlen(config->persistence_location)){
			db->filepath = _mosquitto_malloc(strlen(config->persistence_location) + strlen(config->persistence_file) + 1);
			if(!db->filepath) return 1;
			sprintf(db->filepath, "%s%s", config->persistence_location, config->persistence_file);
			if(mqtt3_db_restore(db)) return 1;
		}else{
			db->filepath = _mosquitto_strdup(config->persistence_file);
			if(mqtt3_db_restore(db)) return 1;
		}
	}
#endif

	if(_mqtt3_db_cleanup(db)) return 1;

	return rc;
}

static void subhier_clean(struct _mosquitto_subhier *subhier)
{
	struct _mosquitto_subhier *next;
	struct _mosquitto_subleaf *leaf, *nextleaf;

	while(subhier){
		next = subhier->next;
		leaf = subhier->subs;
		while(leaf){
			nextleaf = leaf->next;
			_mosquitto_free(leaf);
			leaf = nextleaf;
		}
		if(subhier->retained){
			subhier->retained->ref_count--;
		}
		subhier_clean(subhier->children);
		if(subhier->topic) _mosquitto_free(subhier->topic);

		_mosquitto_free(subhier);
		subhier = next;
	}
}

int mqtt3_db_close(mosquitto_db *db)
{
	subhier_clean(db->subs.children);
	mqtt3_db_store_clean(db);

	if(db->filepath) _mosquitto_free(db->filepath);

	return MOSQ_ERR_SUCCESS;
}

/* Called on client death to add a will to the message queue if the will exists.
 * Returns 1 on failure (context or context->core.id is NULL)
 * Returns 0 on success (will queued or will not found)
 */
int mqtt3_db_client_will_queue(mosquitto_db *db, mqtt3_context *context)
{
	if(!context || !context->core.id) return 1;
	if(!context->core.will) return 0;

	return mqtt3_db_messages_easy_queue(db, context, context->core.will->topic, context->core.will->qos, context->core.will->payloadlen, context->core.will->payload, context->core.will->retain);
}

/* Returns the number of client currently in the database.
 * This includes inactive clients.
 * Returns 1 on failure (count is NULL)
 * Returns 0 on success.
 */
int mqtt3_db_client_count(mosquitto_db *db, int *count)
{
	int i;

	if(!db || !count) return 1;

	*count = 0;
	for(i=0; i<db->context_count; i++){
		if(db->contexts[i]) (*count)++;
	}

	return 0;
}

/* Internal function.
 * Set all stored sockets to -1 (invalid) when starting mosquitto.
 * Also removes any stray clients and subcriptions that may be around from a prior crash.
 * Returns 1 on failure (db is NULL)
 * Returns 0 on success.
 */
static int _mqtt3_db_cleanup(mosquitto_db *db)
{
	int rc = 0;

	if(!db) return 1;

// FIXME - reimplement for new db
#if 0
	query = sqlite3_mprintf("UPDATE clients SET sock=-1");
	/* Remove any stray clients that have clean session set. */
	query = sqlite3_mprintf("DELETE FROM clients WHERE sock=-1 AND clean_session=1");
	/* Remove any subs with no client. */
	query = sqlite3_mprintf("DELETE FROM subs WHERE client_id NOT IN (SELECT id FROM clients)");
	/* Remove any messages with no client. */
	query = sqlite3_mprintf("DELETE FROM messages WHERE client_id NOT IN (SELECT id FROM clients)");
#endif
	return rc;
}

int mqtt3_db_message_delete(mqtt3_context *context, uint16_t mid, enum mosquitto_msg_direction dir)
{
	mosquitto_client_msg *tail, *last = NULL;
	int msg_index = 0;
	bool deleted = false;

	if(!context) return 1;

	tail = context->msgs;
	while(tail){
		msg_index++;
		if(tail->state == ms_queued && msg_index <= max_inflight){
			tail->timestamp = time(NULL);
			if(tail->direction == mosq_md_out){
				switch(tail->qos){
					case 0:
						tail->state = ms_publish;
						break;
					case 1:
						tail->state = ms_publish_puback;
						break;
					case 2:
						tail->state = ms_publish_pubrec;
						break;
				}
			}else{
				if(tail->qos == 2){
					tail->state = ms_wait_pubrec;
				}
			}
		}
		if(tail->mid == mid && tail->direction == dir){
			msg_index--;
			/* FIXME - it would be nice to be able to remove the stored message here if ref_count==0 */
			tail->store->ref_count--;
			if(last){
				last->next = tail->next;
			}else{
				context->msgs = tail->next;
			}
			_mosquitto_free(tail);
			if(last){
				tail = last->next;
			}else{
				tail = context->msgs;
			}
			deleted = true;
		}else{
			last = tail;
			tail = tail->next;
		}
		if(msg_index > max_inflight && deleted){
			return 0;
		}
	}

	return 0;
}

int mqtt3_db_message_insert(mqtt3_context *context, uint16_t mid, enum mosquitto_msg_direction dir, int qos, bool retain, struct mosquitto_msg_store *stored)
{
	mosquitto_client_msg *msg, *tail = NULL;
	enum mqtt3_msg_state state = ms_invalid;
	int msg_count = 0;
	int rc = 0;

	assert(stored);
	if(!context) return 1;

	if(context->msgs){
		tail = context->msgs;
		msg_count = 1;
		while(tail && tail->next){
			if(tail->qos > 0){
				msg_count++;
			}
			tail = tail->next;
		}
	}

	if(qos == 0 || max_inflight == 0 || msg_count < max_inflight){
		if(dir == mosq_md_out){
			switch(qos){
				case 0:
					state = ms_publish;
					break;
				case 1:
					state = ms_publish_puback;
					break;
				case 2:
					state = ms_publish_pubrec;
					break;
			}
		}else{
			if(qos == 2){
				state = ms_wait_pubrec;
			}else{
				return 1;
			}
		}
	}else if(max_queued == 0 || msg_count-max_inflight < max_queued){
		state = ms_queued;
		rc = 2;
	}else{
		/* Dropping message due to full queue.
		 * FIXME - should this be logged? */
		return 2;
	}
	assert(state != ms_invalid);

	msg = _mosquitto_malloc(sizeof(mosquitto_client_msg));
	if(!msg) return 1;
	msg->next = NULL;
	msg->store = stored;
	msg->store->ref_count++;
	msg->mid = mid;
	msg->timestamp = time(NULL);
	msg->direction = dir;
	msg->state = state;
	msg->dup = false;
	msg->qos = qos;
	msg->retain = retain;
	if(tail){
		tail->next = msg;
	}else{
		context->msgs = msg;
	}

	return rc;
}

int mqtt3_db_message_update(mqtt3_context *context, uint16_t mid, enum mosquitto_msg_direction dir, enum mqtt3_msg_state state)
{
	mosquitto_client_msg *tail;

	tail = context->msgs;
	while(tail){
		if(tail->mid == mid && tail->direction == dir){
			tail->state = state;
			tail->timestamp = time(NULL);
			return 0;
		}
		tail = tail->next;
	}
	return 1;
}

int mqtt3_db_messages_delete(mqtt3_context *context)
{
	mosquitto_client_msg *tail, *next;

	if(!context) return 1;

	tail = context->msgs;
	while(tail){
		/* FIXME - it would be nice to be able to remove the stored message here if rec_count==0 */
		tail->store->ref_count--;
		next = tail->next;
		_mosquitto_free(tail);
		tail = next;
	}
	context->msgs = NULL;

	return 0;
}

int mqtt3_db_messages_easy_queue(mosquitto_db *db, mqtt3_context *context, const char *topic, int qos, uint32_t payloadlen, const uint8_t *payload, int retain)
{
	struct mosquitto_msg_store *stored;
	char *source_id;

	assert(db);

	if(!topic) return 1;

	if(context){
		source_id = context->core.id;
	}else{
		source_id = "";
	}
	if(mqtt3_db_message_store(db, source_id, 0, topic, qos, payloadlen, payload, retain, &stored, 0)) return 1;

	return mqtt3_db_messages_queue(db, source_id, topic, qos, retain, stored);
}

int mqtt3_db_messages_queue(mosquitto_db *db, const char *source_id, const char *topic, int qos, int retain, struct mosquitto_msg_store *stored)
{
	int rc = 0;

	assert(db);
	assert(stored);

	/* Find all clients that subscribe to topic and put messages into the db for them. */
	if(!source_id || !topic) return 1;

	mqtt3_sub_search(&db->subs, source_id, topic, qos, retain, stored);
	return rc;
}

int mqtt3_db_message_store(mosquitto_db *db, const char *source, uint16_t source_mid, const char *topic, int qos, uint32_t payloadlen, const uint8_t *payload, int retain, struct mosquitto_msg_store **stored, dbid_t store_id)
{
	struct mosquitto_msg_store *temp;

	assert(db);
	assert(stored);

	if(!topic) return 1;

	temp = _mosquitto_malloc(sizeof(struct mosquitto_msg_store));
	if(!temp) return 1;

	temp->next = db->msg_store;
	temp->ref_count = 0;
	if(source){
		temp->source_id = _mosquitto_strdup(source);
	}else{
		temp->source_id = _mosquitto_strdup("");
	}
	temp->source_mid = source_mid;
	temp->msg.qos = qos;
	temp->msg.retain = retain;
	temp->msg.topic = _mosquitto_strdup(topic);
	temp->msg.payloadlen = payloadlen;
	if(payloadlen){
		temp->msg.payload = _mosquitto_malloc(sizeof(uint8_t)*payloadlen);
		memcpy(temp->msg.payload, payload, sizeof(uint8_t)*payloadlen);
	}else{
		temp->msg.payload = NULL;
	}

	if(!temp->source_id || !temp->msg.topic || (payloadlen && !temp->msg.payload)){
		if(temp->source_id) _mosquitto_free(temp->source_id);
		if(temp->msg.topic) _mosquitto_free(temp->msg.topic);
		if(temp->msg.payload) _mosquitto_free(temp->msg.payload);
		_mosquitto_free(temp);
		return 1;
	}
	db->msg_store_count++;
	db->msg_store = temp;
	(*stored) = temp;

	if(!store_id){
		temp->db_id = ++db->last_db_id;
	}else{
		temp->db_id = store_id;
	}

	return 0;
}

int mqtt3_db_message_store_find(mosquitto_db *db, const char *source, uint16_t mid, struct mosquitto_msg_store **stored)
{
	struct mosquitto_msg_store *tail;

	*stored = NULL;
	tail = db->msg_store;
	while(tail){
		if(tail->source_mid == mid && !strcmp(tail->source_id, source)){
			*stored = tail;
			return 0;
		}
		tail = tail->next;
	}

	return 1;
}

int mqtt3_db_message_timeout_check(mosquitto_db *db, unsigned int timeout)
{
	int i;
	time_t threshold = time(NULL) - timeout;
	enum mqtt3_msg_state new_state = ms_invalid;
	mqtt3_context *context;
	mosquitto_client_msg *msg;

	
	for(i=0; i<db->context_count; i++){
		context = db->contexts[i];
		if(!context) continue;

		msg = context->msgs;
		while(msg){
			if(msg->timestamp < threshold && msg->state != ms_queued){
				switch(msg->state){
					case ms_wait_puback:
						new_state = ms_publish_puback;
						break;
					case ms_wait_pubrec:
						new_state = ms_publish_pubrec;
						break;
					case ms_wait_pubrel:
						new_state = ms_resend_pubrec;
						break;
					case ms_wait_pubcomp:
						new_state = ms_resend_pubrel;
						break;
					default:
						break;
				}
				if(new_state != ms_invalid){
					msg->timestamp = time(NULL);
					msg->state = new_state;
					msg->dup = true;
				}
			}
			msg = msg->next;
		}
	}

	return MOSQ_ERR_SUCCESS;
}

int mqtt3_db_message_release(mosquitto_db *db, mqtt3_context *context, uint16_t mid, enum mosquitto_msg_direction dir)
{
	mosquitto_client_msg *tail, *last = NULL;
	int qos;
	int retain;
	char *topic;
	char *source_id;

	if(!context) return 1;

	tail = context->msgs;
	while(tail){
		if(tail->mid == mid && tail->direction == dir){
			qos = tail->store->msg.qos;
			topic = tail->store->msg.topic;
			retain = tail->retain;
			source_id = tail->store->source_id;

			if(!mqtt3_db_messages_queue(db, source_id, topic, qos, retain, tail->store)){
				tail->store->ref_count--;
				if(last){
					last->next = tail->next;
				}else{
					context->msgs = tail->next;
				}
				_mosquitto_free(tail);
				return 0;
			}else{
				return 1;
			}
		}
		last = tail;
		tail = tail->next;
	}
	return 1;
}

int mqtt3_db_message_write(mqtt3_context *context)
{
	mosquitto_client_msg *tail, *last = NULL;
	uint16_t mid;
	int retries;
	int retain;
	const char *topic;
	int qos;
	uint32_t payloadlen;
	const uint8_t *payload;

	if(!context || !context->core.id || context->core.sock == -1) return 1;

	tail = context->msgs;
	while(tail){
		if(tail->direction == mosq_md_out && tail->state != ms_queued){
			mid = tail->mid;
			retries = tail->dup;
			retain = tail->retain;
			topic = tail->store->msg.topic;
			qos = tail->qos;
			payloadlen = tail->store->msg.payloadlen;
			payload = tail->store->msg.payload;

			switch(tail->state){
				case ms_publish:
					if(!mqtt3_raw_publish(context, retries, qos, retain, mid, topic, payloadlen, payload)){
						if(last){
							last->next = tail->next;
							tail->store->ref_count--;
							_mosquitto_free(tail);
							tail = last->next;
						}else{
							context->msgs = tail->next;
							tail->store->ref_count--;
							_mosquitto_free(tail);
							tail = context->msgs;
						}
					}
					break;

				case ms_publish_puback:
					if(!mqtt3_raw_publish(context, retries, qos, retain, mid, topic, payloadlen, payload)){
						tail->state = ms_wait_puback;
					}
					last = tail;
					tail = tail->next;
					break;

				case ms_publish_pubrec:
					if(!mqtt3_raw_publish(context, retries, qos, retain, mid, topic, payloadlen, payload)){
						tail->state = ms_wait_pubrec;
					}
					last = tail;
					tail = tail->next;
					break;
				
				case ms_resend_pubrec:
					if(!mqtt3_raw_pubrec(context, mid)){
						tail->state = ms_wait_pubrel;
					}
					last = tail;
					tail = tail->next;
					break;

				case ms_resend_pubrel:
					if(!mqtt3_raw_pubrel(context, mid, true)){
						tail->state = ms_wait_pubcomp;
					}
					last = tail;
					tail = tail->next;
					break;

				case ms_resend_pubcomp:
					if(!mqtt3_raw_pubcomp(context, mid)){
						tail->state = ms_wait_pubrel;
					}
					last = tail;
					tail = tail->next;
					break;

				default:
					last = tail;
					tail = tail->next;
					break;
			}
		}else{
			last = tail;
			tail = tail->next;
		}
	}

	return 0;
}

void mqtt3_db_store_clean(mosquitto_db *db)
{
	/* FIXME - this may not be necessary if checks are made when messages are removed. */
	struct mosquitto_msg_store *tail, *last = NULL;
	assert(db);

	tail = db->msg_store;
	while(tail){
		if(tail->ref_count == 0){
			if(tail->source_id) _mosquitto_free(tail->source_id);
			if(tail->msg.topic) _mosquitto_free(tail->msg.topic);
			if(tail->msg.payload) _mosquitto_free(tail->msg.payload);
			if(last){
				last->next = tail->next;
				_mosquitto_free(tail);
				tail = last->next;
			}else{
				db->msg_store = tail->next;
				_mosquitto_free(tail);
				tail = db->msg_store;
			}
			db->msg_store_count--;
		}else{
			last = tail;
			tail = tail->next;
		}
	}
}

/* Send messages for the $SYS hierarchy if the last update is longer than
 * 'interval' seconds ago.
 * 'interval' is the amount of seconds between updates. If 0, then no periodic
 * messages are sent for the $SYS hierarchy.
 * 'start_time' is the result of time() that the broker was started at.
 */
void mqtt3_db_sys_update(mosquitto_db *db, int interval, time_t start_time)
{
	static time_t last_update = 0;
	time_t now = time(NULL);
	char buf[100];
	int count;

	if(interval && now - interval > last_update){
		snprintf(buf, 100, "%d seconds", (int)(now - start_time));
		mqtt3_db_messages_easy_queue(db, NULL, "$SYS/broker/uptime", 2, strlen(buf), (uint8_t *)buf, 1);

		snprintf(buf, 100, "%d", db->msg_store_count);
		mqtt3_db_messages_easy_queue(db, NULL, "$SYS/broker/messages/stored", 2, strlen(buf), (uint8_t *)buf, 1);

		if(!mqtt3_db_client_count(db, &count)){
			snprintf(buf, 100, "%d", count);
			mqtt3_db_messages_easy_queue(db, NULL, "$SYS/broker/clients/total", 2, strlen(buf), (uint8_t *)buf, 1);
		}

#ifdef REAL_WITH_MEMORY_TRACKING
		snprintf(buf, 100, "%ld", _mosquitto_memory_used());
		mqtt3_db_messages_easy_queue(db, NULL, "$SYS/broker/heap/current size", 2, strlen(buf), (uint8_t *)buf, 1);
#endif

		snprintf(buf, 100, "%lu", mqtt3_net_msgs_total_received());
		mqtt3_db_messages_easy_queue(db, NULL, "$SYS/broker/messages/received", 2, strlen(buf), (uint8_t *)buf, 1);
		
		snprintf(buf, 100, "%lu", mqtt3_net_msgs_total_sent());
		mqtt3_db_messages_easy_queue(db, NULL, "$SYS/broker/messages/sent", 2, strlen(buf), (uint8_t *)buf, 1);

		snprintf(buf, 100, "%llu", (unsigned long long)mqtt3_net_bytes_total_received());
		mqtt3_db_messages_easy_queue(db, NULL, "$SYS/broker/bytes/received", 2, strlen(buf), (uint8_t *)buf, 1);
		
		snprintf(buf, 100, "%llu", (unsigned long long)mqtt3_net_bytes_total_sent());
		mqtt3_db_messages_easy_queue(db, NULL, "$SYS/broker/bytes/sent", 2, strlen(buf), (uint8_t *)buf, 1);
		
		last_update = time(NULL);
	}
}

void mqtt3_db_limits_set(int inflight, int queued)
{
	max_inflight = inflight;
	max_queued = queued;
}

void mqtt3_db_vacuum(void)
{
	/* FIXME - reimplement? */
}

