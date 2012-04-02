#include <assert.h>
#include <signal.h>
#include <fcntl.h>

#include "uhttp.h"
#include "http_parser.h"

#include <lua.h>
#include <lauxlib.h>

const char *STATUS_CODES[600];


// TODO: pass via server to client and to message!!!
static lua_State *LLL;



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
  buf->uv_buf_t.len = size;
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
/* HTTP message
/******************************************************************************/

static int l_msg(lua_State *L)
{
  const msg_t *msg = lua_touserdata(L, 1);
  //lua_pop(L, 1);
  //printf("MMM: %p\n", msg);
  if (!msg) {
    lua_pushnil(L);
  } else {
    lua_createtable(L, 0, 8);
    luaL_getmetatable(L, "uhttp.msg");
    lua_setmetatable(L, -2);
    lua_pushstring(L, msg->method);
    lua_setfield(L, -2, "method");
    if (msg->should_keep_alive) {
      lua_pushboolean(L, 1);
      lua_setfield(L, -2, "should_keep_alive");
    }
    if (msg->upgrade) {
      lua_pushboolean(L, 1);
      lua_setfield(L, -2, "upgrade");
    }
    // url
    const char *p = msg->heap.base;
    lua_pushstring(L, p);
    lua_setfield(L, -2, "url");
    // uri
    {
    lua_newtable(L);
    struct http_parser_url url;
    const char *field_names[] = {
      "schema", "host", "port", "path", "query", "fragment"
    };
    if (http_parser_parse_url(p, strlen(p), 0, &url) == 0) {
      int i;
      for (i = UF_SCHEMA; i < UF_MAX; ++i) if (url.field_set & (1 << i)) {
        lua_pushlstring(L, p + url.field_data[i].off, url.field_data[i].len);
        lua_setfield(L, -2, field_names[i]);
      }
    }
    lua_setfield(L, -2, "uri");
    }
    // headers
    lua_newtable(L);
    p += strlen(p) + 1;
    while (*p) {
      const char *name = p;
      p += strlen(p) + 1;
      lua_pushstring(L, p);
      lua_setfield(L, -2, name);
      p += strlen(p) + 1;
    }
    lua_setfield(L, -2, "headers");
  }
  return 1;
}

/******************************************************************************/
/* TCP client methods
/******************************************************************************/

static void client_on_timeout(uv_timer_t *timer, int status);
static void response_free(msg_t *self);

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
  // free pending responses, if any
  msg_t *p = self->msg, *prev;
  self->msg = NULL;
  if (p) {
    assert(!p->next);
    //DEBUGF("PENDING: %p", p);
    while (p) {
      ////p->finished = 0;
      prev = p->prev;
      response_free(p);
      p = prev;
    }
  }
  // fire 'close' event
  EVENT(self, NULL, EVT_CLOSE, last_err().code, NULL);
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

// close the message
static int l_client_close(lua_State *L) {
  client_close(lua_touserdata(L, 1));
  return 0;
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
  if (uv_shutdown(rq, (uv_stream_t *)&self->handle, client_after_shutdown)) {
    req_free((uv_req_t *)rq);
  }
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
  lua_State *L = LLL;
  // allocate message
  msg_t *msg = lua_newuserdata(L, sizeof(*msg));
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
  // allocate message heap
  // N.B. size should equal uv's buffer size
  msg->heap = buf_alloc(NULL, 64 * 1024);
  memset(msg->heap.base, 0, 64 * 1024);
  msg->heap.len = 0;
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
  strncat(msg->heap.base, p, len);
  msg->heap.len += len;
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
  int new = (parser->state == 47 && msg->heap.len) ? 1 : 0; // s_header_field?
  {
  // lower case and append to the heap
  size_t i;
  char *s = msg->heap.base + msg->heap.len + new;
  for (i = 0; i < len; ++i) *s++ = tolower(p[i]);
  *s++ = '\0';
  }
  msg->heap.len += len + new;
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
  strncat(msg->heap.base + msg->heap.len + new, p, len);
  msg->heap.len += len + new;
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
/* HTTP response methods
/******************************************************************************/

// write data to the message buffer
void response_write(msg_t *self, const char *data, size_t len)
{
  assert(self);
  if (!self->finished) {
    // TODO: HEAD should void body
  //printf("WRITE %*s\n", len, data);
    // TODO: overflow
    uv_buf_t *buf = &self->bufs[self->nbufs++];
    buf->base = (char *)data;
    buf->len = len;
  }
}

static void response_free(msg_t *self)
{
  assert(self);
  //DEBUGF("RFREE %p", self);
  // this is the last message?
  if (self->client->msg == self) {
    self->client->msg = NULL;
  }
  // TODO: cleanup cleaner
  buf_free(self->heap);
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
        if (uv_write(rq, handle, p->bufs, p->nbufs,
            response_client_after_write)) {
          req_free((uv_req_t *)rq);
          EVENT(p->client, p, EVT_ERROR, last_err().code, NULL);
        }
      // stream is not writable? just cleanup message
      } else {
        response_free(p);
      }
      // try to flush next message
      p = next;
    }
  }
}

