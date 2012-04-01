#ifndef _LUV_HTTP_H
#define _LUV_HTTP_H

#include "common.h"
#include "http_parser.h"

typedef struct client_s client_t;
typedef struct msg_s msg_t;

typedef void (*callback_data_t)(const char *data);
typedef void (*callback_t)(int status);
typedef void (*event_cb)(client_t *self, msg_t *msg, enum event_t ev,
    int status, void *data);

struct msg_s {
  client_t *client;
  const char *method;
  int upgrade : 1;
  int should_keep_alive : 1;
  int should_pipeline : 1;
  int headers_sent : 1;
  int chunked : 1;
  int no_chunking : 1;
  int has_content_length : 1;
  int has_transfer_encoding : 1;
  size_t heap_len;
  // TODO: reconsider
  char heap[4096 + HTTP_MAX_HEADER_SIZE]; // collect url and headers
};

struct client_s {
  uv_tcp_t handle;
  uv_timer_t timer_timeout; // inactivity close timer
  http_parser parser;
  msg_t *msg; // current message http_parser deals with
  event_cb on_event;
  int closing : 1;
  int closed : 1;
};

uv_tcp_t *server_init(
    int port,
    const char *host,
    int backlog_size,
    event_cb on_event
  );

int response_write(msg_t *self, const char *data, size_t len, callback_t cb);
void response_end(msg_t *self, int close);

#endif
