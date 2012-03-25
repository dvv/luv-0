#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

int main(int argc, char *argv[])
{
  lua_State *L = luaL_newstate();

  luaL_openlibs(L);
  luaopen_luv(L);

  //Image_register(L);

  if (argc > 1) luaL_dofile(L, argv[1]);
  uv_run(uv_default_loop());

  lua_close(L);
  return 0;
}
