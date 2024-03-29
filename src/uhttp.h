#ifndef _LUV_HTTP_H
#define _LUV_HTTP_H

#include "common.h"
#include "http_parser.h"

typedef struct client_s client_t;
typedef struct msg_s msg_t;

typedef void (*callback_t)(int status);
typedef void (*event_cb)(client_t *self, msg_t *msg, enum event_t ev,
    int status, void *data);

struct msg_s {
  client_t *client;
  msg_t *prev, *next;
  const char *method;
  int upgrade : 1;
  int should_keep_alive : 1;
  int headers_sent : 1;
  int chunked : 1;
  int no_chunking : 1;
  int has_content_length : 1;
  int has_transfer_encoding : 1;
  int finished : 1;
  uv_buf_t heap;
  // TODO: reconsider? malloc/realloc?
  size_t nbufs;
  uv_buf_t bufs[128];
};

struct client_s {
  uv_tcp_t handle;
  uv_timer_t timer_timeout; // inactivity close timer
  http_parser parser;
  msg_t *msg; // current message http_parser deals with
  event_cb on_event;
  // LUA
  ////lua_State *L;
  uv_tcp_t *server;
};

uv_tcp_t *server_init(
    int port,
    const char *host,
    int backlog_size,
    event_cb on_event
  );

void response_write(msg_t *self, const char *data, size_t len);
void response_end(msg_t *self);

#endif
