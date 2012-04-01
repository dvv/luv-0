#!/usr/bin/env node

var body = 'Hello\n';
require('http').createServer(function (req, res) {
  req.on('end', function () {
    if (req.url === '/1') {
      setTimeout(function () {
      res.writeHead(200, {
        'Content-Length': body.length,
      });
      res.end('[111]\n');
      }, 20);
    } else if (req.url === '/2') {
      setTimeout(function () {
      res.writeHead(200, {
        'Content-Length': body.length,
      });
      res.end('[222]\n');
      }, 0);
    } else if (req.url === '/3') {
      setTimeout(function () {
      res.writeHead(200, {
        'Content-Length': body.length,
      });
      res.end('[333]\n');
      }, 30);
    } else if (req.url === '/4') {
      setTimeout(function () {
      res.writeHead(200, {
        'Content-Length': body.length,
      });
      res.end('[444]\n');
      }, 10);
    } else {
      res.writeHead(200, {
        'Content-Length': body.length,
      });
      res.end(body);
    }
  });
}).listen(8080);
