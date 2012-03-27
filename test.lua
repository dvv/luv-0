#!/usr/bin/env luajit

local setmetatable = setmetatable
local Table = require('table')
--local delay = require('timer').setTimeout
--local LUV = require('./luv')
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

LUV.make_server(8080, '0.0.0.0', 128, function (msg, ev, int, void)
  --print('EVENT', msg, ev, int, void)
  --local m = Message(msg)
  if ev == LUV.DATA then
    print('DATA', int, void)
  elseif ev == LUV.END then
    --debug('METH', m)
    --delay(0, function ()
    if false then
      print(LUV.write_head(msg, 200, {
        ['Content-Length'] = #RESPONSE_BODY
      }))
      print(LUV.write(msg, RESPONSE_BODY))
      LUV.finish(msg)
    else
      LUV.send(msg, RESPONSE_BODY, 200, {
        --['Content-Length'] = #RESPONSE_BODY
      })
    end
    --end)
  elseif ev == LUV.ERROR then
    LUV.finish(msg, true)
  end
end)
print('Server listening to http://*:8080. CTRL+C to exit.')
LUV.run()
