#include "uhttp.h"

//#define DELAY_RESPONSE 100

#define RESPONSE_HEAD \
  "HTTP/1.1 200 OK\r\n" \
  "Content-Length: 6\r\n" \
  "\r\n"

#define RESPONSE_HEAD_KA \
  "HTTP/1.1 200 OK\r\n" \
  "Content-Length: 6\r\n" \
  "Connection: Keep-Alive\r\n" \
  "\r\n"

#define RESPONSE_HEAD_NKA \
  "HTTP/1.1 200 OK\r\n" \
  "Content-Length: 6\r\n" \
  "\r\n"

#define RESPONSE_BODY \
  "Hello\n"

static void on_timeout(uv_timer_t *timer, int status)
{
  msg_t *msg = timer->data;
  response_write(msg, RESPONSE_HEAD RESPONSE_BODY, 44, NULL);
  msg->headers_sent = 1;
  response_end(msg, 0);
  uv_close((uv_handle_t *)timer, NULL);
}

static void client_on_event(client_t *self, msg_t *msg, enum event_t ev, int status, void *data)
{
  static uv_buf_t refbuf[] = {{
    base : "hel",
    len  : 3,
  }, {
    base : "lo\n",
    len  : 3,
  }};
  if (ev == EVT_DATA) {
    DEBUGF("RDATA %p %*s", msg, status, (char *)data);
  } else if (ev == EVT_REQUEST) {
    DEBUGF("CREQ %p", self);
  } else if (ev == EVT_SHUT) {
    DEBUGF("CEND %p", self);
  } else if (ev == EVT_OPEN) {
    DEBUGF("COPEN %p", self);
  } else if (ev == EVT_CLOSE) {
    DEBUGF("CCLOSE %p", self);
  } else if (ev == EVT_ERROR) {
    DEBUGF("CERROR %p %d %s", self, status, (char *)data);
  } else if (ev == EVT_END) {
    DEBUGF("REND %p", msg);
#ifdef DELAY_RESPONSE
    uv_timer_t timer;
    memset(&timer, 0, sizeof(timer));
    timer.data = msg;
    uv_timer_init(self->handle.loop, &timer);
    uv_timer_start(&timer, on_timeout, DELAY_RESPONSE, 0);
#else
    response_write(msg, RESPONSE_HEAD RESPONSE_BODY, 44, NULL);
    msg->headers_sent = 1;
    response_end(msg, 0);
#endif
  } else if (ev == EVT_ERROR) {
    DEBUGF("RERROR %p %d %s", msg, status, (char *)data);
  }
}

int main()
{
printf("MSG %ld\n", sizeof(msg_t));

  uv_loop_t *loop = uv_default_loop();

  // N.B. let libuv catch EPIPE
  signal(SIGPIPE, SIG_IGN);

  uv_tcp_t *server = server_init(8080, "0.0.0.0", 1024, client_on_event);

  // REPL?

  // block in the main loop
  uv_run(loop);
}
