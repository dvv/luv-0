#ifndef _LUV_H
#define _LUV_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define container_of(ptr, type, member) \
  ((type *) ((char *) (ptr) - offsetof(type, member)))

#include "uv.h"
#include "http_parser.h"

enum event_t {
  EVT_ERROR = 1,
  EVT_DATA,
  EVT_END,
  EVT_CLOSE,
  EVT_REQUEST,
};

typedef void (*callback_data_t)(const char *data);
typedef void (*callback_t)(int status);

typedef struct client_s client_t;
typedef struct req_s req_t;

struct req_s {
  client_t *client;
  const char *method;
  int upgrade;
  int should_keep_alive;
  const char *url;
  char *headers;
  size_t lheaders;
  uint8_t headers_sent : 1;
  void (*on_event)(req_t *self, enum event_t ev, int status, void *data);
};

struct client_s {
  uv_tcp_t handle;
  http_parser parser;
  req_t *req;
  uv_timer_t timer_timeout;
  void (*on_event)(client_t *self, enum event_t ev, int status, void *data);
};

typedef struct {
  uv_write_t req;
  callback_t cb;
} client_write_req_t;

#define DEBUG(fmt) fprintf(stderr, fmt "\n")
#define DEBUGF(fmt, params...) fprintf(stderr, fmt "\n", params)

static void client_active(client_t *self);
static void client_inactive(client_t *self, uint64_t timeout);
static void client_on_timeout(uv_timer_t *timer, int status);

#endif
