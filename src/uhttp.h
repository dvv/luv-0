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
  EVT_OPEN,
  EVT_REQUEST,
  EVT_DATA,
  EVT_END,
  EVT_SHUT,
  EVT_CLOSE,
};

typedef struct client_s client_t;
typedef struct msg_s msg_t;

typedef void (*callback_data_t)(const char *data);
typedef void (*callback_t)(int status);
typedef void (*event_cb)(client_t *self, msg_t *msg, enum event_t ev, int status, void *data);

struct msg_s {
  client_t *client;
  const char *method;
  int upgrade;
  int should_keep_alive;
  uint8_t headers_sent : 1;
  uv_timer_t timer_wait;
  size_t heap_len;
  char heap[4096]; // collect url and headers
};

struct client_s {
  uv_tcp_t handle;
  uv_timer_t timer_timeout; // inactivity close timer
  http_parser parser;
  msg_t *msg;
  event_cb on_event;
};

#define EVENT(self, params...) (self)->on_event((self), params)

#if 0
# define DEBUG(fmt) fprintf(stderr, fmt "\n")
# define DEBUGF(fmt, params...) fprintf(stderr, fmt "\n", params)
#else
# define DEBUG(fmt) do {} while (0)
# define DEBUGF(fmt, params...) do {} while (0)
#endif

uv_tcp_t *server_init(
    int port,
    const char *host,
    int backlog_size,
    event_cb on_event
  );

int response_write_head(msg_t *self, const char *data, callback_t cb);
int response_write(msg_t *self, const char *data, callback_t cb);
void response_end(msg_t *self);

#endif
