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

static uv_req_t *req_alloc() {
  req_list_t *req;

  req = req_freelist;
  if (req != NULL) {
    req_freelist = req->next;
    return (uv_req_t *)req;
  }

  req = (req_list_t *)malloc(sizeof *req);
  return (uv_req_t *)req;
}

static void req_free(uv_req_t *uv_req) {
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

static int NB = 0;

static uv_buf_t buf_alloc(uv_handle_t *handle, size_t size) {
  buf_list_t *buf;

  buf = buf_freelist;
  if (buf != NULL) {
    buf_freelist = buf->next;
    return buf->uv_buf_t;
  }

  buf = (buf_list_t *) malloc(size + sizeof *buf);
  printf("BUFALLOC %d\n", ++NB);
  buf->uv_buf_t.len = (unsigned int)size;
  buf->uv_buf_t.base = ((char *) buf) + sizeof *buf;

  return buf->uv_buf_t;
}

static void buf_free(uv_buf_t uv_buf_t) {
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

static msg_t *msg_alloc() {
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

static void msg_free(msg_t *msg) {
  msg_list_t *m = (msg_list_t *)msg;

  m->next = msg_freelist;
  msg_freelist = m;
}

/***
static void x_free(void **list)
{
  while (*list != NULL) {
    x = *list;
    *list = x->next;
    free(x);
  }
}***/

/*
 * HTTP client allocator
 */

typedef struct client_list_s {
  client_t client;
  struct client_list_s *next;
} client_list_t;

static client_list_t *client_freelist = NULL;

static int NC = 0;

static client_t *client_alloc() {
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

static void client_free(client_t *client) {
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

// async: close is done
static void client_after_close(uv_handle_t *handle)
{
  client_t *self = handle->data;
  assert(self);
  // dispose close timer
  if (!uv_is_closing((uv_handle_t *)&self->timer_timeout)) {
    uv_close((uv_handle_t *)&self->timer_timeout, NULL);
  }
  // fire 'close' event
  EVENT(self, self->msg, EVT_CLOSE, last_err().code, NULL);
  // free self
  client_free(self);
}

// shutdown and close the client
static void client_close(client_t *self)
{
  assert(self);
  // sanity check
  if (uv_is_closing((uv_handle_t *)&self->handle)) return;
  // stop close timer
  client_timeout(self, 0);
  // close the handle
  uv_close((uv_handle_t *)&self->handle, client_after_close);
}

// async: shutdown is done
static void client_after_shutdown(uv_shutdown_t *rq, int status)
{
  client_t *self = rq->data;
  req_free((uv_req_t *)rq);
  // fire 'shut' event
  EVENT(self, self->msg, EVT_SHUT, last_err().code, NULL);
  // close the handle
  client_close(self);
}

// shutdown and close the client
static void client_shutdown(client_t *self)
{
  // sanity check
  if (!uv_is_writable((uv_stream_t *)&self->handle)) return;
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
  memset(msg, 0, sizeof(*msg));
  // set message's client
  msg->client = client;
  // store links to previous message, if any
  msg->prev = client->msg;
  assert(msg != msg->prev);
  if (msg != msg->prev) {
    // this message is the next one for the current message
    if (msg->prev) msg->prev->next = msg;
  }
  //
  client->msg = msg;
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
  // reset parser
  http_parser_execute(parser, &parser_settings, NULL, 0);
  // fire 'end' event
  EVENT(client, msg, EVT_END, 0, NULL);
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
        if (msg) {
          EVENT(self, msg, EVT_ERROR, UV_UNKNOWN, NULL);
        }
        // just close the client
        client_close(self);
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
  // set client initial inactivity timeout to 10 seconds
  client_timeout(client, 10000);

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

// write data to the message buffer
int response_write(msg_t *self, const char *data, size_t len)
{
  assert(self);
  assert(!self->finished);
  // TODO: HEAD should void body
//printf("WRITE %*s\n", len, data);
  // TODO: overflow
  uv_buf_t *buf = &self->bufs[self->nbufs++];
  buf->base = (char *)data;
  buf->len = len;
  return 0;
}

static void response_free(msg_t *self)
{
  DEBUGF("RFREE %p\n", self);
  self->client->msg = NULL;
  // TODO: cleanup cleaner
  msg_free(self);
}

// async: write is done
static void response_client_after_write(uv_write_t *rq, int status)
{
  msg_t *msg = rq->data;
  req_free((uv_req_t *)rq);
  // client is keep-alive, set keep-alive timeout upon request completion
  if (msg->should_keep_alive) {
    client_timeout(msg->client, 500);
  } else {
    client_shutdown(msg->client);
  }
  // free message
  response_free(msg);
}

// flush message buffer to the client
// N.B. honor order of responses
void response_end(msg_t *self)
{
  assert(self);
  assert(!self->finished);
  // mark this message as finished
  self->finished = 1;
  // all of previous messages are also finished?
  msg_t *p = self;
  while (p->prev && p->prev->finished) {
    p = p->prev;
  }
  // yes! pipeline ok
  if (!p->prev) {
    // flush the buffers of all previous messages.
    // flush all next finished messages as well
    msg_t *next;
    while (p && p->finished) {
      next = p->next;
      p->prev = p->next = NULL;
      // unlink the message
      if (next) next->prev = NULL;
      // write message buffers
      assert(p->headers_sent);
      uv_stream_t *handle = (uv_stream_t *)&p->client->handle;
      // write only to writable stream
      if (uv_is_writable(handle)) {
        // stop close timer
        client_timeout(p->client, 0);
        // create write request
        uv_write_t *rq = (uv_write_t *)req_alloc();
        rq->data = p;
        // write buffers
        uv_write(rq, handle, p->bufs, p->nbufs, response_client_after_write);
      // stream is not writable? just cleanup message
      } else {
        response_free(p);
      }
      // try to flush next message
      p = next;
    }
  }
}
