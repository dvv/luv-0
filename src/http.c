#include "luv.h"

static int request_path_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = container_of(parser, client_t, handle);
  client->http.path = strndup(p, len);
  return 0;
}

int request_uri_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = container_of(parser, client_t, handle);
  client->http.uri = strndup(p, len);
  return 0;
}

int query_string_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = container_of(parser, client_t, handle);
  client->http.query = strndup(p, len);
  return 0;
}

int header_field_cb(http_parser *parser, const char *p, size_t len)
{
  /*struct message *m = &messages[num_messages];

  if (m->last_header_element != FIELD)
    m->num_headers++;

  strncat(m->headers[m->num_headers-1][0], p, len);

  m->last_header_element = FIELD;*/

  return 0;
}

int header_value_cb(http_parser *parser, const char *p, size_t len)
{
  /*struct message *m = &messages[num_messages];

  strncat(m->headers[m->num_headers-1][1], p, len);

  m->last_header_element = VALUE;*/

  return 0;
}

int body_cb(http_parser *parser, const char *p, size_t len)
{
  client_t *client = container_of(parser, client_t, handle);
  return 0;
}

int message_complete_cb(http_parser *parser)
{
  client_t *client = container_of(parser, client_t, handle);
  client->http.method = parser->method;
  return 0;
}

int message_begin_cb(http_parser *parser)
{
  client_t *client = container_of(parser, client_t, handle);
  return 0;
}

int headers_complete_cb(http_parser *parser)
{
  client_t *client = container_of(parser, client_t, handle);
  client->http.should_keep_alive = http_should_keep_alive(parser);
  DEBUG("http message!");
  client_write(client, &refbuf, 1, after_write);
  client_close(client, after_close);
  return 0; // 1?
}

void parser_init(http_parser *parser, enum http_parser_type type)
{
  http_parser_init(parser, type);
  parser.on_message_begin     = message_begin_cb;
  parser.on_header_field      = header_field_cb;
  parser.on_header_value      = header_value_cb;
  parser.on_path              = request_path_cb;
  parser.on_uri               = request_uri_cb;
  //parser.on_fragment          = fragment_cb;
  parser.on_query_string      = query_string_cb;
  parser.on_body              = body_cb;
  parser.on_headers_complete  = headers_complete_cb;
  parser.on_message_complete  = message_complete_cb;
}
