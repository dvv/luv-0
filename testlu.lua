#!/usr/bin/env luvit

local Table = require('table')
--local delay = require('timer').setTimeout
--local LUV = require('./luv')
--local JSON = require('json')
local LUV = require('./lu')
p('LUV', LUV)

local RESPONSE_TABLE = {'H','e','llo','\n'}
local RESPONSE_BODY = ('Hello\n'):rep(1)
local RESPONSE = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\n" .. RESPONSE_BODY

local server = LUV.make_server(8080, '0.0.0.0', 128, function (msg, ev, int, void)
  --print('EVENT', msg, ev, int, void)
  --local m = Message(msg)
  if ev == LUV.DATA then
    --print('DATA', int, void)
  elseif ev == LUV.END then
    p(msg)
    for k, v in pairs(msg.headers) do print(k, v) end
    if msg.uri.path == '/1' then
      LUV.delay(20, function () LUV.send(msg, '[111]\n', 200, {}) end)
    elseif msg.uri.path == '/2' then
      LUV.delay(0, function () LUV.send(msg, '[222]\n', 200, {}) end)
    elseif msg.uri.path == '/3' then
      LUV.delay(30, function () LUV.send(msg, '[333]\n', 200, {}) end)
    elseif msg.uri.path == '/4' then
      LUV.delay(10, function () LUV.send(msg, '[444]\n', 200, {}) end)
    else
      LUV.delay(1, function () LUV.send(msg, RESPONSE_BODY, 200, {}) end)
      --LUV.send(msg, RESPONSE_BODY, 200, {})
    end
  elseif ev == LUV.ERROR then
    print('ERROR', int, void)
  end
end)
print('Server listening to http://*:8080. CTRL+C to exit.')
p(server, server.run)
--server.foo = 1
