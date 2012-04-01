#ifndef _LUV_COMMON_H
#define _LUV_COMMON_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define container_of(ptr, type, member) \
  ((type *) ((char *) (ptr) - offsetof(type, member)))

#include "uv.h"

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
  EVT_MAX
};

#define WRITABLE(handle) (!((handle)->flags & (UV_CLOSING | UV_CLOSED | UV_SHUTTING | UV_SHUT)))
#define CLOSABLE(handle) (!((handle)->flags & (UV_CLOSING | UV_CLOSED)))

#define EVENT(self, params...) (self)->on_event((self), params)

#ifdef DEBUG
# define DEBUGF(fmt, params...) fprintf(stderr, fmt "\n", params)
#else
# define DEBUGF(fmt, params...) do {} while (0)
#endif

#endif
