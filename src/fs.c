#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>

#include "uv.h"

typedef void (*callback_t)(int status);
typedef void (*progress_t)(int status);

typedef struct read_s {
  uv_loop_t *loop;
  uv_fs_t rq;
  uv_file fd;
  callback_t on_end;
  callback_t on_progress;
  size_t offset;
  size_t size;
  size_t CHUNK_SIZE;
  char buf[0];
} read_t;

static uv_err_t last_err()
{
  return uv_last_error(uv_default_loop());
}

static void on_close(uv_fs_t *rq)
{
  read_t *read = rq->data;
  uv_fs_req_cleanup(rq);
printf("CLOSE %d %p\n", rq->result, read->on_end);
  if (read->on_end) {
    read->on_end(last_err().code);
  }
  free(rq->data);
}

static void on_read(uv_fs_t *rq)
{
  read_t *read = rq->data;
  uv_fs_req_cleanup(rq);
printf("READ %d %p\n", rq->result, read->on_end);
  if (rq->result <= 0) {
    uv_fs_close(read->loop, rq, read->fd, on_close);
  } else {
    size_t nread = rq->result;
    read->offset += nread;
    read->size -= nread;
    uv_fs_read(read->loop, rq, read->fd, &read->buf, read->CHUNK_SIZE, read->offset, on_read);
  }
}

static void on_open(uv_fs_t *rq)
{
  read_t *read = rq->data;
printf("OPEN %d %p\n", rq->result, read->on_end);
  if (rq->result == -1) {
    on_close(rq);
  } else {
    read->fd = rq->result;
    uv_fs_req_cleanup(rq);
    size_t len = read->size < read->CHUNK_SIZE ? read->size : read->CHUNK_SIZE;
    uv_fs_read(read->loop, rq, read->fd, &read->buf, len, read->offset, on_read);
  }
}

void stream_file(
    uv_loop_t *loop,
    const char *path,
    size_t offset,
    size_t size,
    progress_t on_progress,
    callback_t on_end,
    size_t CHUNK_SIZE
  )
{
  if (!CHUNK_SIZE) CHUNK_SIZE = 4096;
  read_t *read = calloc(1, sizeof(*read) + CHUNK_SIZE);
  read->rq.data = read;
  read->loop = loop;
  read->offset = offset;
  read->size = size;
  read->on_progress = on_progress;
  read->on_end = on_end;
  read->CHUNK_SIZE = CHUNK_SIZE;
printf("OPEN? %p\n", read->on_end);
  int rc = uv_fs_open(read->loop, &read->rq, path, O_RDONLY, 0644, on_open);
printf("OPEN! %d %p\n", rc, read->on_end);
}

void on_end(int status)
{
  printf("DONE %d\n", status);
}

int main()
{
  stream_file(uv_default_loop(), "fs.c", 0, -1, NULL, on_end, 100);
  uv_run(uv_default_loop());
  return 0;
}