/******************************************************************************/
/* HTTP server
/******************************************************************************/

static void on_event(client_t *self, msg_t *msg, enum event_t ev, int status, void *data)
{
  assert(self);
  lua_State *L = LLL;
  int argc = 2;
  lua_rawgeti(L, LUA_REGISTRYINDEX, self->server->data); // get event handler
  lua_pushlightuserdata(L, msg);
  lua_pushinteger(L, ev);
  switch (ev) {
    case EVT_DATA:
      lua_pushlstring(L, data, status);
      argc += 1;
      break;
    case EVT_ERROR:
      lua_pushinteger(L, status);
      argc += 1;
  }
  lua_call(L, argc, 0);
}

/******************************************************************************/
/* HTTP server
/******************************************************************************/

// HTTP connection handler
static void server_on_connection(uv_stream_t *self, int status)
{
  // allocate client
  client_t *client = client_alloc();
  assert(client);
  memset(client, 0, sizeof(*client));
  client->server = (uv_tcp_t *)self;
  client->on_event = on_event;
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

// start HTTP server
static int l_make_server(lua_State *L)
{
  int port = luaL_checkint(L, 1);
  const char *host = luaL_checkstring(L, 2);
  int backlog_size = luaL_checkint(L, 3);
  int handler_ref = luaL_ref(L, LUA_REGISTRYINDEX); // store event handler

  uv_tcp_t *server = lua_newuserdata(L, sizeof(*server));
  server->data = (void *)handler_ref; // store message event handler
  uv_tcp_init(uv_default_loop(), server);
  struct sockaddr_in address = uv_ip4_addr(host, port);
  CHECK("bind", uv_tcp_bind(server, address));
  CHECK("listen",
      uv_listen((uv_stream_t *)server, backlog_size, server_on_connection)
  );

  // TODO: doesn't work
  luaL_getmetatable(L, "http.server");
  lua_setmetatable(L, -2);
  lua_newtable(L);
  lua_setfenv(L, -2);

  return 1;
}

// close the message
static int l_message_close(lua_State *L) {
  msg_t *self = lua_touserdata(L, 1);
  if (self->chunked) {
    response_write(self, "0\r\n\r\n", 5);
  }
  response_end(self);
  return 0;
}

// write to the message
static int l_message_send(lua_State *L)
{
  size_t len;
  const char *s;
  luaL_Buffer b;

  //self, body, code, headers, do-not-end
  msg_t *self = lua_touserdata(L, 1);
  int code = lua_tointeger(L, 3);
  int finish = lua_toboolean(L, 5) == 0;

  // collect body
  // TODO: if method is HEAD, body is ""
  // table case?
  if (lua_istable(L, 2)) {
    size_t i;
    len = lua_objlen(L, 2);
    luaL_buffinit(L, &b);
    for (i = 1; i <= len; ++i) {
      lua_rawgeti(L, 2, i);
      luaL_addvalue(&b);
    }
    luaL_pushresult(&b); // body
  // something else case?
  } else if (!lua_isnoneornil(L, 2)) {
    lua_pushvalue(L, 2); // body
  // none or nil
  } else {
    lua_pushliteral(L, ""); // body
  }

  // collect code and headers
  if (!self->headers_sent && (code || lua_istable(L, 4))) {
    luaL_buffinit(L, &b);
    if (code) {
      self->headers_sent = 1;
      // start response
      // TODO: real version
      luaL_addstring(&b, "HTTP/1.1 ");
      // append code
      lua_pushvalue(L, 3);
      luaL_addvalue(&b);
      luaL_addchar(&b, ' ');
      // append status message
      luaL_addstring(&b, STATUS_CODES[code]);
      luaL_addstring(&b, "\r\n");
    }
    // append headers
    if (lua_istable(L, 4)) {
      self->headers_sent = 1;
      // walk over the key/value
      lua_pushnil(L);
      while (lua_next(L, 4) != 0) {
        // analyze key
        s = lua_tostring(L, -2);
        if (!self->has_content_length
              && strcasecmp(s, "content-length") == 0) {
          self->has_content_length = 1;
        } else if (!self->has_transfer_encoding
              && strcasecmp(s, "transfer-encoding") == 0) {
          self->has_transfer_encoding = 1;
        }
        lua_pushvalue(L, -2);
        luaL_addvalue(&b);
        luaL_addstring(&b, ": ");
        // analyze value
        s = lua_tostring(L, -1);
        if (!self->chunked && self->has_transfer_encoding
            && strcasecmp(s, "chunked") == 0) {
          self->chunked = 1;
        }
        //
        luaL_addvalue(&b);
        luaL_addstring(&b, "\r\n");
      }
      // determine whether response should be chunk encoded.
      // explicit Content-Length: voids chunk encoding
      if (self->has_content_length) {
        self->chunked = 0;
      // neither Content-Length: nor Transfer-Encoding: chunked was met.
      } else if (!self->has_transfer_encoding) {
        // response is to be finished?
        // no chunking and we need to know body length
        if (finish) {
          // get body length
          len = lua_objlen(L, -1);
          s = luaL_prepbuffer(&b);
          sprintf((char *)s, "Content-Length: %" PRIu32 "\r\n", len);
          luaL_addsize(&b, strlen(s));
          ////self->chunked = 0;
        // response is not finished. setup chunking
        } else if (!self->no_chunking) {
          luaL_addstring(&b, "Transfer-Encoding: chunked\r\n");
          self->chunked = 1;
        }
      }
      luaL_addstring(&b, "\r\n");
    }
    luaL_pushresult(&b); // body, headers
  } else {
    lua_pushliteral(L, ""); // body, headers
  }

  // swap the stack
  lua_insert(L, -2); // headers, body

  // chunked encoding wraps the body
  if (self->chunked) {
    len = lua_objlen(L, -1);
    luaL_buffinit(L, &b);
    if (len > 0) {
      s = luaL_prepbuffer(&b);
      sprintf((char *)s, "%" PRIx32 "\r\n", len);
      luaL_addsize(&b, strlen(s));
      luaL_addvalue(&b);
      luaL_addstring(&b, "\r\n");
    }
    // finishing chunk
    if (finish) {
      luaL_addstring(&b, "0\r\n\r\n");
    }
    luaL_pushresult(&b); // headers, chunked body
  }

  // concat: headers + body
  lua_concat(L, 2);
  s = luaL_checklstring(L, -1, &len);
//printf("SEND %*s\n", len, s);
  response_write(self, s, len);
  // finish response
  if (finish) {
    response_end(self);
  }

  return 1;
}

/******************************************************************************/
/* timer
/******************************************************************************/

typedef struct {
  uv_timer_t timer;
  lua_State *L;
  int cb;
} delay_t;

static void delay_on_close(uv_handle_t *timer)
{
  delay_t *delay = timer->data;
  free((void *)delay);
}

static void delay_on_timer(uv_timer_t *timer, int status)
{
  delay_t *delay = timer->data;
  lua_State *L = delay->L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, delay->cb);
  luaL_unref(L, LUA_REGISTRYINDEX, delay->cb);
  lua_call(L, 0, 0);
#if 0
  if (!uv_is_closing((uv_handle_t *)&delay->timer)) {
    // TODO: delayed until https://github.com/joyent/libuv/issues/364 solved
    uv_close((uv_handle_t *)&delay->timer, delay_on_close);
  }
#else
  delay_on_close((uv_handle_t *)timer);
#endif
}

