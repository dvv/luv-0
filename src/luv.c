#include <assert.h>
#include <signal.h>
#include <fcntl.h>

#include <lua.h>
#include <lauxlib.h>

#include "luv.h"

static uv_loop_t *loop;

#define FIX_EMFILE 0
#define DELAY_RESPONSE 1
#define SEPARATE_WRITE_HEAD 1

/******************************************************************************/
/* utility
/******************************************************************************/

static int CHECK(const char *msg, int status) {
  if (status == -1) {
    uv_err_t err = uv_last_error(loop);
    fprintf(stderr, "%s: %s\n", msg, uv_strerror(err));
    exit(-1);
  }
  return status;
}

/******************************************************************************/
/* TCP client methods
/******************************************************************************/

// set close timeout
static void client_timeout(client_t *self, uint64_t timeout)
{
  //DEBUGF("TIMEOUT %p %d", self, (int)timeout);
  if (!timeout) {
    uv_timer_stop(&self->timer_timeout);
  } else {
    uv_timer_start(&self->timer_timeout, client_on_timeout, timeout, 0);
  }
}

// async: write is done
static void client_after_write(uv_write_t *rq, int status)
{
  callback_t cb = rq->data;
  if (cb) {
    cb(status ? uv_last_error(loop).code : 0);
  }
  req_free((uv_req_t *)rq);
}

// write some buffers to the client
static int client_write(client_t *self, uv_buf_t *buf, int nbuf, callback_t cb)
{
  assert(self);
  uv_stream_t *handle = (uv_stream_t *)&self->handle;
  // do not write to closed stream
  if (handle->flags & (UV_CLOSING | UV_CLOSED | UV_SHUTTING | UV_SHUT)) {
    if (cb) cb(UV_EPIPE);
    return;
  }
  // stop close timer
  client_timeout(self, 0);
  // create write request
  uv_write_t *rq = (uv_write_t *)req_alloc();
  rq->data = cb; // memo cb, call it in client_after_write
  // write buffers
  return CHECK("write", uv_write(rq, handle, buf, nbuf,client_after_write));
}

// async: close is done
static void client_after_close(uv_handle_t *handle)
{
  client_t *self = handle->data;
  // fire 'close' event
  EVENT(self, EVT_CLOSE, uv_last_error(loop).code, NULL);
  // free allocations
  DEBUGF("CFREE %p", self);
  client_free(self);
}

// shutdown and close the client
static void client_close(client_t *self)
{
  // stop close timer
  client_timeout(self, 0);
  // close the handle
  uv_close((uv_handle_t *)&self->handle, client_after_close);
}

// async: shutdown is done
static void client_after_shutdown(uv_shutdown_t *rq, int status)
{
  client_t *self = rq->data;
  // fire 'end' event
  EVENT(self, EVT_END, uv_last_error(loop).code, NULL);
  // close the handle
  client_close(self);
  // free rq
  req_free((uv_req_t *)rq);
}

// shutdown and close the client
static void client_shutdown(client_t *self)
{
  // stop close timer
  client_timeout(self, 0);
  // flush write queue
  uv_shutdown_t *rq = (uv_shutdown_t *)req_alloc();
  rq->data = self;
  uv_shutdown(rq, (uv_stream_t *)&self->handle, client_after_shutdown);
}

// async: client close timer expired
static void client_on_timeout(uv_timer_t *timer, int status)
{
  client_t *self = (client_t *)timer->data;
  DEBUGF("TIMEDOUT %p", self);
  // close the client
  client_close(self);
}

/******************************************************************************/
/* HTTP response methods
/******************************************************************************/

// write headers to the client
static int response_write_head(msg_t *self, char *data, callback_t cb)
{
  assert(!self->headers_sent);
  uv_buf_t buf = { base: data, len: strlen(data) };
  self->headers_sent = 1;
  return client_write(self->client, &buf, 1, cb);
}

// write data to the client
static int response_write(msg_t *self, char *data, callback_t cb)
{
  assert(self->headers_sent);
  uv_buf_t buf = { base: data, len: strlen(data) };
  return client_write(self->client, &buf, 1, cb);
}

// finalize the response
static void response_end(msg_t *self)
{
  // client is keep-alive, set keep-alive timeout upon request completion
  assert(self->headers_sent);
  if (self->should_keep_alive) {
    client_timeout(self->client, 75);
  } else {
    client_shutdown(self->client);
  }
  // reset
  DEBUGF("RFREE %p", self);
  msg_free(self);
}

/******************************************************************************/
/* custom stuff
/******************************************************************************/

#define RESPONSE_HEAD \
  "HTTP/1.1 200 OK\r\n" \
  "Content-Length: 6\r\n" \
  "\r\n"

#define RESPONSE_BODY \
  "Hello\n"

static void after_write(int status)
{
  DEBUGF("after_write: %d", status);
}

