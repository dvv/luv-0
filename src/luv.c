#include <assert.h>

#include "uhttp.h"
#include "http_parser.h"

#include <lua.h>
#include <lauxlib.h>

const char *STATUS_CODES[600];

/******************************************************************************/
/* HTTP server
/******************************************************************************/

static lua_State *LLL;
static int cb_ref;

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
    const char *p = msg->heap;
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

static void on_event(client_t *self, msg_t *msg, enum event_t ev, int status, void *data)
{
  lua_State *L = LLL;
  int argc = 2;
  lua_rawgeti(L, LUA_REGISTRYINDEX, cb_ref); // get event handler
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

// start HTTP server
static int l_make_server(lua_State *L)
{
  int port = luaL_checkint(L, 1);
  const char *host = luaL_checkstring(L, 2);
  int backlog_size = luaL_checkint(L, 3);
  cb_ref = luaL_ref(L, LUA_REGISTRYINDEX); // store event handler
  uv_tcp_t *server = server_init(port, host, backlog_size, on_event);
  return 0;
}

// finish the response
static int l_end(lua_State *L) {
  msg_t *self = lua_touserdata(L, 1);
  if (self->chunked) {
    response_write(self, "0\r\n\r\n", 5);
  }
  response_end(self, lua_toboolean(L, 2));
  return 0;
}

// write the response
static int l_send(lua_State *L)
{
  size_t len;
  const char *s;
  luaL_Buffer b;

  //self, body, code, headers, do-not-end
  msg_t *self = lua_touserdata(L, 1);
  int code = lua_tointeger(L, 3);
  int finish = lua_toboolean(L, 5) == 0;

  // collect body
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
          sprintf((char *)s, "Content-Length: %ld\r\n", len);
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
      sprintf((char *)s, "%lx\r\n", len);
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
    response_end(self, 0);
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
  if (CLOSABLE(&delay->timer)) {
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

static const luaL_Reg exports[] = {
  { "make_server", l_make_server },
  { "send", l_send },
  { "finish", l_end },
  { "delay", l_delay },
  { "msg", l_msg },
  { "run", l_run },
  { NULL, NULL }
};

LUALIB_API int luaopen_luv(lua_State *L) {

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

  return 1;
}
