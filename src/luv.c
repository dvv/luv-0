#include "uhttp.h"

#include <lua.h>
#include <lauxlib.h>

/******************************************************************************/
/* HTTP server
/******************************************************************************/

static lua_State *LLL;
static int cb_ref;

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
  { "respond", l_respond },
  { NULL, NULL }
};

LUALIB_API int luaopen_luv(lua_State *L) {

  /* module table */
  lua_newtable(L);
  luaL_register(L, NULL, exports);

  return 1;
}
