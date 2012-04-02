#!/usr/bin/env luajit

local setmetatable = setmetatable
local Table = require('table')
--local delay = require('timer').setTimeout
--local LUV = require('./luv')
--local JSON = require('json')
local LUV = require('luv')
print('LUV', LUV)

local RESPONSE_TABLE = {'H','e','llo','\n'}
local RESPONSE_BODY = ('Hello\n'):rep(1)
local RESPONSE = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\n" .. RESPONSE_BODY

local function Message(handle)
  local self = LUV.msg(handle)
  if self then
    self.send = function (self, ...) LUV.send(handle, ...) end
  end
  return self
end

local slow = false

LUV.make_server(8080, '0.0.0.0', 128, function (msg, ev, int, void)
  --print('EVENT', msg, ev, int, void)
  --local m = Message(msg)
  if ev == LUV.DATA then
    --print('DATA', int, void)
  elseif ev == LUV.END then
    --LUV.delay(10, function ()
    if not slow then
      local m = LUV.msg(msg)
      for k, v in pairs(m.headers) do print(k, v) end
      if m.uri.path == '/1' then
        LUV.delay(20, function () LUV.send(msg, '[111]\n', 200, {}) end)
      elseif m.uri.path == '/2' then
        LUV.delay(0, function () LUV.send(msg, '[222]\n', 200, {}) end)
      elseif m.uri.path == '/3' then
        LUV.delay(30, function () LUV.send(msg, '[333]\n', 200, {}) end)
      elseif m.uri.path == '/4' then
        LUV.delay(10, function () LUV.send(msg, '[444]\n', 200, {}) end)
      else
        LUV.delay(1, function () LUV.send(msg, RESPONSE_BODY, 200, {}) end)
        --LUV.send(msg, RESPONSE_BODY, 200, {})
      end
    else
      -- one write()
      LUV.send(msg, RESPONSE_BODY, 200, {
        --['Content-Length'] = #RESPONSE_BODY
      }, true)
      -- second write()
      LUV.send(msg, nil, nil, nil, true)
      --LUV.finish(msg)
      LUV.send(msg)
    end
    --end)
  elseif ev == LUV.ERROR then
    print('ERROR', int, void)
  end
end)
print('Server listening to http://*:8080. CTRL+C to exit.')
LUV.run()