// async: client close timer expired
static void request_on_timeout(uv_timer_t *timer, int status)
{
  msg_t *self = timer->data;
#if SEPARATE_WRITE_HEAD
  response_write_head(self, RESPONSE_HEAD, after_write);
  response_write(self, RESPONSE_BODY, after_write);
#else
  response_write_head(self, RESPONSE_HEAD RESPONSE_BODY, after_write);
#endif
  response_end(self);
}

static void request_on_event(msg_t *self, enum event_t ev, int status, void *data)
{
  static uv_buf_t refbuf[] = {{
    base : "hel",
    len  : 3,
  }, {
    base : "lo\n",
    len  : 3,
  }};
  if (ev == EVT_DATA) {
    DEBUGF("RDATA %p %*s", self, status, (char *)data);
  } else if (ev == EVT_END) {
    DEBUGF("REND %p", self);
#if DELAY_RESPONSE
    uv_timer_init(loop, &self->timer_wait);
    self->timer_wait.data = self;
    uv_timer_start(&self->timer_wait, request_on_timeout, DELAY_RESPONSE, 0);
#else
# if SEPARATE_WRITE_HEAD
    response_write_head(self, RESPONSE_HEAD, after_write);
    response_write(self, RESPONSE_BODY, after_write);
# else
    response_write_head(self, RESPONSE_HEAD RESPONSE_BODY, after_write);
# endif
    response_end(self);
#endif
  } else if (ev == EVT_ERROR) {
    DEBUGF("RERROR %p %d %s", self, status, (char *)data);
  }
}

static void client_on_event(client_t *self, enum event_t ev, int status, void *data)
{
  if (ev == EVT_REQUEST) {
  } else if (ev == EVT_END) {
    DEBUGF("CEND %p", self);
  } else if (ev == EVT_CLOSE) {
    DEBUGF("CCLOSE %p", self);
  } else if (ev == EVT_ERROR) {
    DEBUGF("CERROR %p %d %s", self, status, (char *)data);
  }
}

/******************************************************************************/
/* HTTP parser
/******************************************************************************/

static int message_begin_cb(http_parser *parser)
{
  client_t *client = parser->data;
  assert(client);
  // allocate first message
  msg_t *msg = msg_alloc();
  assert(msg);
  client->msg = msg;
  memset(msg, 0, sizeof(*msg));
  msg->client = client;
  msg->on_event = request_on_event;
  DEBUGF("MESSAGE BEGIN %p %p", client, msg);
  return 0;
}

static int url_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = parser->data;
  assert(client);
  msg_t *msg = client->msg;
  assert(msg);
  //DEBUGF("MESSAGE URL %p %p", client, msg);
  // copy URL
  strncat(msg->heap, p, len);
  return 0;
}

static int header_field_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = parser->data;
  assert(client);
  msg_t *msg = client->msg;
  assert(msg);
  /*client_t *client = parser->data;
  assert(client);
  req_t *req = client->req;
  assert(req);
  int new = (parser->state == 47 && req->lheaders) ? 1 : 0; // s_header_field?
  //DEBUGF("HEADER FIELD for %p %d %d %d", req, parser->state, req->lheaders, strlen(req->headers + req->lheaders + new));
  strncat(req->headers + req->lheaders + new, p, len);
  req->lheaders += len + new;*/
  return 0;
}

static int header_value_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = parser->data;
  assert(client);
  msg_t *msg = client->msg;
  assert(msg);
  /*client_t *client = parser->data;
  assert(client);
  req_t *req = client->req;
  assert(req);
  int new = parser->state == 50 ? 1 : 0; // s_header_value?
  //DEBUGF("HEADER VALUE for %p %d %d %d", req, parser->state, req->lheaders, strlen(req->headers + req->lheaders + new));
  strncat(req->headers + req->lheaders + new, p, len);
  req->lheaders += len + new;*/
  return 0;
}

static int headers_complete_cb(http_parser *parser)
{
  client_t *client = parser->data;
  assert(client);
  msg_t *msg = client->msg;
  assert(msg);
  // copy parser info
  msg->method = http_method_str(parser->method);
  msg->should_keep_alive = http_should_keep_alive(parser);
  // message is keep-alive, stop close timer
  if (msg->should_keep_alive) {
    client_timeout(msg->client, 0);
  }
  /*DEBUGF("HEADERS COMPLETE %p %p %s %s %s %d",
      client, req, req->method, req->url, req->headers, req->should_keep_alive
    );*/
  /*const char *p = req->headers;
  while (*p) {
    DEBUGF("NAME %s", p);
    p += strlen(p) + 1;
    DEBUGF("VALUE %s", p);
    p += strlen(p) + 1;
  }*/
  // run 'request' handler
  EVENT(client, EVT_REQUEST, 0, NULL);
  return 0; // 1 to skip body!
}

