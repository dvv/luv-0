#ifndef _LUV_H
#define _LUV_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define container_of(ptr, type, member) \
  ((type *) ((char *) (ptr) - offsetof(type, member)))

#include "uv.h"
#include "http_parser.h"

typedef void (*callback_data_t)(const char *data);
typedef void (*callback_t)(int status);

typedef struct {
  int upgrade;
  int should_keep_alive;
  const char *url;
  const char *pathname;
  const char *query;
} http_t;

typedef struct {
  uv_tcp_t handle;
  http_parser parser;
  http_t http;
  callback_data_t on_read;
  callback_t on_close;
} client_t;

typedef struct {
  uv_write_t req;
  callback_t cb;
} client_write_req_t;

#endif

