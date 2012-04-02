#include "uhttp.h"

#define DELAY_RESPONSE 100

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

static void timer_on_close(uv_handle_t *timer)
{
  free((void *)timer);
}

#include <assert.h>
static void timer_on_timeout(uv_timer_t *timer, int status)
{
  msg_t *msg = timer->data;
  response_write(msg, RESPONSE_HEAD, 38);
  msg->headers_sent = 1;
  if (strcmp(msg->heap, "/1") == 0) {
    response_write(msg, "[111]\n", 6);
  } else if (strcmp(msg->heap, "/2") == 0) {
    response_write(msg, "[222]\n", 6);
  } else if (strcmp(msg->heap, "/3") == 0) {
    response_write(msg, "[333]\n", 6);
  } else if (strcmp(msg->heap, "/4") == 0) {
    response_write(msg, "[444]\n", 6);
  } else {
    response_write(msg, RESPONSE_BODY, 6);
  }
  response_end(msg);
  uv_close((uv_handle_t *)timer, timer_on_close);
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
    uv_timer_t *timer = malloc(sizeof(*timer));
    timer->data = msg;
    uv_timer_init(self->handle.loop, timer);
    if (strcmp(msg->heap, "/1") == 0) {
      uv_timer_start(timer, timer_on_timeout, 2 * DELAY_RESPONSE, 0);
    } else if (strcmp(msg->heap, "/2") == 0) {
      uv_timer_start(timer, timer_on_timeout, 0 * DELAY_RESPONSE, 0);
    } else if (strcmp(msg->heap, "/3") == 0) {
      uv_timer_start(timer, timer_on_timeout, 3 * DELAY_RESPONSE, 0);
    } else if (strcmp(msg->heap, "/4") == 0) {
      uv_timer_start(timer, timer_on_timeout, 1 * DELAY_RESPONSE, 0);
    } else {
      uv_timer_start(timer, timer_on_timeout, DELAY_RESPONSE, 0);
    }
#else
    response_write(msg, RESPONSE_HEAD RESPONSE_BODY, 44);
    msg->headers_sent = 1;
    response_end(msg);
#endif
  } else if (ev == EVT_ERROR) {
    DEBUGF("RERROR %p %d %s", msg, status, (char *)data);
  }
}

int main()
{
  uv_loop_t *loop = uv_default_loop();

  // N.B. let libuv catch EPIPE
  signal(SIGPIPE, SIG_IGN);

  uv_tcp_t *server = server_init(8080, "0.0.0.0", 1024, client_on_event);

  // REPL?

  // block in the main loop
  uv_run(loop);
}