static int body_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = parser->data;
  assert(client);
  msg_t *msg = client->msg;
  assert(msg);
  DEBUGF("MESSAGE BODY %p %p", client, msg);
  EVENT(msg, EVT_DATA, len, (void *)p);
  return 0;
}

static int message_complete_cb(http_parser *parser);

static http_parser_settings parser_settings = {
  on_message_begin    : message_begin_cb,
  on_url              : url_cb,
  on_header_field     : header_field_cb,
  on_header_value     : header_value_cb,
  on_headers_complete : headers_complete_cb,
  on_body             : body_cb,
  on_message_complete : message_complete_cb,
};

static int message_complete_cb(http_parser *parser)
{
  client_t *client = parser->data;
  assert(client);
  msg_t *msg = client->msg;
  assert(msg);
  DEBUGF("MESSAGE COMPLETE %p %p", client, msg);
  // fire 'end' event
  EVENT(msg, EVT_END, 0, NULL);
  // reset parser
  client->msg = NULL;
  http_parser_execute(parser, &parser_settings, NULL, 0);
  return 0;
}

/******************************************************************************/
/* HTTP client reader
/******************************************************************************/

static void client_on_read(uv_stream_t *handle, ssize_t nread, uv_buf_t buf)
{
  client_t *self = handle->data;
  assert(self);
  msg_t *msg = self->msg;

  //DEBUGF("READ %p %d %p %d", self, nread, req, req ? req->upgrade : 0);

  if (nread > 0) {
    // once in "upgrade" mode, the protocol is no longer HTTP
    // and data should bypass parser
    if (msg && msg->upgrade) {
      assert("junk" == NULL);
      EVENT(msg, EVT_DATA, nread, buf.base);
    } else {
      size_t nparsed = http_parser_execute(
          &self->parser, &parser_settings, buf.base, nread
        );
      if (nparsed < nread) {
        // reset parser
        http_parser_execute(&self->parser, &parser_settings, NULL, 0);
        // report parse error
        EVENT(msg, EVT_ERROR, UV_UNKNOWN, "parse error");
      }
    }
  // don't route empty chunks to the parser
  } else if (nread == 0) {
  // report read errors
  } else {
    uv_err_t err = uv_last_error(loop);
    DEBUGF("READERROR %p %d", self, err.code);
    // N.B. must close stream on read error, or libuv assertion fails
    client_close(self);
    if (err.code != UV_EOF && err.code != UV_ECONNRESET) {
      EVENT(msg, EVT_ERROR, err.code, NULL);
    }
  }

  buf_free(buf);
}

/******************************************************************************/
/* HTTP connection handler
/******************************************************************************/

#if FIX_EMFILE
static int reserved_fd = 0;
#endif

static void server_on_connection(uv_stream_t *self, int status)
{
#if FIX_EMFILE
  if (status && uv_last_error(loop).code == UV_EMFILE) {
    close(reserved_fd);
    reserved_fd = 0;
  }
#endif
  // allocate client
  client_t *client = client_alloc();
  assert(client);
  memset(client, 0, sizeof(*client));
  DEBUGF("OPEN %p", client);
  client->on_event = client_on_event; // must have
  client->handle.data = client;
  client->timer_timeout.data = client;
  client->parser.data = client;

  uv_timer_init(loop, &client->timer_timeout);
  // TODO: de-hardcode
  // set client initial inactivity timeout to 60 seconds
  client_timeout(client, 60000);

  // accept client
  uv_tcp_init(loop, &client->handle);
  // TODO: EMFILE trick!
  // https://github.com/joyent/libuv/blob/master/src/unix/ev/ev.3#L1812-1816
  CHECK("accept", uv_accept(self, (uv_stream_t *)&client->handle));
#if FIX_EMFILE
  if (reserved_fd == 0) {
    uv_close((uv_handle_t *)&client->handle, NULL);
    reserved_fd = open("/dev/null", O_RDONLY);
  }
#endif

  // initialize HTTP parser
  http_parser_init(&client->parser, HTTP_REQUEST);

  // start reading client
  uv_read_start((uv_stream_t *)&client->handle, buf_alloc, client_on_read);
}

/******************************************************************************/
/* HTTP server
/******************************************************************************/

int main()
{
#if FIX_EMFILE
  // TODO: https://github.com/joyent/node/blob/v0.4/lib/net.js#L928-935
  reserved_fd = open("/dev/null", O_RDONLY);
#endif
  loop = uv_default_loop();

  //signal(SIGPIPE, SIG_IGN);

  uv_stream_t server;
  uv_tcp_init(loop, (uv_tcp_t *)&server);
  // FIXME: de-hardcode
  int backlog_size = 128;
  struct sockaddr_in address = uv_ip4_addr("0.0.0.0", 8080);
  CHECK("bind", uv_tcp_bind((uv_tcp_t *)&server, address));
  CHECK("listen", uv_listen(&server, backlog_size, server_on_connection));

  // REPL?

  // block in the main loop
  uv_run(loop);
}
