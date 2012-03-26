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
  lua_rawgeti(L, LUA_REGISTRYINDEX, cb_ref);
  //lua_pushlightuserdata(L, self);
  lua_pushlightuserdata(L, msg);
  lua_pushinteger(L, ev);
  lua_pushinteger(L, status);
  lua_pushlightuserdata(L, data);
  lua_call(L, 4, 0);

  /*luaL_getmetatable(L, "luv_tcp");
  lua_setmetatable(L, -2);*/

}

static int l_make_server(lua_State *L)
{
  LLL = L;
  int port = luaL_checkint(L, 1);
  const char *host = luaL_checkstring(L, 2);
  int backlog_size = luaL_checkint(L, 3);
  cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  uv_tcp_t *server = server_init(port, host, backlog_size, on_event);
  return 0;
}

static int l_mmm(lua_State *L) {
  msg_t *self = lua_newuserdata(L, sizeof(*self));
  self->method = "AAA";
  luaL_getmetatable(L, "uhttp.msg");
  lua_setmetatable(L, -2);
  return 1;
}

static int l_send(lua_State *L)
{
  msg_t *self = lua_touserdata(L, 1);
  int code = luaL_checkint(L, 2);
  //self, code, body, headers, do-not-end
  luaL_checktype(L, 4, LUA_TTABLE);
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
  lua_pushnil(L);
  while (lua_next(L, 4) != 0) {
    lua_pushvalue(L, -2);
    luaL_addvalue(&b);
    luaL_addstring(&b, ": ");
    luaL_addvalue(&b);
    luaL_addstring(&b, "\r\n");
  }
  luaL_addstring(&b, "\r\n");
  // append body
  // table case
  if (lua_istable(L, 3)) {
    size_t i, argc = lua_objlen(L, 3);
    for (i = 1; i <= argc; ++i) {
      lua_rawgeti(L, 3, i);
      luaL_addvalue(&b);
    }
  // string case
  } else {
    lua_pushvalue(L, 3);
    luaL_addvalue(&b);
  }
  // concat
  luaL_pushresult(&b);
  size_t len;
  const char *data = luaL_checklstring(L, -1, &len);
  lua_pop(L, 1);
//printf("SEND %*s\n", len, data);
  response_write_head(self, data, len, NULL);
  // finish response
  if (lua_toboolean(L, 5) == 0) {
    response_end(self);
  }
  return 0;
}

static int l_end(lua_State *L) {
  msg_t *self = lua_touserdata(L, 1);
  response_end(self);
  return 0;
}

/******************************************************************************/
/* module
/******************************************************************************/

static const luaL_Reg exports[] = {
  { "make_server", l_make_server },
  { "send", l_send },
  { "finish", l_end },
  { "msg", l_msg },
  { "mmm", l_mmm },
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

  luaL_newmetatable(L, "uhttp.msg");
  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_settable(L, -3);
  lua_pushcfunction(L, l_send);
  lua_setfield(L, -2, "send");
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

  return 1;
}
