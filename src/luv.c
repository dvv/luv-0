#include "uhttp.h"

#include <lua.h>
#include <lauxlib.h>

/******************************************************************************/
/* HTTP server
/******************************************************************************/

static lua_State *LLL;
static int cb_ref;

static int l_msg(lua_State *L)
{
  const msg_t *msg = lua_touserdata(L, 1);
  //printf("MMM: %p\n", msg);
  if (!msg) {
    lua_pushnil(L);
  } else {
    lua_createtable(L, 0, 3);
    luaL_getmetatable(L, "uhttp.msg");
    lua_setmetatable(L, -2);
    lua_pushstring(L, msg->method);
    lua_setfield(L, -2, "method");
    lua_pushboolean(L, msg->should_keep_alive);
    lua_setfield(L, -2, "should_keep_alive");
    lua_pushboolean(L, msg->upgrade);
    lua_setfield(L, -2, "upgrade");
  }
  return 1;
}

static void on_event(client_t *self, msg_t *msg, enum event_t ev, int status, void *data)
{
  lua_State *L = LLL;
  lua_rawgeti(L, LUA_REGISTRYINDEX, cb_ref);
  lua_pushlightuserdata(L, self);
  lua_pushlightuserdata(L, msg);
  lua_pushinteger(L, ev);
  lua_pushinteger(L, status);
  lua_pushlightuserdata(L, data);
  lua_call(L, 5, 0);

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

static int l_respond(lua_State *L) {
  msg_t *self = lua_touserdata(L, 1);
  const char *data = luaL_checkstring(L, 2);
  //callback_t cb)
  response_write_head(self, data, NULL);
  response_end(self);
  return 0;
}

/******************************************************************************/
/* module
/******************************************************************************/

static const luaL_Reg exports[] = {
  { "make_server", l_make_server },
  { "msg", l_msg },
  { "respond", l_respond },
  { NULL, NULL }
};

LUALIB_API int luaopen_luv(lua_State *L) {

  luaL_newmetatable(L, "uhttp.msg");
  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_settable(L, -3);
  lua_pushcfunction(L, l_respond);
  lua_setfield(L, -2, "send");
  lua_pop(L, 1);

  /* module table */
  lua_newtable(L);
  luaL_register(L, "LUV", exports);

  return 1;
}
