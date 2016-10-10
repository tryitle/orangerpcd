/*
	JUCI Backend Websocket API Server

	Copyright (C) 2016 Martin K. Schröder <mkschreder.uk@gmail.com>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. (Please read LICENSE file on special
	permission to include this software in signed images). 

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
*/

#include "orange.h"
#include "orange_rpc.h"
#include "internal.h"
#include <pthread.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/prctl.h>

#include "util.h"

#define WORKER_TIMEOUT_US 10000000UL

struct request_record {
	struct avl_node avl; 
	char *name; 
	struct timespec ts_expired; 
}; 

static bool rpcmsg_parse_call(struct blob *msg, uint32_t *id, const char **method, const struct blob_field **params){
	if(!msg) return false; 
	struct blob_policy policy[] = {
		{ .name = "id", .type = BLOB_FIELD_ANY, .value = NULL },
		{ .name = "method", .type = BLOB_FIELD_STRING, .value = NULL },
		{ .name = "params", .type = BLOB_FIELD_ARRAY, .value = NULL }
	}; 
	const struct blob_field *b = blob_field_first_child(blob_head(msg));  
	blob_field_parse_values(b, policy, 3); 
	*id = blob_field_get_int(policy[0].value); 
	*method = blob_field_get_string(policy[1].value); 
	*params = policy[2].value; 
	return !!(policy[0].value && method && params); 
}

static bool rpcmsg_parse_call_params(struct blob_field *params, const char **sid, const char **object, const char **method, const struct blob_field **args){
	if(!params) return false; 
	struct blob_policy policy[] = {
		{ .type = BLOB_FIELD_STRING }, // sid
		{ .type = BLOB_FIELD_STRING }, // object 
		{ .type = BLOB_FIELD_STRING }, // method
		{ .type = BLOB_FIELD_TABLE } // args
	}; 
	if(!blob_field_parse_values(params, policy, 4)) return false;  
	*sid = blob_field_get_string(policy[0].value); 
	*object = blob_field_get_string(policy[1].value); 
	*method = blob_field_get_string(policy[2].value); 
	*args = policy[3].value; 
	return !!(sid && object && method && args); 
}

static bool rpcmsg_parse_authenticate(struct blob_field *params, const char **sid){
	if(!params) return false; 
	struct blob_policy policy[] = {
		{ .type = BLOB_FIELD_STRING }, // sid
	}; 
	if(!blob_field_parse_values(params, policy, 1)) return false;  
	*sid = blob_field_get_string(policy[0].value); 
	return !!(sid); 
}

static bool rpcmsg_parse_list_params(struct blob_field *params, const char **sid, const char **path){
	if(!params) return false; 
	struct blob_policy policy[] = {
		{ .type = BLOB_FIELD_STRING }, // sid
		{ .type = BLOB_FIELD_STRING } // path
	}; 
	if(!blob_field_parse_values(params, policy, 2)) return false;  
	*sid = blob_field_get_string(policy[0].value); 
	*path = blob_field_get_string(policy[1].value); 
	return !!(sid && path); 
}

static bool rpcmsg_parse_login(struct blob_field *params, const char **username, const char **response){
	if(!params) return false; 
	struct blob_policy policy[] = {
		{ .type = BLOB_FIELD_STRING }, // username
		{ .type = BLOB_FIELD_STRING }  // challenge response
	}; 
	if(!blob_field_parse_values(params, policy, 2)) return false; 
	*username = blob_field_get_string(policy[0].value); 
	*response = blob_field_get_string(policy[1].value); 
	return !!(username && response); 
}

