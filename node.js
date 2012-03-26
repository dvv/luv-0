#!/usr/bin/env node

var body = 'Hello\n';
require('http').createServer(function (req, res) {
  res.writeHead(200, {
    'Content-Length': body.length,
  });
  res.end(body);
}).listen(8080);
