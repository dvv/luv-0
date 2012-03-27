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
    if (msg->should_pipeline) {
      lua_pushboolean(L, 1);
      lua_setfield(L, -2, "should_pipeline");
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
  LLL = L;
  int port = luaL_checkint(L, 1);
  const char *host = luaL_checkstring(L, 2);
  int backlog_size = luaL_checkint(L, 3);
  cb_ref = luaL_ref(L, LUA_REGISTRYINDEX); // store event handler
  uv_tcp_t *server = server_init(port, host, backlog_size, on_event);
  return 0;
}

// start the response
static int l_send(lua_State *L)
{
  size_t len;
  const char *data;
  //self, body, code, headers, do-not-end
  msg_t *self = lua_touserdata(L, 1);
self->chunked = 1;
  int code = luaL_optint(L, 3, 0);
  // empty output buffer
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  // send code and headers unless already sent
  if (!self->headers_sent && (code || lua_istable(L, 4))) {
    if (code) {
      self->headers_sent = 1;
      // start response
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
      lua_pushnil(L);
      while (lua_next(L, 4) != 0) {
        lua_pushvalue(L, -2);
        luaL_addvalue(&b);
        luaL_addstring(&b, ": ");
        luaL_addvalue(&b);
        luaL_addstring(&b, "\r\n");
      }
      if (self->chunked) {
        luaL_addstring(&b, "Transfer-Encoding: chunked\r\n");
      }
      luaL_addstring(&b, "\r\n");
    }
  }
  luaL_pushresult(&b); // headers on the stack
  // TODO: flush headers and honor chunked encoding
  // append body
  luaL_buffinit(L, &b);
  // table case
  if (lua_istable(L, 2)) {
    size_t i, argc = lua_objlen(L, 2);
    for (i = 1; i <= argc; ++i) {
      lua_rawgeti(L, 2, i);
      luaL_addvalue(&b);
    }
  // string case
  } else if (!lua_isnil(L, 2)) {
    lua_pushvalue(L, 2);
    luaL_addvalue(&b);
  }
  // concat body
  luaL_pushresult(&b); // headers, body
  if (self->chunked) {
    data = luaL_checklstring(L, -1, &len);
    luaL_buffinit(L, &b);
    char *s = luaL_prepbuffer(&b);
    sprintf(s, "%x\r\n", len);
    luaL_addsize(&b, strlen(s));
    luaL_addvalue(&b);
    luaL_addstring(&b, "\r\n");
  } else {
    luaL_addvalue(&b);
  }
  // concat
  luaL_pushresult(&b); // headers, body
  data = luaL_checklstring(L, -1, &len);
  lua_pop(L, 1);
//printf("SEND %*s\n", len, data);
  response_write(self, data, len, NULL);
  // finish response
  if (lua_toboolean(L, 5) == 0) {
    response_end(self, 0);
  }
  return 0;
}

// start the response
static int l_write_head(lua_State *L)
{
  //self, code, headers
  msg_t *self = lua_touserdata(L, 1);
self->chunked = 1;
  assert(self->headers_sent == 0);
  self->headers_sent = 1;
  int code = luaL_checkint(L, 2);
  // empty output buffer
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  // start response
  luaL_addstring(&b, "HTTP/1.1 ");
  // append code
  lua_pushvalue(L, 2);
  luaL_addvalue(&b);
  luaL_addchar(&b, ' ');
  // append status message
  luaL_addstring(&b, STATUS_CODES[code]);
  luaL_addstring(&b, "\r\n");
  // append headers
  if (lua_istable(L, 3)) {
    lua_pushnil(L);
    while (lua_next(L, 3) != 0) {
      lua_pushvalue(L, -2);
      luaL_addvalue(&b);
      luaL_addstring(&b, ": ");
      luaL_addvalue(&b);
      luaL_addstring(&b, "\r\n");
    }
    if (self->chunked) {
      luaL_addstring(&b, "Transfer-Encoding: chunked\r\n");
    }
  }
  luaL_addstring(&b, "\r\n");
  // concat
  luaL_pushresult(&b);
  size_t len;
  const char *data = luaL_checklstring(L, -1, &len);
  lua_pop(L, 1);
  response_write(self, data, len, NULL);
  return 0;
}

// write to response
static int l_write(lua_State *L)
{
  //self, body
  msg_t *self = lua_touserdata(L, 1);
  assert(self->headers_sent != 0);
  // empty output buffer
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  // table case
  if (lua_istable(L, 2)) {
    size_t i, argc = lua_objlen(L, 2);
    for (i = 1; i <= argc; ++i) {
      lua_rawgeti(L, 2, i);
      luaL_addvalue(&b);
    }
  // string case
  } else if (!lua_isnil(L, 2)) {
    lua_pushvalue(L, 2);
    luaL_addvalue(&b);
  }
  // concat
  luaL_pushresult(&b);
  size_t len;
  const char *data = luaL_checklstring(L, -1, &len);
  lua_pop(L, 1);
  if (self->chunked) {
    char s[128];
    sprintf(s, "%x\r\n", len);
    response_write(self, s, strlen(s), NULL);
  }
  response_write(self, data, len, NULL);
  if (self->chunked) {
    response_write(self, "\r\n", 2, NULL);
  }
  return 0;
}

// finalize the response
static int l_end(lua_State *L) {
  msg_t *self = lua_touserdata(L, 1);
  if (self->chunked) {
    response_write(self, "0\r\n\r\n", 5, NULL);
  }
  response_end(self, lua_toboolean(L, 2));
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
  { "write_head", l_write_head },
  { "write", l_write },
  { "finish", l_end },
  { "send", l_send },
  { "msg", l_msg },
  { "run", l_run },
  { NULL, NULL }
};

LUALIB_API int luaopen_luv(lua_State *L) {

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
