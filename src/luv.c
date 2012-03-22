#include <assert.h>
#include <signal.h>
#include <fcntl.h>

#include <lua.h>
#include <lauxlib.h>

#include "luv.h"

static uv_loop_t *loop;

/******************************************************************************/
/* utility
/******************************************************************************/

static void *zmalloc(size_t n)
{
  void *r = malloc(n);
  memset(r, 0, n);
  return r;
}

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
static void client_after_write(uv_write_t *req, int status)
{
  client_write_req_t *write_req = container_of(req, client_write_req_t, req);
  if (write_req->cb) {
    write_req->cb(status ? uv_last_error(loop).code : 0);
  }
  free(write_req);
}

// write some buffers to the client
static int client_write(client_t *self, uv_buf_t *buf, int nbuf, callback_t cb)
{
  assert(self);
  assert(!self->closed);
  // stop close timer
  client_timeout(self, 0);
  // create write request
  client_write_req_t *write_req = zmalloc(sizeof(*write_req));
  write_req->req.data = write_req;
  write_req->cb = cb; // memo cb, call it in client_after_write
  // write buffers
  return CHECK("write", uv_write(
      &write_req->req, (uv_stream_t *)&self->handle, buf, nbuf,
      client_after_write
    ));
}

// async: close is done
static void client_after_close(uv_handle_t *handle)
{
  client_t *self = handle->data;
  // fire 'close' event
  self->on_event(self, EVT_CLOSE, uv_last_error(loop).code, NULL);
  // free allocations
  // TODO: free self->req and its fields
  DEBUGF("CFREE %p", self);
  free(self);
}

// shutdown and close the client
static void client_close(client_t *self)
{
  // stop close timer
  client_timeout(self, 0);
  // flush write queue
  self->closed = 1;
  // close the handle
  uv_close((uv_handle_t *)&self->handle, client_after_close);
}

// async: shutdown is done
static void client_after_shutdown(uv_shutdown_t *rq, int status)
{
  client_t *self = rq->data;
  // fire 'end' event
  self->on_event(self, EVT_END, uv_last_error(loop).code, NULL);
  // close the handle
  client_close(self);
  // free rq
  free(rq);
}

// shutdown and close the client
static void client_shutdown(client_t *self)
{
  // stop close timer
  client_timeout(self, 0);
  // flush write queue
  self->closed = 1;
  uv_shutdown_t *rq = zmalloc(sizeof(*rq));
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
static int response_write_head(req_t *self, char *data, callback_t cb)
{
  assert(!self->headers_sent);
  uv_buf_t buf = { base: data, len: strlen(data) };
  self->headers_sent = 1;
  return client_write(self->client, &buf, 1, cb);
}

// write data to the client
static int response_write(req_t *self, char *data, callback_t cb)
{
  assert(self->headers_sent);
  uv_buf_t buf = { base: data, len: strlen(data) };
  return client_write(self->client, &buf, 1, cb);
}

// finalize the response
static void response_end(req_t *self)
{
  // client is keep-alive, set keep-alive timeout upon request completion
  assert(self->headers_sent);
  if (self->should_keep_alive) {
    client_timeout(self->client, 75);
  } else {
    client_shutdown(self->client);
  }
  // reset
  assert(!self->_freed);
  self->_freed = 1;
  DEBUGF("RFREE %p", self);
  //free((void *)self->headers);
  //free((void *)self->url);
  free((void *)self);
}

/******************************************************************************/
/* custom stuff
/******************************************************************************/

#define RESPONSE_HEAD \
  "HTTP/1.1 200 OK\r\n" \
  "Connection: Keep-Alive\r\n" \
  "Content-Type: text/plain\r\n" \
  "Content-Length: 6\r\n" \
  "\r\n"

#define RESPONSE_BODY \
  "hello\n"

static void request_on_event(req_t *self, enum event_t ev, int status, void *data)
{
  void after_write(int status)
  {
    DEBUGF("after_write: %d", status);
  }
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
    response_write_head(self, RESPONSE_HEAD, after_write);
    response_write(self, RESPONSE_BODY, after_write);
    response_end(self);
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
  // allocate request
  client_t *client = parser->data;
  assert(client);
  req_t *req = zmalloc(sizeof(*req));
  assert(req);
  client->req = req;
  req->client = client;
  // assign request handler
  req->on_event = request_on_event; // must have
  // allocate request headers
  //req->headers = zmalloc(1024);
    // TODO: store (len, chunk) tuples, no strcat()
  //req->lheaders = 0;
  // client is keep-alive, stop close timer on new request
  // FIXME: can be indeterminate here
  if (http_should_keep_alive(parser)) {
    client_timeout(client, 0);
  }
  DEBUGF("MESSAGE BEGIN %p %p", client, req);
  return 0;
}

static int url_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = parser->data;
  assert(client);
  req_t *req = client->req;
  assert(req);
  //req->url = strndup(p, len);
  //DEBUGF("MESSAGE URL %p %p", client, req);
  return 0;
}