#ifdef CONFIG_THREADS
static int orange_rpc_process_requests(struct orange_rpc *self){
#else 
int orange_rpc_process_requests(struct orange_rpc *self){
#endif
	struct orange_message *msg = NULL;         
	struct timespec tss, tse; 

	int ret = orange_server_recv(self->server, &msg, self->timeout_us); 

	if(ret < 0){  
		return ret; 
	}
	
	if(!msg) return -EOF; 

	if(orange_debug_level >= JUCI_DBG_DEBUG){
		DEBUG("got message from %08x: ", msg->peer); 
		blob_dump_json(&msg->buf);
	}

	struct blob_field *params = NULL, *args = NULL; 
	const char *sid = "", *rpc_method = "", *object = "", *method = ""; 
	uint32_t rpc_id = 0; 
	
	struct orange_message *result = orange_message_new(); 
	result->peer = msg->peer; 

	if(!rpcmsg_parse_call(&msg->buf, &rpc_id, &rpc_method, (const struct blob_field**)&params)){
		DEBUG("could not parse incoming message\n"); 
		// we silently discard invalid messages!
		orange_message_delete(&msg); 
		return -EPROTO; 
	}

	blob_offset_t t = blob_open_table(&result->buf); 
	blob_put_string(&result->buf, "jsonrpc"); 
	blob_put_string(&result->buf, "2.0"); 
	blob_put_string(&result->buf, "id"); 
	blob_put_int(&result->buf, rpc_id); 

	if(rpc_method && strcmp(rpc_method, "call") == 0){
		if(rpcmsg_parse_call_params(params, &sid, &object, &method, (const struct blob_field**)&args)){
			// store the request for monitoring 
			struct request_record *req = calloc(1, sizeof(struct request_record)); 
			size_t len = strlen(object) + strlen(method) + 2; 
			req->name = calloc(1, len); 
			timespec_from_now_us(&req->ts_expired, WORKER_TIMEOUT_US); 
			snprintf(req->name, len, "%s.%s", object, method); 
			req->avl.key = req->name; 

			pthread_mutex_lock(&self->lock); 
			avl_insert(&self->requests, &req->avl); 
			pthread_mutex_unlock(&self->lock); 

			orange_call(self->ctx, sid, object, method, args, &result->buf); 

			pthread_mutex_lock(&self->lock); 
			avl_delete(&self->requests, &req->avl); 
			pthread_mutex_unlock(&self->lock); 
			free(req->name); 
			free(req); 
		} else {
			DEBUG("Could not parse call message!\n"); 
			blob_put_string(&result->buf, "error"); 
			blob_offset_t o = blob_open_table(&result->buf); 
			blob_put_string(&result->buf, "code"); 
			blob_put_int(&result->buf, -EINVAL); 
			blob_put_string(&result->buf, "str"); 
			blob_put_string(&result->buf, "Invalid call message format!"); 
			blob_close_table(&result->buf, o); 
		}
	} else if(rpc_method && strcmp(rpc_method, "list") == 0){
		const char *path = "*"; 	
		if(rpcmsg_parse_list_params(params, &sid, &path)){
			blob_put_string(&result->buf, "result"); 
			orange_list(self->ctx, sid, path, &result->buf); 
		}
	} else if(rpc_method && strcmp(rpc_method, "challenge") == 0){
		blob_put_string(&result->buf, "result"); 
		blob_offset_t o = blob_open_table(&result->buf); 
		blob_put_string(&result->buf, "token"); 
		char token[32]; 
		snprintf(token, sizeof(token), "%08x", msg->peer); //TODO: make hash
		blob_put_string(&result->buf, token);  
		blob_close_table(&result->buf, o); 
	} else if(rpc_method && strcmp(rpc_method, "login") == 0){
		// TODO: make challenge response work. Perhaps use custom pw database where only sha1 hasing is used. 
		const char *username = NULL, *response = NULL; 
		struct orange_sid _sid; 

		char token[32]; 
		snprintf(token, sizeof(token), "%08x", msg->peer); //TODO: make hash

		if(rpcmsg_parse_login(params, &username, &response)){
			if(orange_login(self->ctx, username, token, response, &_sid) == 0){
				blob_put_string(&result->buf, "result"); 
				blob_offset_t o = blob_open_table(&result->buf); 
				blob_put_string(&result->buf, "success"); 
				blob_put_string(&result->buf, _sid.hash); 
				blob_close_table(&result->buf, o); 
			} else {
				blob_put_string(&result->buf, "error"); 
				blob_offset_t o = blob_open_table(&result->buf); 
				blob_put_string(&result->buf, "code"); 
				blob_put_string(&result->buf, "EACCESS"); 
				blob_close_table(&result->buf, o); 
			}	
		} else {
			blob_put_string(&result->buf, "error"); 
			blob_offset_t o = blob_open_table(&result->buf); 
			blob_put_string(&result->buf, "code"); 
			blob_put_string(&result->buf, "EINVAL"); 
			blob_close_table(&result->buf, o); 
			DEBUG("Could not parse login parameters!\n"); 
		}
	} else if(rpc_method && strcmp(rpc_method, "logout") == 0){
		const char *_sid = NULL; 
		if(rpcmsg_parse_authenticate(params, &_sid) && orange_logout(self->ctx, _sid) == 0){
			blob_put_string(&result->buf, "result"); 
			blob_offset_t o = blob_open_table(&result->buf); 
				blob_put_string(&result->buf, "success"); 
				blob_put_string(&result->buf, "VALID"); 
			blob_close_table(&result->buf, o); 
		} else {
			blob_put_string(&result->buf, "error"); 
			blob_put_string(&result->buf, "Could not logout!"); 
		}
	} else {
		blob_put_string(&result->buf, "error"); 
		blob_offset_t o = blob_open_table(&result->buf); 
			blob_put_string(&result->buf, "code"); 
			blob_put_int(&result->buf, -EINVAL); 
			blob_put_string(&result->buf, "str"); 
			blob_put_string(&result->buf, "Invalid Method"); 
		blob_close_table(&result->buf, o); 
	}	

	blob_close_table(&result->buf, t); 

	if(orange_debug_level >= JUCI_DBG_TRACE){
		DEBUG("sending back: "); 
		blob_field_dump_json(blob_field_first_child(blob_head(&result->buf))); 
	}

	orange_server_send(self->server, &result); 		
	orange_message_delete(&msg); 

	return 0; 
}

#ifdef CONFIG_THREADS
static void *_request_dispatcher(void *ptr){
	struct orange_rpc *self = (struct orange_rpc*)ptr; 
	prctl(PR_SET_NAME, "request_dispatcher"); 
	pthread_mutex_lock(&self->lock); 
	while(!self->shutdown){
		sem_wait(&self->sem_bw); 
		pthread_mutex_unlock(&self->lock); 
		orange_rpc_process_requests(self);
		pthread_mutex_lock(&self->lock); 
		sem_post(&self->sem_bw); 
	}
	DEBUG("rpc request dispatcher exiting..\n"); 
	pthread_mutex_unlock(&self->lock); 
	pthread_exit(0); 
	return NULL; 
}

// NOTE: there is always a possibility that some errornous script will go into an infinite loop and subsequent calls to it will consume all our worker threads. 
// this is highly critical so we need to monitor for it
static void *_request_monitor(void *ptr){
	struct orange_rpc *self = (struct orange_rpc*)ptr; 
	prctl(PR_SET_NAME, "request_monitor"); 
	pthread_mutex_lock(&self->lock); 
	while(!self->shutdown){
		struct request_record *req, *tmp; 
		int hanged = 0; 
		avl_for_each_element_safe(&self->requests, req, avl, tmp){
			if(timespec_expired(&req->ts_expired)){
				syslog(LOG_CRIT, "request %s may have hanged. You can ignore this message if this is expected.", req->name); 
				DEBUG("Request %s may have hanged!\n", req->name); 
				hanged++; 
			}
		}

		// we probably don't even need to use a semaphore for this..
		#if 0
		// NOTE: this is so far just a possible solution (it has several problems still)
		int sem = 0; 
		sem_getvalue(&self->sem_bw, &sem); 
		DEBUG("monitor: free slots: %d, hanged threads: %d\n", sem, hanged);  
		if(sem == 0 && hanged == (int)self->num_workers){
			// we have a bunch of hanged calls and no free slots so this is bad
			syslog(LOG_CRIT, "no free request slots left. All %d slots are busy! restarting server..", self->num_workers); 
			FILE *cf = fopen("/proc/self/cmdline", "r"); 
			char buf[255]; 
			int len = fread(buf, 1, sizeof(buf), cf); 
			int argc = 0; 
			char **argv = NULL; 
			for(int c = 0; c < len; c++){
				if(buf[c] == 0) argc++; 
			}
			if(argc > 0){
				argv = malloc(sizeof(char*) * argc + 1); 
				memset(argv, 0, sizeof(char*) * argc + 1);  
				int arg = 0; 
				char *ptr = argv[arg++] = buf; 
				for(int c = 0; c < len; c++){
					if(buf[c] == 0) {
						while(buf[c] == 0 && c < len) c++; 
						argv[arg++] = buf + c; 
					}
				}
			}
			// FIXME: close all file descriptors
			/*for(int c = 3; c < 1024; c++){
				close(c); 
			}*/
			printf("restarting process with args: "); 
			char **a = argv; 
			while(*a){ printf("%s ", *a); a++; }
			printf("\n"); 

			execv("/proc/self/exe", argv); 
			// never get here. 
		}
#endif
		pthread_mutex_unlock(&self->lock); 
		sleep(5); 
		pthread_mutex_lock(&self->lock); 
	}
	pthread_mutex_unlock(&self->lock); 
	pthread_exit(0); 
	return NULL; 
}
#endif 

void orange_rpc_init(struct orange_rpc *self, orange_server_t server, struct orange *ctx, unsigned long long timeout_us, unsigned int num_workers){
	self->server = server; 
	self->ctx = ctx; 
	self->timeout_us = timeout_us; 
	self->shutdown = 0; 
	self->num_workers = num_workers; 
	avl_init(&self->requests, avl_strcmp, true, NULL);

	if(num_workers == 0) num_workers = 1; 
	self->threads = malloc(num_workers * sizeof(pthread_t)); 
	memset(self->threads, 0, num_workers * sizeof(pthread_t)); 

	pthread_mutex_init(&self->lock, NULL); 
	sem_init(&self->sem_bw, false, num_workers); 

	#if CONFIG_THREADS
	// start threads that will be handling rpc messages
	for(unsigned int c = 0; c < num_workers; c++){
		pthread_create(&self->threads[c], NULL, _request_dispatcher, self); 
	}

	// start a monitor thread to check periodically if we are running out of workers
	pthread_create(&self->monitor, NULL, _request_monitor, self); 
	#endif
}

void orange_rpc_deinit(struct orange_rpc *self){
	pthread_mutex_lock(&self->lock); 
	self->shutdown = 1; 
	pthread_mutex_unlock(&self->lock); 
	for(unsigned c = 0; c < self->num_workers; c++){
		pthread_join(self->threads[c], NULL); 
	}
	pthread_join(self->monitor, NULL); 
	pthread_mutex_destroy(&self->lock); 
	free(self->threads); 
}

//bool orange_rpc_running(struct orange_rpc *self){
	//return self->num_workers > 0; 
//}
