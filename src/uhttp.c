#include <assert.h>
#include <signal.h>
#include <fcntl.h>

#include "uhttp.h"

/******************************************************************************/
/* utility
/******************************************************************************/

static uv_err_t last_err()
{
  return uv_last_error(uv_default_loop());
}

static int CHECK(const char *msg, int status) {
  if (status == -1) {
    fprintf(stderr, "%s: %s\n", msg, uv_strerror(last_err()));
    exit(-1);
  }
  return status;
}

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

/*
 * HTTP client allocator
 */

typedef struct client_list_s {
  client_t client;
  struct client_list_s *next;
} client_list_t;

static client_list_t *client_freelist = NULL;

static int NC = 0;

client_t *client_alloc() {
  client_list_t *client;

  client = client_freelist;
  if (client != NULL) {
    client_freelist = client->next;
    return (client_t *)client;
  }

  client = (client_list_t *)malloc(sizeof *client);
  printf("CLIENTALLOC %d\n", ++NC);
  return (client_t *)client;
}

void client_free(client_t *client) {
  client_list_t *l = (client_list_t *)client;

  l->next = client_freelist;
  client_freelist = l;
}
/******************************************************************************/
/* TCP client methods
/******************************************************************************/

static void client_on_timeout(uv_timer_t *timer, int status);

// set close timeout. N.B. start/stop is idempotent
static void client_timeout(client_t *self, uint64_t timeout)
{
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
    cb(status ? last_err().code : 0);
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
  return uv_write(rq, handle, buf, nbuf,client_after_write);
}

// async: close is done
static void client_after_close(uv_handle_t *handle)
{
  client_t *self = handle->data;
  // fire 'close' event
  EVENT(self, self->msg, EVT_CLOSE, last_err().code, NULL);
  // dispose close timer
  uv_close((uv_handle_t *)&self->timer_timeout, NULL);
  // free self
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
  // fire 'shut' event
  EVENT(self, self->msg, EVT_SHUT, last_err().code, NULL);
  // close the handle
  client_close(self);
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
  // close the client
  client_close(self);
}

/******************************************************************************/
/* HTTP parser
/******************************************************************************/

static int message_begin_cb(http_parser *parser)
{
  client_t *client = parser->data;
  assert(client);
  // allocate message
  msg_t *msg = msg_alloc();
  assert(msg);
  client->msg = msg;
  memset(msg, 0, sizeof(*msg));
  msg->client = client;
  return 0;
}

static int url_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = parser->data;
  assert(client);
  msg_t *msg = client->msg;
  assert(msg);
  // memo URL
  // TODO: fix overflow!
  strncat(msg->heap, p, len);
  msg->heap_len += len;
  return 0;
}

static int header_field_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = parser->data;
  assert(client);
  msg_t *msg = client->msg;
  assert(msg);
  // memo header name
  // TODO: fix overflow!
  int new = (parser->state == 47 && msg->heap_len) ? 1 : 0; // s_header_field?
  {
  // lower case and append to the heap
  size_t i;
  char *s = msg->heap + msg->heap_len + new;
  for (i = 0; i < len; ++i) *s++ = tolower(p[i]);
  *s++ = '\0';
  }
  msg->heap_len += len + new;
  return 0;
}

static int header_value_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = parser->data;
  assert(client);
  msg_t *msg = client->msg;
  assert(msg);
  // memo header value
  // TODO: fix overflow!
  int new = parser->state == 50 ? 1 : 0; // s_header_value?
  // append to the heap
  strncat(msg->heap + msg->heap_len + new, p, len);
  msg->heap_len += len + new;
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
  // activate pipeline for idempotent methods
  switch (parser->method) {
    case HTTP_GET:
    case HTTP_HEAD:
    case HTTP_PUT:
    case HTTP_DELETE:
    // TODO: you name others
      msg->should_pipeline = 1;
  }
  // run 'request' handler
  EVENT(client, msg, EVT_REQUEST, 0, NULL);
  return 0; // 1 to skip body!
}

static int body_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = parser->data;
  assert(client);
  msg_t *msg = client->msg;
  assert(msg);
  // pump message body via 'data' events
  EVENT(client, msg, EVT_DATA, len, (void *)p);
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
  // fire 'end' event
  EVENT(client, msg, EVT_END, 0, NULL);
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

  if (nread > 0) {
    // once in "upgrade" mode, the protocol is no longer HTTP
    // and data should bypass parser
    if (msg && msg->upgrade) {
      assert("junk" == NULL);
      EVENT(self, msg, EVT_DATA, nread, buf.base);
    } else {
      size_t nparsed = http_parser_execute(
          &self->parser, &parser_settings, buf.base, nread
        );
      if (nparsed < nread) {
        // reset parser
        http_parser_execute(&self->parser, &parser_settings, NULL, 0);
        // report parse error
        EVENT(self, msg, EVT_ERROR, UV_UNKNOWN, "parse error");
      }
    }
  // don't route empty chunks to the parser
  } else if (nread == 0) {
  // report read errors
  } else {
    uv_err_t err = last_err();
    // N.B. must close stream on read error, or libuv assertion fails
    client_close(self);
    if (err.code != UV_EOF && err.code != UV_ECONNRESET) {
      EVENT(self, msg, EVT_ERROR, err.code, NULL);
    }
  }

  buf_free(buf);
}

