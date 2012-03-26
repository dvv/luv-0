#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>

#include "uv.h"

typedef void (*callback_t)(int status);
typedef void (*callback_fs_t)(uv_fs_t *rq);
typedef void (*progress_t)(uv_fs_t *rq, const char *data, size_t len, callback_fs_t cb);

typedef struct read_s {
  uv_loop_t *loop;
  uv_fs_t rq;
  uv_file fd;
  callback_t on_end;
  progress_t on_progress;
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
  if (read->on_end) {
printf("CLOSE %d %p\n", rq->result, read->on_end);
    read->on_end(last_err().code);
  }
  free(rq->data);
}

static void on_read(uv_fs_t *rq);

static void on_write(uv_fs_t *rq)
{
  read_t *read = rq->data;
  size_t nread = rq->result;
  read->offset += nread;
  read->size -= nread;
  uv_fs_read(read->loop, rq, read->fd, &read->buf, read->CHUNK_SIZE, read->offset, on_read);
}

static void on_read(uv_fs_t *rq)
{
  read_t *read = rq->data;
  uv_fs_req_cleanup(rq);
printf("READ %d %p\n", rq->result, read->on_end);
  if (rq->result <= 0) {
    if (read->on_end) {
      read->on_end(last_err().code);
      read->on_end = NULL;
    }
    uv_fs_close(read->loop, rq, read->fd, on_close);
  } else {
    size_t nread = rq->result;
    if (read->on_progress) {
      read->on_progress(rq, read->buf, nread, on_write);
    } else {
      on_write(rq);
    }
  }
}

static void on_open(uv_fs_t *rq)
{
  read_t *read = rq->data;
  if (rq->result == -1) {
printf("OPENERR %d\n", last_err().code);
    if (read->on_end) {
      read->on_end(last_err().code);
      read->on_end = NULL;
    }
    on_close(rq);
  } else {
printf("OPEN %d %p\n", last_err().code, read->on_end);
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
  uv_fs_open(read->loop, &read->rq, path, O_RDONLY, 0644, on_open);
}

void on_end(int status)
{
  printf("END %d\n", status);
}

void on_progress(uv_fs_t *rq, const char *data, size_t len, callback_fs_t cb)
{
printf("DATA: %*s", len, data);
  cb(rq);
}

int main()
{
  stream_file(uv_default_loop(), "src/fs.c", 0, -1, on_progress, on_end, 100);
  uv_run(uv_default_loop());
  return 0;
}
