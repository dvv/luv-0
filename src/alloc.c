#include <assert.h>

#include "luv.h"

/*
 * https://github.com/joyent/libuv/blob/master/test/benchmark-pump.c#L294-326
 */


/*
 * Request allocator
 */

typedef struct req_list_s {
  union uv_any_req uv_req;
  struct req_list_s *next;
} req_list_t;

static req_list_t *req_freelist = NULL;

uv_req_t *req_alloc() {
  req_list_t *req;

  req = req_freelist;
  if (req != NULL) {
    req_freelist = req->next;
    return (uv_req_t *) req;
  }

  req = (req_list_t *)malloc(sizeof *req);
  return (uv_req_t *)req;
}

void req_free(uv_req_t *uv_req) {
  req_list_t *req = (req_list_t *)uv_req;

  req->next = req_freelist;
  req_freelist = req;
}

/*
 * Buffer allocator
 */

typedef struct buf_list_s {
  uv_buf_t uv_buf_t;
  struct buf_list_s *next;
} buf_list_t;

static buf_list_t *buf_freelist = NULL;

uv_buf_t buf_alloc(uv_handle_t *handle, size_t size) {
  buf_list_t *buf;

  buf = buf_freelist;
  if (buf != NULL) {
    buf_freelist = buf->next;
    return buf->uv_buf_t;
  }

  buf = (buf_list_t *) malloc(size + sizeof *buf);
  buf->uv_buf_t.len = (unsigned int)size;
  buf->uv_buf_t.base = ((char *) buf) + sizeof *buf;

  return buf->uv_buf_t;
}

void buf_free(uv_buf_t uv_buf_t) {
  buf_list_t *buf = (buf_list_t *) (uv_buf_t.base - sizeof *buf);

  buf->next = buf_freelist;
  buf_freelist = buf;
}

/*
 * HTTP message allocator
 */

typedef struct msg_list_s {
  msg_t msg;
  struct msg_list_s *next;
} msg_list_t;

static msg_list_t *msg_freelist = NULL;

static int NM = 0;

msg_t *msg_alloc() {
  msg_list_t *msg;

  msg = msg_freelist;
  if (msg != NULL) {
    msg_freelist = msg->next;
    return (msg_t *)msg;
  }

  msg = (msg_list_t *)malloc(sizeof *msg);
  printf("MSGALLOC %d\n", ++NM);
  return (msg_t *)msg;
}

void msg_free(msg_t *msg) {
  msg_list_t *m = (msg_list_t *)msg;

  m->next = msg_freelist;
  msg_freelist = m;
}
