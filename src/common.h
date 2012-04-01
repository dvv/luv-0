#ifndef _LUV_COMMON_H
#define _LUV_COMMON_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define container_of(ptr, type, member) \
  ((type *) ((char *) (ptr) - offsetof(type, member)))

#include "uv.h"

/* events */
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

#define EVENT(self, params...) (self)->on_event((self), params)

#ifdef DEBUG
# define DEBUGF(fmt, params...) fprintf(stderr, fmt "\n", params)
#else
# define DEBUGF(fmt, params...) do {} while (0)
#endif

#endif
