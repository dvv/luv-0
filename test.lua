#!/usr/bin/env luvit

print('FOO')
local setmetatable = setmetatable
print('FUB')
local LUV = require('./luv')
print('BAR', LUV)--make_server)

local RESPONSE = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nHello\n"

local m = LUV.mmm()
debug('m', m, m.method, m.send)

LUV.make_server(8080, '0.0.0.0', 128, function (client, msg, ev, int, void)
  debug('EVENT', client, msg, ev, int, void)
  local M = LUV.msg(msg)
  if ev == 5 then
    debug('METH', M, M and M.send)
    --response_write_head(msg, RESPONSE_HEAD, nil)
    --response_end(msg)
    LUV.respond(msg, RESPONSE)
    --M:send(RESPONSE)
  end
end)
print('Server listening to http://*:8080. CTRL+C to exit.')
