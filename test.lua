#!/usr/bin/env luvit

local setmetatable = setmetatable
local LUV = require('./luv')

local RESPONSE = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nHello\n"

LUV.make_server(8080, '0.0.0.0', 128, function (client, msg, ev, int, void)
  --p('EVENT', client, msg, ev, int, void)
  if ev == 5 then
    --response_write_head(msg, RESPONSE_HEAD, nil)
    --response_end(msg)
    LUV.respond(msg, RESPONSE)
  end
end)
print('Server listening to http://*:8080. CTRL+C to exit.')
