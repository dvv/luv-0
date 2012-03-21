#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include "luv.h"

#define DEBUG(fmt) fprintf(stderr, fmt "\n")
//#define DEBUG(fmt, params...) fprintf(stderr, fmt "\n", params)

static uv_loop_t *loop;

static void CHECK(const char *msg, int status) {
  if (status == -1) {
    uv_err_t err = uv_last_error(loop);
    fprintf(stderr, "%s: %s\n", msg, uv_strerror(err));
    exit(-1);
  }
}

static http_parser_settings parser_settings;

static uv_buf_t refbuf;

#define RESPONSE \
  "HTTP/1.1 200 OK\r\n" \
  "Content-Type: text/plain\r\n" \
  "Content-Length: 6\r\n" \
  "\r\n" \
  "hello\n"



static void client_after_write(uv_write_t *req, int status) {
  client_write_req_t *write_req = container_of(req, client_write_req_t, req);
  if (write_req->cb) {
    write_req->cb(status);
  }
  free(write_req);
}

static int client_write(client_t *self, uv_buf_t *buf, int nbuf, callback_t cb) {
  client_write_req_t *write_req = malloc(sizeof(*write_req));
  write_req->req.data = write_req;
  write_req->cb = cb; // memo cb, call it in client_after_write
  return uv_write(&write_req->req, (uv_stream_t *)&self->handle, buf, nbuf, client_after_write);
}

static void client_after_close(uv_handle_t *handle) {
  client_t *client = container_of(handle, client_t, handle);
  if (client->on_close) {
    client->on_close(0);
  }
  free(handle);
}

static int client_close(client_t *client, callback_t cb) {
  client->on_close = cb;
  uv_close((uv_handle_t *)&client->handle, client_after_close);
}





static void after_write(int status) {
  fprintf(stderr, "after_write: %d\n", status);
}

static void after_close(int status) {
  fprintf(stderr, "after_close: %d\n", status);
}







static uv_buf_t client_on_alloc(uv_handle_t *handle, size_t suggested_size) {
  uv_buf_t buf;
  buf.base = malloc(suggested_size);
  buf.len = suggested_size;
  return buf;
}

static void client_on_read(uv_stream_t *handle, ssize_t nread, uv_buf_t buf) {
  client_t *client = container_of(handle, client_t, handle);
  size_t nparsed;

  // TODO: upgrade!

  if (nread >= 0) {
    nparsed = http_parser_execute(&client->parser, &parser_settings, buf.base, nread);
    if (nparsed < nread) {
      DEBUG("parse error, closing");
      client_close(client, after_close);
    }
  } else {
    uv_err_t err = uv_last_error(loop);
    if (err.code == UV_EOF) {
      client_close(client, after_close);
    } else {
      CHECK("read", nread);
    }
  }

  free(buf.base);
}

static void server_on_connection(uv_stream_t *server, int status) {
  DEBUG("connected");

  client_t *client = malloc(sizeof(*client));
  uv_tcp_init(loop, &client->handle);

  client->handle.data = client;
  CHECK("accept", uv_accept(server, (uv_stream_t *)&client->handle));

  client->parser.data = client;
  http_parser_init(&client->parser, HTTP_REQUEST);

  uv_read_start((uv_stream_t *)&client->handle, client_on_alloc, client_on_read);
}

int main() {

  int backlog_size = 128;

  uv_stream_t server;

  refbuf.base = RESPONSE;
  refbuf.len = sizeof(RESPONSE);

  parser_settings.on_headers_complete = on_headers_complete;

  loop = uv_default_loop();

  uv_tcp_init(loop, (uv_tcp_t *)&server);
  // FIXME: de-hardcode
  struct sockaddr_in address = uv_ip4_addr("0.0.0.0", 8080);
  CHECK("bind", uv_tcp_bind((uv_tcp_t *)&server, address));
  CHECK("listen", uv_listen(&server, backlog_size, server_on_connection));

  // REPL?

  // Block in the main loop
  uv_run(loop);
}
