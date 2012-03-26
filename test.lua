#!/usr/bin/env luvit

local setmetatable = setmetatable
local Table = require('table')
--local delay = require('timer').setTimeout
local LUV = require('./luv')
p('LUV', LUV)

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

LUV.make_server(8080, '0.0.0.0', 128, function (msg, ev, ...)
  --debug('EVENT', msg, ev, ...)
  --local m = Message(msg)
  if ev == LUV.END then
    --debug('METH', m)
    delay(0, function ()
    --m:send(200, RESPONSE_BODY, {
    LUV.send(msg, 200, RESPONSE_BODY, {
      ['Content-Length'] = #RESPONSE_BODY
    })
    end)
  end
end)
print('Server listening to http://*:8080. CTRL+C to exit.')
