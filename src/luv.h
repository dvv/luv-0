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

/* flags */
enum {
  UV_CLOSING       = 0x01,   /* uv_close() called but not finished. */
  UV_CLOSED        = 0x02,   /* close(2) finished. */
  UV_READING       = 0x04,   /* uv_read_start() called. */
  UV_SHUTTING      = 0x08,   /* uv_shutdown() called but not complete. */
  UV_SHUT          = 0x10,   /* Write side closed. */
  UV_READABLE      = 0x20,   /* The stream is readable */
  UV_WRITABLE      = 0x40,   /* The stream is writable */
  UV_TCP_NODELAY   = 0x080,  /* Disable Nagle. */
  UV_TCP_KEEPALIVE = 0x100,  /* Turn on keep-alive. */
  UV_TIMER_ACTIVE  = 0x080,
  UV_TIMER_REPEAT  = 0x100
};

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
typedef struct msg_s msg_t;

struct msg_s {
  client_t *client;
  const char *method;
  int upgrade;
  int should_keep_alive;
  uint8_t headers_sent : 1;
  uv_timer_t timer_wait;
  char heap[4096]; // collect url and headers
  void (*on_event)(msg_t *self, enum event_t ev, int status, void *data);
};

struct client_s {
  uv_tcp_t handle;
  uv_timer_t timer_timeout; // inactivity close timer
  http_parser parser;
  msg_t *msg;
  void (*on_event)(client_t *self, enum event_t ev, int status, void *data);
};

#define EVENT(self, params...) (self)->on_event((self), params)

#if 0
# define DEBUG(fmt) fprintf(stderr, fmt "\n")
# define DEBUGF(fmt, params...) fprintf(stderr, fmt "\n", params)
#else
# define DEBUG(fmt) do {} while (0)
# define DEBUGF(fmt, params...) do {} while (0)
#endif

static void client_timeout(client_t *self, uint64_t timeout);
static void client_on_timeout(uv_timer_t *timer, int status);

uv_req_t *req_alloc();
void req_free(uv_req_t *uv_req);
uv_buf_t buf_alloc(uv_handle_t *handle, size_t size);
void buf_free(uv_buf_t uv_buf_t);
msg_t *msg_alloc();
void msg_free(msg_t *msg);

#endif