static int l_delay(lua_State *L)
{
  delay_t *delay = malloc(sizeof(*delay));
  delay->timer.data = delay;
  delay->L = L;
  delay->cb = luaL_ref(L, LUA_REGISTRYINDEX);
  uv_timer_init(uv_default_loop(), &delay->timer);
  uv_timer_start(&delay->timer, delay_on_timer, luaL_checkint(L, 1), 0);
  return 0;
}

/******************************************************************************/
/* module
/******************************************************************************/

// for standart Lua interpreters, call to start event loop
static int l_run(lua_State *L) {
  uv_run(uv_default_loop());
  return 0;
}

static const luaL_Reg http_server_methods[] = {
  { "run", l_run },
  { NULL, NULL }
};

static const luaL_Reg http_client_methods[] = {
  { "close", l_client_close },
  { NULL, NULL }
};

static const luaL_Reg http_message_methods[] = {
  { "send", l_message_send },
  { "close", l_message_close },
  { NULL, NULL }
};

static const luaL_Reg exports[] = {
  { "make_server", l_make_server },
  { "delay", l_delay },
  { "msg", l_msg },
  { "run", l_run },
  { NULL, NULL }
};

LUALIB_API int luaopen_lu(lua_State *L) {

  LLL = L;

  STATUS_CODES[100] = "Continue";
  STATUS_CODES[101] = "Switching Protocols";
  STATUS_CODES[102] = "Processing";             // RFC 2518; obsoleted by RFC 4918
  STATUS_CODES[200] = "OK";
  STATUS_CODES[201] = "Created";
  STATUS_CODES[202] = "Accepted";
  STATUS_CODES[203] = "Non-Authoritative Information";
  STATUS_CODES[204] = "No Content";
  STATUS_CODES[205] = "Reset Content";
  STATUS_CODES[206] = "Partial Content";
  STATUS_CODES[207] = "Multi-Status";               // RFC 4918
  STATUS_CODES[300] = "Multiple Choices";
  STATUS_CODES[301] = "Moved Permanently";
  STATUS_CODES[302] = "Moved Temporarily";
  STATUS_CODES[303] = "See Other";
  STATUS_CODES[304] = "Not Modified";
  STATUS_CODES[305] = "Use Proxy";
  STATUS_CODES[307] = "Temporary Redirect";
  STATUS_CODES[400] = "Bad Request";
  STATUS_CODES[401] = "Unauthorized";
  STATUS_CODES[402] = "Payment Required";
  STATUS_CODES[403] = "Forbidden";
  STATUS_CODES[404] = "Not Found";
  STATUS_CODES[405] = "Method Not Allowed";
  STATUS_CODES[406] = "Not Acceptable";
  STATUS_CODES[407] = "Proxy Authentication Required";
  STATUS_CODES[408] = "Request Time-out";
  STATUS_CODES[409] = "Conflict";
  STATUS_CODES[410] = "Gone";
  STATUS_CODES[411] = "Length Required";
  STATUS_CODES[412] = "Precondition Failed";
  STATUS_CODES[413] = "Request Entity Too Large";
  STATUS_CODES[414] = "Request-URI Too Large";
  STATUS_CODES[415] = "Unsupported Media Type";
  STATUS_CODES[416] = "Requested Range Not Satisfiable";
  STATUS_CODES[417] = "Expectation Failed";
  STATUS_CODES[418] = "I'm a teapot";               // RFC 2324
  STATUS_CODES[422] = "Unprocessable Entity";       // RFC 4918
  STATUS_CODES[423] = "Locked";                     // RFC 4918
  STATUS_CODES[424] = "Failed Dependency";          // RFC 4918
  STATUS_CODES[425] = "Unordered Collection";       // RFC 4918
  STATUS_CODES[426] = "Upgrade Required";           // RFC 2817
  STATUS_CODES[500] = "Internal Server Error";
  STATUS_CODES[501] = "Not Implemented";
  STATUS_CODES[502] = "Bad Gateway";
  STATUS_CODES[503] = "Service Unavailable";
  STATUS_CODES[504] = "Gateway Time-out";
  STATUS_CODES[505] = "HTTP Version not supported";
  STATUS_CODES[506] = "Variant Also Negotiates";    // RFC 2295
  STATUS_CODES[507] = "Insufficient Storage";       // RFC 4918
  STATUS_CODES[509] = "Bandwidth Limit Exceeded";
  STATUS_CODES[510] = "Not Extended";               // RFC 2774

  /* metatables */

  luaL_newmetatable(L, "http.server");
  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_settable(L, -3);
  luaL_register(L, NULL, http_server_methods);
  lua_pop(L, 1);

  luaL_newmetatable(L, "http.client");
  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_settable(L, -3);
  luaL_register(L, NULL, http_client_methods);
  lua_pop(L, 1);

  luaL_newmetatable(L, "http.message");
  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_settable(L, -3);
  luaL_register(L, NULL, http_message_methods);
  lua_pop(L, 1);

  /* module table */
  lua_newtable(L);
  luaL_register(L, NULL, exports);

  /* constants */
  lua_pushinteger(L, EVT_ERROR);
  lua_setfield(L, -2, "ERROR");
  lua_pushinteger(L, EVT_OPEN);
  lua_setfield(L, -2, "OPEN");
  lua_pushinteger(L, EVT_REQUEST);
  lua_setfield(L, -2, "REQUEST");
  lua_pushinteger(L, EVT_DATA);
  lua_setfield(L, -2, "DATA");
  lua_pushinteger(L, EVT_END);
  lua_setfield(L, -2, "END");
  lua_pushinteger(L, EVT_SHUT);
  lua_setfield(L, -2, "SHUT");
  lua_pushinteger(L, EVT_CLOSE);
  lua_setfield(L, -2, "CLOSE");
  lua_pushinteger(L, EVT_MESSAGE);
  lua_setfield(L, -2, "MESSAGE");

  return 1;
}