static int header_field_cb(http_parser *parser, const char *p, size_t len)
{
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
  req_t *req = client->req;
  assert(req);
  // copy parser info
  req->method = http_method_str(parser->method);
  req->should_keep_alive = http_should_keep_alive(parser);
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
  client->on_event(client, EVT_REQUEST, 0, NULL);
  return 0;
}

static int body_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = parser->data;
  assert(client);
  req_t *req = client->req;
  assert(req);
  DEBUGF("MESSAGE BODY %p %p", client, req);
  req->on_event(req, EVT_DATA, len, (void *)p);
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
  req_t *req = client->req;
  assert(req);
  DEBUGF("MESSAGE COMPLETE %p %p", client, req);
  // fire 'end' event
  req->on_event(req, EVT_END, 0, NULL);
  // reset parser
  http_parser_execute(parser, &parser_settings, NULL, 0);
  return 0;
}

/******************************************************************************/
/* HTTP connection handler
/******************************************************************************/

static uv_buf_t client_on_alloc(uv_handle_t *handle, size_t suggested_size)
{
  uv_buf_t buf;
  buf.base = zmalloc(suggested_size);
  buf.len = suggested_size;
  return buf;
}

static void client_on_read(uv_stream_t *handle, ssize_t nread, uv_buf_t buf)
{
  client_t *self = handle->data;
  assert(self);
  assert(!self->closed);
  req_t *req = self->req;

  //DEBUGF("READ %p %d %p %d", self, nread, req, req ? req->upgrade : 0);

  if (nread > 0) {
    // once in "upgrade" mode, the protocol is no longer HTTP
    // and data should bypass parser
    CHECK("junk", req && req->_freed > 1);
    if (req && req->upgrade) {
      req->on_event(req, EVT_DATA, nread, buf.base);
    } else {
      size_t nparsed;
      nparsed = http_parser_execute(
          &self->parser, &parser_settings, buf.base, nread
        );
      if (nparsed < nread) {
        // reset parser
        http_parser_execute(&self->parser, &parser_settings, NULL, 0);
        // report parse error
        req->on_event(req, EVT_ERROR, UV_UNKNOWN, "parse error");
      }
    }
  // don't route empty chunks to the parser
  } else if (nread == 0) {
  // report read errors
  } else {
    uv_err_t err = uv_last_error(loop);
    DEBUGF("READERROR %p %d", self, err.code);
    client_close(self);
    if (err.code != UV_EOF && err.code != UV_ECONNRESET) {
      req->on_event(req, EVT_ERROR, err.code, NULL);
    }
  }

  free(buf.base);
}

static int reserved_fd = 0;

static void server_on_connection(uv_stream_t *self, int status)
{
  if (status && uv_last_error(loop).code == UV_EMFILE) {
    close(reserved_fd);
    reserved_fd = 0;
  }

  // allocate client
  client_t *client = zmalloc(sizeof(*client));
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
  if (reserved_fd == 0) {
    uv_close((uv_handle_t *)&client->handle, NULL);
    reserved_fd = open("/dev/null", O_RDONLY);
  }

  // initialize HTTP parser
  http_parser_init(&client->parser, HTTP_REQUEST);

  // start reading client
  uv_read_start((uv_stream_t *)&client->handle, client_on_alloc, client_on_read);
}

/******************************************************************************/
/* HTTP server
/******************************************************************************/

int main()
{
  reserved_fd = open("/dev/null", O_RDONLY);

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