/******************************************************************************/
/* HTTP connection handler
/******************************************************************************/

static void server_on_connection(uv_stream_t *self, int status)
{
  // allocate client
  client_t *client = client_alloc();
  assert(client);
  memset(client, 0, sizeof(*client));
  client->on_event = self->data;
  client->handle.data = client;
  client->timer_timeout.data = client;
  client->parser.data = client;
  EVENT(client, NULL, EVT_OPEN, 0, NULL);

  uv_timer_init(self->loop, &client->timer_timeout);
  // TODO: de-hardcode
  // set client initial inactivity timeout to 60 seconds
  client_timeout(client, 60000);

  // accept client
  uv_tcp_init(self->loop, &client->handle);
  // TODO: EMFILE trick!
  // https://github.com/joyent/libuv/blob/master/src/unix/ev/ev.3#L1812-1816
  CHECK("accept", uv_accept(self, (uv_stream_t *)&client->handle));

  // initialize HTTP parser
  http_parser_init(&client->parser, HTTP_REQUEST);

  // start reading client
  uv_read_start((uv_stream_t *)&client->handle, buf_alloc, client_on_read);
}

/******************************************************************************/
/* HTTP server
/******************************************************************************/

uv_tcp_t *server_init(
    int port,
    const char *host,
    int backlog_size,
    event_cb on_event
  )
{
  uv_tcp_t *server = calloc(1, sizeof(*server));
  server->data = on_event; // store message event handler
  uv_tcp_init(uv_default_loop(), server);
  struct sockaddr_in address = uv_ip4_addr(host, port);
  CHECK("bind", uv_tcp_bind(server, address));
  CHECK("listen",
      uv_listen((uv_stream_t *)server, backlog_size, server_on_connection)
  );
  return server;
}

/******************************************************************************/
/* HTTP response methods
/******************************************************************************/

/*
const char *STATUS_CODES[6][] = {
  {},
  { "Continue", "Switching Protocols", "Processing" },
  { "OK", "Created", "Accepted", "Non-Authoritative Information", "No Content",
    "Reset Content", "Partial Content", "Multi-Status" },
  { "Multiple Choices", "Moved Permanently", "Moved Temporarily", "See Other",
    "Not Modified", "Use Proxy", "306", "Temporary Redirect" },
  { "Bad Request", "Unauthorized", "Payment Required", "Forbidden", "Not Found",
    "Method Not Allowed", "Not Acceptable", "Proxy Authentication Required",
    "Request Time-out", "Conflict", "Gone", "Length Required",
    "Precondition Failed", "Request Entity Too Large", "Request-URI Too Large",
    "Unsupported Media Type", "Requested Range Not Satisfiable",
    "Expectation Failed", "I'm a teapot", "419", "420", "421",
    "Unprocessable Entity", "Locked", "Failed Dependency",
    "Unordered Collection", "Upgrade Required" },
  { "Internal Server Error", "Not Implemented", "Bad Gateway",
    "Service Unavailable", "Gateway Time-out", "HTTP Version not supported",
    "Variant Also Negotiates", "Insufficient Storage", "508",
    "Bandwidth Limit Exceeded", "Not Extended" }
};*/

// write headers to the client
int response_write_head(msg_t *self, const char *data, size_t len, callback_t cb)
{
  assert(!self->headers_sent);
  uv_buf_t buf = { base: (char *)data, len: len };
  self->headers_sent = 1;
  return client_write(self->client, &buf, 1, cb);
}

// write data to the client
int response_write(msg_t *self, const char *data, size_t len, callback_t cb)
{
  assert(self->headers_sent);
  uv_buf_t buf = { base: (char *)data, len: len };
  return client_write(self->client, &buf, 1, cb);
}

// finalize the response
void response_end(msg_t *self)
{
  // client is keep-alive, set keep-alive timeout upon request completion
  assert(self->headers_sent);
  if (self->should_keep_alive) {
    client_timeout(self->client, 500);
  } else {
    client_shutdown(self->client);
  }
  // cleanup
  msg_free(self);
}
