/*
 * HTTP protocol parser is based on https://github.com/jeremycw/httpserver.h MIT code.
 */

#include "http_server.h"
#include "poller_adapter.h"
#include "poller/direct_poller_adapter.h"
#include "ssl/brssl_poller_adapter.h"

#include <iowow/iwlog.h>
#include <iowow/iwutils.h>
#include <iowow/iwpool.h>
#include <iowow/iwxstr.h>

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct server {
  struct iwn_http_server      server;
  struct iwn_http_server_spec spec;
  atomic_long stime;  ///< Server time second since epoch.
  int fd;
  int refs;
  pthread_mutex_t mtx;
  IWPOOL *pool;
  bool    https;
};

struct token {
  int index;
  int len;
  int type;
};

struct tokens_buf {
  struct token *buf;
  ssize_t       capacity;
  ssize_t       size;
};

struct stream {
  char *buf;
  struct token token;
  ssize_t      bytes_total;
  ssize_t      capacity;
  ssize_t      length;
  ssize_t      index;
  ssize_t      anchor;
  uint8_t      flags;
};

struct parser {
  ssize_t content_length;
  ssize_t body_consumed;
  int16_t match_index;
  int16_t header_count;
  int8_t  state;
  int8_t  meta;
};

struct header {
  char *name;
  char *value;
  struct header *next;
};

struct response {
  struct header *headers;
  IWPOOL     *pool;
  const char *body;
  void   (*body_free)(void*);
  size_t body_len;
  int    code;
};

struct client {
  struct iwn_http_request request;
  void    (*chunk_cb)(struct iwn_http_request*, void*);
  void   *chunk_cb_user_data;
  IWPOOL *pool;
  struct iwn_poller_adapter *pa;
  struct server    *server;
  struct tokens_buf tokens;
  struct stream     stream;
  struct parser     parser;
  struct response   response;
  int     fd;
  uint8_t state;     ///< HTTP_SESSION_{INIT,READ,WRITE,NOP}
  uint8_t flags;     ///< HTTP_END_SESSION,HTTP_AUTOMATIC,HTTP_CHUNKED_RESPONSE
};

// stream flags
#define HS_SF_CONSUMED 0x01U

// parser flags
#define HS_PF_IN_CONTENT_LEN  0x01U
#define HS_PF_IN_TRANSFER_ENC 0x02U
#define HS_PF_CHUNKED         0x04U
#define HS_PF_CKEND           0x08U
#define HS_PF_REQ_END         0x10U

// http session states
#define HTTP_SESSION_INIT  0
#define HTTP_SESSION_READ  1
#define HTTP_SESSION_WRITE 2
#define HTTP_SESSION_NOP   3

// http session flags
#define HTTP_KEEP_ALIVE       0x01U
#define HTTP_STREAMED         0x02U
#define HTTP_END_SESSION      0x04U
#define HTTP_AUTOMATIC        0x08U
#define HTTP_CHUNKED_RESPONSE 0x10U

// http version indicators
#define HTTP_1_0 0
#define HTTP_1_1 1

#define HS_META_NOT_CHUNKED  0
#define HS_META_NON_ZERO     0
#define HS_META_END_CHK_SIZE 1
#define HS_META_END_CHUNK    2
#define HS_META_NEXT         0

// *INDENT-OFF*
enum token_e {
  HS_TOK_NONE,        HS_TOK_METHOD,     HS_TOK_TARGET,     HS_TOK_VERSION,
  HS_TOK_HEADER_KEY,  HS_TOK_HEADER_VAL, HS_TOK_CHUNK_BODY, HS_TOK_BODY,
  HS_TOK_BODY_STREAM, HS_TOK_REQ_END,    HS_TOK_EOF,        HS_TOK_ERROR
};

enum char_type_e {
  HS_SPC,   HS_NL,  HS_CR,    HS_COLN,  HS_TAB,   HS_SCOLN,
  HS_DIGIT, HS_HEX, HS_ALPHA, HS_TCHAR, HS_VCHAR, HS_ETC,   HS_CHAR_TYPE_LEN
};

enum meta_state_e {
  M_WFK, M_ANY, M_MTE, M_MCL, M_CLV, M_MCK, M_SML, M_CHK, M_BIG, M_ZER, M_CSZ,
  M_CBD, M_LST, M_STR, M_SEN, M_BDY, M_END, M_ERR
};

enum meta_type_e {
  HS_META_NOT_CONTENT_LEN, HS_META_NOT_TRANSFER_ENC, HS_META_END_KEY,
  HS_META_END_VALUE,       HS_META_END_HEADERS,      HS_META_LARGE_BODY,
  HS_META_TYPE_LEN
};

enum state_e {
  ST, MT, MS, TR, TS, VN, RR, RN, HK, HS, HV, HR, HE,
  ER, HN, BD, CS, CB, CE, CR, CN, CD, C1, C2, BR, HS_STATE_LEN
};

static const int8_t _transitions[] = {
//                                            A-Z G-Z
//                spc \n  \r  :   \t  ;   0-9 a-f g-z tch vch etc
/* ST start */    BR, BR, BR, BR, BR, BR, BR, MT, MT, MT, BR, BR,
/* MT method */   MS, BR, BR, BR, BR, BR, MT, MT, MT, MT, BR, BR,
/* MS methodsp */ BR, BR, BR, BR, BR, BR, TR, TR, TR, TR, TR, BR,
/* TR target */   TS, BR, BR, TR, BR, TR, TR, TR, TR, TR, TR, BR,
/* TS targetsp */ BR, BR, BR, BR, BR, BR, VN, VN, VN, VN, VN, BR,
/* VN version */  BR, BR, RR, BR, BR, BR, VN, VN, VN, VN, VN, BR,
/* RR rl \r */    BR, RN, BR, BR, BR, BR, BR, BR, BR, BR, BR, BR,
/* RN rl \n */    BR, BR, BR, BR, BR, BR, HK, HK, HK, HK, BR, BR,
/* HK headkey */  BR, BR, BR, HS, BR, BR, HK, HK, HK, HK, BR, BR,
/* HS headspc */  HS, HS, HS, HV, HS, HV, HV, HV, HV, HV, HV, BR,
/* HV headval */  HV, BR, HR, HV, HV, HV, HV, HV, HV, HV, HV, BR,
/* HR head\r */   BR, HE, BR, BR, BR, BR, BR, BR, BR, BR, BR, BR,
/* HE head\n */   BR, BR, ER, BR, BR, BR, HK, HK, HK, HK, BR, BR,
/* ER hend\r */   BR, HN, BR, BR, BR, BR, BR, BR, BR, BR, BR, BR,
/* HN hend\n */   BD, BD, BD, BD, BD, BD, BD, BD, BD, BD, BD, BD,
/* BD body */     BD, BD, BD, BD, BD, BD, BD, BD, BD, BD, BD, BD,
/* CS chksz */    BR, BR, CR, BR, BR, CE, CS, CS, BR, BR, BR, BR,
/* CB chkbd */    CB, CB, CB, CB, CB, CB, CB, CB, CB, CB, CB, CB,
/* CE chkext */   BR, BR, CR, CE, CE, CE, CE, CE, CE, CE, CE, BR,
/* CR chksz\r */  BR, CN, BR, BR, BR, BR, BR, BR, BR, BR, BR, BR,
/* CN chksz\n */  CB, CB, CB, CB, CB, CB, CB, CB, CB, CB, CB, CB,
/* CD chkend */   BR, BR, C1, BR, BR, BR, BR, BR, BR, BR, BR, BR,
/* C1 chkend\r */ BR, C2, BR, BR, BR, BR, BR, BR, BR, BR, BR, BR,
/* C2 chkend\n */ BR, BR, BR, BR, BR, BR, CS, CS, BR, BR, BR, BR
};

static const int8_t _meta_transitions[] = {
//                 no chk
//                 not cl not te endkey endval end h  toobig
/* WFK wait */     M_WFK, M_WFK, M_WFK, M_ANY, M_END, M_ERR,
/* ANY matchkey */ M_MTE, M_MCL, M_WFK, M_ERR, M_END, M_ERR,
/* MTE matchte */  M_MTE, M_WFK, M_MCK, M_ERR, M_ERR, M_ERR,
/* MCL matchcl */  M_WFK, M_MCL, M_CLV, M_ERR, M_ERR, M_ERR,
/* CLV clvalue */  M_ERR, M_ERR, M_ERR, M_SML, M_ERR, M_ERR,
/* MCK matchchk */ M_WFK, M_ERR, M_ERR, M_CHK, M_ERR, M_ERR,
/* SML smallbdy */ M_SML, M_SML, M_SML, M_SML, M_BDY, M_BIG,
/* CHK chunkbdy */ M_CHK, M_CHK, M_CHK, M_CHK, M_ZER, M_ERR,
/* BIG bigbody */  M_BIG, M_BIG, M_BIG, M_BIG, M_STR, M_ERR,

//                         *** chunked body ***

//                 nonzer endsz  endchk
/* ZER zerochk */  M_CSZ, M_LST, M_ERR, M_ERR, M_ERR, M_ERR,
/* CSZ chksize */  M_CSZ, M_CBD, M_ERR, M_ERR, M_ERR, M_ERR,
/* CBD readchk */  M_CBD, M_CBD, M_ZER, M_ERR, M_ERR, M_ERR,
/* LST lastchk */  M_LST, M_END, M_END, M_ERR, M_ERR, M_ERR,

//                         *** streamed body ***

//                 next
/* STR readstr */  M_SEN, M_ERR, M_ERR, M_ERR, M_ERR, M_ERR,
/* SEN strend */   M_END, M_ERR, M_ERR, M_ERR, M_ERR, M_ERR,

//                         *** small body ***

//                 next
/* BDY readbody */ M_END, M_ERR, M_ERR, M_ERR, M_ERR, M_ERR,
/* END reqend */   M_WFK, M_ERR, M_ERR, M_ERR, M_ERR, M_ERR
};

static const int8_t _ctype[] = {
  HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,
  HS_ETC,   HS_ETC,   HS_TAB,   HS_NL,    HS_ETC,   HS_ETC,   HS_CR,
  HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,
  HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,
  HS_ETC,   HS_ETC,   HS_ETC,   HS_ETC,   HS_SPC,   HS_TCHAR, HS_VCHAR,
  HS_TCHAR, HS_TCHAR, HS_TCHAR, HS_TCHAR, HS_TCHAR, HS_VCHAR, HS_VCHAR,
  HS_TCHAR, HS_TCHAR, HS_TCHAR, HS_TCHAR, HS_TCHAR, HS_VCHAR, HS_DIGIT,
  HS_DIGIT, HS_DIGIT, HS_DIGIT, HS_DIGIT, HS_DIGIT, HS_DIGIT, HS_DIGIT,
  HS_DIGIT, HS_DIGIT, HS_COLN,  HS_SCOLN, HS_VCHAR, HS_VCHAR, HS_VCHAR,
  HS_VCHAR, HS_VCHAR, HS_HEX,   HS_HEX,   HS_HEX,   HS_HEX,   HS_HEX,
  HS_HEX,   HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA,
  HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA,
  HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA,
  HS_VCHAR, HS_VCHAR, HS_VCHAR, HS_TCHAR, HS_TCHAR, HS_TCHAR, HS_HEX,
  HS_HEX,   HS_HEX,   HS_HEX,   HS_HEX,   HS_HEX,   HS_ALPHA, HS_ALPHA,
  HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA,
  HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA,
  HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_ALPHA, HS_VCHAR, HS_TCHAR, HS_VCHAR,
  HS_TCHAR, HS_ETC
};

static int8_t const _token_start_states[] = {
//ST MT             MS TR             TS VN              RR RN HK
  0, HS_TOK_METHOD, 0, HS_TOK_TARGET, 0, HS_TOK_VERSION, 0, 0, HS_TOK_HEADER_KEY,
//HS HV                 HR HE ER HN BD           CS CB                 CE CR CN
  0, HS_TOK_HEADER_VAL, 0, 0, 0, 0, HS_TOK_BODY, 0, HS_TOK_CHUNK_BODY, 0, 0, 0,
//CD C1 C2
  0, 0, 0,
};

static char const *_status_text[] = {
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",

  //100s
  "Continue", "Switching Protocols", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",

  //200s
  "OK", "Created", "Accepted", "Non-Authoritative Information", "No Content",
  "Reset Content", "Partial Content", "", "", "",

  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",

  //300s
  "Multiple Choices", "Moved Permanently", "Found", "See Other", "Not Modified",
  "Use Proxy", "", "Temporary Redirect", "", "",

  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",

  //400s
  "Bad Request", "Unauthorized", "Payment Required", "Forbidden", "Not Found",
  "Method Not Allowed", "Not Acceptable", "Proxy Authentication Required",
  "Request Timeout", "Conflict",

  "Gone", "Length Required", "", "Payload Too Large", "", "", "", "", "", "",

  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",

  //500s
  "Internal Server Error", "Not Implemented", "Bad Gateway", "Service Unavailable",
  "Gateway Timeout", "", "", "", "", "",

  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", ""
};
// *INDENT-ON*

static iwrc _server_ref(struct server *server, struct server **out);
static void _server_unref(struct server *server);

static void _server_time(struct server *server, char out_buf[32]) {
  time_t rawtime;
  time(&rawtime);
  if (server->stime != rawtime) {
    server->stime = rawtime;
    struct tm *timeinfo = gmtime(&rawtime);
    strftime(out_buf, 32, "%a, %d %b %Y %T GMT", timeinfo);
  }
}

IW_INLINE void _stream_free_buffer(struct client *client) {
  if (client->stream.buf) {
    free(client->stream.buf);
  }
  memset(&client->stream, 0, sizeof(client->stream));
}

IW_INLINE void _tokens_free_buffer(struct client *client) {
  if (client->tokens.buf) {
    free(client->tokens.buf);
  }
  memset(&client->tokens, 0, sizeof(client->tokens));
}

static bool _stream_next(struct stream *stream, char *c) {
  stream->flags &= ~HS_SF_CONSUMED;
  if (stream->index >= stream->length) {
    return false;
  }
  *c = stream->buf[stream->index];
  return true;
}

static void _stream_consume(struct stream *stream) {
  if (stream->flags & HS_SF_CONSUMED) {
    return;
  }
  stream->flags |= HS_SF_CONSUMED;
  stream->index++;
  int nlen = stream->token.len + 1;
  stream->token.len = stream->token.type ? nlen : 0;
}

static void _stream_shift(struct stream *stream) {
  if (stream->token.index == stream->anchor) {
    return;
  }
  if (stream->token.len > 0) {
    char *dst = stream->buf + stream->anchor;
    char *src = stream->buf + stream->token.index;
    memcpy(dst, src, stream->length - stream->token.index);
  }
  stream->token.index = stream->anchor;
  stream->index = stream->anchor + stream->token.len;
  stream->length = stream->index;
}

IW_INLINE void _stream_anchor(struct stream *stream) {
  stream->anchor = stream->index;
}

IW_INLINE void _stream_begin_token(struct stream *stream, int token_type) {
  stream->token.type = token_type;
  stream->token.index = stream->index;
}

IW_INLINE struct token _stream_emit(struct stream *stream) {
  struct token token = stream->token;
  memset(&stream->token, 0, sizeof(stream->token));
  return token;
}

IW_INLINE bool _stream_can_contain(struct client *client, int64_t size) {
  return client->server->spec.request_buf_max_size - client->stream.index + 1 >= size;
}

static bool _stream_jump(struct stream *stream, int offset) {
  stream->flags |= HS_SF_CONSUMED;
  if (stream->index + offset > stream->length) {
    return false;
  }
  int nlen = stream->token.len + offset;
  stream->token.len = stream->token.type ? nlen : 0;
  return true;
}

static ssize_t _stream_jumpall(struct stream *stream) {
  stream->flags |= HS_SF_CONSUMED;
  ssize_t offset = stream->length - stream->index;
  stream->index += offset;
  stream->token.len = stream->token.type ? (int) (stream->token.len + offset) : 0;
  return offset;
}

///////////////////////////////////////////////////////////////////////////
//								              Client                                   //
///////////////////////////////////////////////////////////////////////////

IW_INLINE void _response_data_reset(struct response *response) {
  if (response->pool) {
    iwpool_destroy(response->pool);
    response->pool = 0;
  }
  if (response->body) {
    if (response->body_free) {
      response->body_free((void*) response->body);
      response->body_free = 0;
    }
    response->body = 0;
  }
  response->headers = 0;
  response->code = 200;
}

static iwrc _client_response_error(struct client *client, int code, char *response) {
  return iwn_http_response_write_simple(&client->request, code, "text/plain", response, -1, 0);
}

static void _client_reset(struct client *client) {
  client->state = HTTP_SESSION_INIT;
  _stream_free_buffer(client);
  _tokens_free_buffer(client);
}

static void _client_destroy(struct client *client) {
  if (!client) {
    return;
  }
  if (client->server) {
    if (client->server->spec.on_connection_close) {
      client->server->spec.on_connection_close(
        &(struct iwn_http_server_connection) {
        .server = (void*) client->server,
        .fd = client->fd
      });
    }
    _server_unref(client->server);
  }
  _stream_free_buffer(client);
  _tokens_free_buffer(client);
  iwpool_destroy(client->pool);
}

static iwrc _client_init(struct client *client) {
  iwrc rc = 0;
  client->flags = HTTP_AUTOMATIC;
  _stream_free_buffer(client);
  _tokens_free_buffer(client);
  memset(&client->parser, 0, sizeof(client->parser));
  client->chunk_cb = 0;
  client->chunk_cb_user_data = 0;
  client->tokens.capacity = 32;
  client->tokens.size = 0;
  client->tokens.buf = malloc(sizeof(client->tokens.buf[0]) * client->tokens.capacity);
  if (!client->tokens.buf) {
    client->tokens.capacity = 0;
    rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    goto finish;
  }
  if (client->server->spec.request_timeout_sec > 0) {
    iwn_poller_set_timeout(client->server->spec.poller, client->fd, client->server->spec.request_timeout_sec);
  }

finish:
  return rc;
}

static bool _client_write_bytes(struct client *client) {
  struct iwn_poller_adapter *pa = client->pa;
  struct stream *stream = &client->stream;
  ssize_t bytes = pa->write(pa,
                            (uint8_t*) stream->buf + stream->bytes_total,
                            stream->length - stream->bytes_total);
  if (bytes > 0) {
    stream->bytes_total += bytes;
  }
  return errno != EPIPE;
}

static void _client_write(struct client *client) {
  iwrc rc = 0;
  struct stream *stream = &client->stream;
  if (!_client_write_bytes(client)) {
    client->flags |= HTTP_END_SESSION;
    return;
  }
  if (stream->bytes_total != stream->length) {
    RCC(rc, finish, iwn_poller_arm_events(client->server->spec.poller, client->fd, IWN_POLLOUT));
    client->state = HTTP_SESSION_WRITE;
  } else if (client->flags & HTTP_CHUNKED_RESPONSE) {
    client->state = HTTP_SESSION_WRITE;
    _stream_free_buffer(client);
    if (client->server->spec.request_timeout_sec > 0) {
      iwn_poller_set_timeout(client->server->spec.poller, client->fd, client->server->spec.request_timeout_sec);
    }
    if (client->chunk_cb) {
      client->chunk_cb((void*) client, client->chunk_cb_user_data);
    }
  } else {
    if (client->flags & HTTP_KEEP_ALIVE) {
      if (client->server->spec.request_timeout_keepalive_sec > 0) {
        iwn_poller_set_timeout(client->server->spec.poller, client->fd,
                               client->server->spec.request_timeout_keepalive_sec);
      }
      _client_reset(client);
    } else {
      client->flags |= HTTP_END_SESSION;
    }
  }

finish:
  if (rc) {
    iwlog_ecode_error3(rc);
    client->flags |= HTTP_END_SESSION;
  }
}

static bool _client_read_bytes(struct client *client) {
  struct iwn_poller_adapter *pa = client->pa;
  struct stream *stream = &client->stream;
  struct server *server = client->server;
  if (stream->index < stream->length) {
    return true;
  }
  if (!stream->buf) {
    stream->length = 0;
    stream->capacity = 0;
    stream->buf = calloc(1, server->spec.request_buf_size);
    if (!stream->buf) {
      return false;
    }
    stream->capacity = server->spec.request_buf_size;
  }
  ssize_t bytes;
  do {
    bytes = pa->read(pa, (uint8_t*) stream->buf + stream->length, stream->capacity - stream->length);
    if (bytes > 0) {
      stream->length += bytes;
      stream->bytes_total += bytes;
    }
    if (stream->length == stream->capacity) {
      if (stream->capacity != server->spec.request_buf_max_size) {
        ssize_t ncap = stream->capacity * 2;
        if (ncap > server->spec.request_buf_max_size) {
          ncap = server->spec.request_buf_max_size;
        }
        char *nbuf = realloc(stream->buf, ncap);
        if (!nbuf) {
          bytes = 0;
          break;
        }
        stream->capacity = ncap;
        stream->buf = nbuf;
      } else {
        break;
      }
    }
  } while (bytes > 0);

  return bytes != 0;
}

IW_INLINE void _meta_trigger(struct parser *parser, int event) {
  int8_t to = _meta_transitions[parser->meta * HS_META_TYPE_LEN + event];
  parser->meta = to;
}

struct token _meta_emit_token(struct parser *parser) {
  struct token token = { 0 };
  switch (parser->meta) {
    case M_SEN:
      token.type = HS_TOK_CHUNK_BODY;
      _meta_trigger(parser, HS_META_NEXT);
      break;
    case M_END:
      token.type = HS_TOK_REQ_END;
      memset(parser, 0, sizeof(*parser));
      break;
  }
  return token;
}

struct token _transition(struct client *client, char c, int8_t from, int8_t to) {
  struct server *server = client->server;
  struct parser *parser = &client->parser;
  struct stream *stream = &client->stream;
  struct token emitted = { 0 };

  if (from == HN) {
    _stream_anchor(stream);
  }
  if (from != to) {
    int8_t type = _token_start_states[to];
    if (type != HS_TOK_NONE) {
      _stream_begin_token(stream, type);
    }
    if (from == CS) {
      _meta_trigger(parser, HS_META_END_CHK_SIZE);
    }
    if (to == HK) {
      if (++parser->header_count > server->spec.request_max_header_count) {
        emitted.type = HS_TOK_ERROR;
      }
    } else if (to == HS) {
      _meta_trigger(parser, HS_META_END_KEY);
      emitted = _stream_emit(stream);
    }
    parser->match_index = 0;
  }

  char low, m = '\0';
  int in_bounds = 0;
  ssize_t body_left = 0;

#define MATCH(str__, meta__) \
  in_bounds = parser->match_index < (int) sizeof(str__) - 1; \
  m = in_bounds ? str__[parser->match_index] : m; \
  low = c >= 'A' && c <= 'Z' ? c + 32 : c; \
  if (low != m) _meta_trigger(parser, meta__)

  switch (to) {
    case MS:
    case TS:
      emitted = _stream_emit(stream);
      break;
    case RR:
    case HR:
      _meta_trigger(parser, HS_META_END_VALUE);
      emitted = _stream_emit(stream);
      break;
    case HK:
      MATCH("transfer-encoding", HS_META_NOT_TRANSFER_ENC);
      MATCH("content-length", HS_META_NOT_CONTENT_LEN);
      parser->match_index++;
      break;
    case HV:
      if (parser->meta == M_MCK) {
        MATCH("chunked", HS_META_NOT_CHUNKED);
        parser->match_index++;
      } else if (parser->meta == M_CLV) {
        parser->content_length *= 10;
        parser->content_length += c - '0';
      }
      break;
    case HN:
      if (parser->meta == M_SML && !_stream_can_contain(client, parser->content_length)) {
        _meta_trigger(parser, HS_META_LARGE_BODY);
      }
      if (parser->meta == M_BIG || parser->meta == M_CHK) {
        emitted.type = HS_TOK_BODY_STREAM;
      }
      _meta_trigger(parser, HS_META_END_HEADERS);
      if (parser->content_length == 0 && parser->meta == M_BDY) {
        parser->meta = M_END;
      }
      if (parser->meta == M_END) {
        emitted.type = HS_TOK_BODY;
      }
      break;
    case CS:
      if (c != '0') {
        _meta_trigger(parser, HS_META_NON_ZERO);
      }
      if (c >= 'A' && c <= 'F') {
        parser->content_length *= 0x10;
        parser->content_length += c - 55;
      } else if (c >= 'a' && c <= 'f') {
        parser->content_length *= 0x10;
        parser->content_length += c - 87;
      } else if (c >= '0' && c <= '9') {
        parser->content_length *= 0x10;
        parser->content_length += c - '0';
      }
      break;
    case CB:
    case BD:
      if (parser->meta == M_STR) {
        _stream_begin_token(stream, HS_TOK_CHUNK_BODY);
      }
      body_left = parser->content_length - parser->body_consumed;
      if (_stream_jump(stream, body_left)) {
        emitted = _stream_emit(stream);
        _meta_trigger(parser, HS_META_NEXT);
        if (to == CB) {
          parser->state = CD;
        }
        parser->content_length = 0;
        parser->body_consumed = 0;
      } else {
        parser->body_consumed += _stream_jumpall(stream);
        if (parser->meta == M_STR) {
          emitted = _stream_emit(stream);
          _stream_shift(stream);
        }
      }
      break;
    case C2:
      _meta_trigger(parser, HS_META_END_CHUNK);
      break;
    case BR:
      emitted.type = HS_TOK_ERROR;
      break;
  }
#undef MATCH

  return emitted;
}

struct token _token_parse(struct client *client) {
  struct server *server = client->server;
  struct parser *parser = &client->parser;
  struct stream *stream = &client->stream;
  struct token token = _meta_emit_token(parser);

  if (token.type != HS_TOK_NONE) {
    return token;
  }

  char c = 0;
  while (_stream_next(stream, &c)) {
    int8_t type = c < 0 ? HS_ETC : _ctype[(size_t) c];
    int8_t to = _transitions[parser->state * HS_CHAR_TYPE_LEN + type];
    if (parser->meta == M_ZER && parser->state == HN && to == BD) {
      to = CS;
    }
    int8_t from = parser->state;
    parser->state = to;
    struct token emitted = _transition(client, c, from, to);
    _stream_consume(stream);
    if (emitted.type != HS_TOK_NONE) {
      return emitted;
    }
  }
  if (parser->state == CB) {
    _stream_shift(stream);
  }
  token = _meta_emit_token(parser);
  struct token *ct = &stream->token;
  if (  ct->type != HS_TOK_CHUNK_BODY
     && ct->type != HS_TOK_BODY
     && ct->len > server->spec.request_token_max_len) {
    token.type = HS_TOK_ERROR;
  }
  return token;
}

static struct iwn_http_val _token_get_string(struct client *client, int token_type) {
  struct iwn_http_val ret = { 0 };
  if (client->tokens.buf == 0) {
    return ret;
  }
  for (int i = 0; i < client->tokens.size; ++i) {
    struct token token = client->tokens.buf[i];
    if (token.type == token_type) {
      ret.buf = &client->stream.buf[token.index];
      ret.len = token.len;
      return ret;
    }
  }
  return ret;
}

static void _client_read(struct client *client) {
  struct token token;
  client->state = HTTP_SESSION_READ;
  if (client->server->spec.request_timeout_sec > 0) {
    iwn_poller_set_timeout(client->server->spec.poller, client->fd, client->server->spec.request_timeout_sec);
  }
  if (!_client_read_bytes(client)) {
    client->flags |= HTTP_END_SESSION;
    return;
  }
  do {
    token = _token_parse(client);
    if (token.type != HS_TOK_NONE) {
      if (client->tokens.size == client->tokens.capacity) {
        ssize_t ncap = client->tokens.capacity * 2;
        struct token *nbuf = realloc(client->tokens.buf, ncap * sizeof(client->tokens.buf[0]));
        if (!nbuf) {
          client->flags = HTTP_END_SESSION;
          return;
        }
        client->tokens.buf = nbuf;
        client->tokens.capacity = ncap;
      }
      client->tokens.buf[client->tokens.size++] = token;
    }
    switch (token.type) {
      case HS_TOK_ERROR:
        _client_response_error(client, 400, "Bad request");
        break;
      case HS_TOK_BODY:
      case HS_TOK_BODY_STREAM:
        if (token.type == HS_TOK_BODY_STREAM) {
          client->flags |= HTTP_STREAMED;
        }
        client->state = HTTP_SESSION_NOP;
        client->server->spec.request_handler(&client->request);
        break;
      case HS_TOK_CHUNK_BODY:
        client->state = HTTP_SESSION_NOP;
        if (client->chunk_cb) {
          client->chunk_cb(&client->request, client->chunk_cb_user_data);
        }
        break;
    }
  } while (token.type != HS_TOK_NONE && client->state == HTTP_SESSION_READ);
}

static int64_t _client_on_poller_adapter_event(struct iwn_poller_adapter *pa, void *user_data, uint32_t events) {
  iwrc rc = 0;
  int64_t resp = 0;
  struct client *client = user_data;
  if (client->pa != pa) {
    client->pa = pa;
  }
  switch (client->state) {
    case HTTP_SESSION_INIT:
      RCC(rc, finish, _client_init(client));
      client->state = HTTP_SESSION_READ;
    // NOTE: Fallthrough
    case HTTP_SESSION_READ:
      _client_read(client);
      break;
    case HTTP_SESSION_WRITE:
      _client_write(client);
      break;
  }
  if (client->flags & HTTP_END_SESSION) {
    resp = -1;
  }

finish:
  if (rc) {
    iwlog_ecode_error3(rc);
    resp = -1;
  }
  return resp;
}

static void _client_on_poller_adapter_dispose(struct iwn_poller_adapter *pa, void *user_data) {
  struct client *client = user_data;
  _client_destroy(client);
}

static iwrc _client_accept(struct server *server, int fd) {
  iwrc rc = 0;
  IWPOOL *pool = iwpool_create_empty();
  if (!pool) {
    rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    close(fd);
    return rc;
  }
  struct client *client = iwpool_calloc(sizeof(*client), pool);
  if (!client) {
    rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    goto finish;
  }
  client->pool = pool;
  client->fd = fd;
  RCC(rc, finish, _server_ref(server, &client->server));
  client->request.server_user_data = client->server->spec.user_data;

  int flags = fcntl(fd, F_GETFL, 0);
  RCN(finish, flags);
  RCN(finish, fcntl(fd, F_SETFL, flags | O_NONBLOCK));

  if (server->https) {
    RCC(rc, finish, iwn_brssl_server_poller_adapter(&(struct iwn_brssl_server_poller_adapter_spec) {
      .certs_data = server->spec.certs_data,
      .certs_data_in_buffer = server->spec.certs_data_in_buffer,
      .certs_data_len = server->spec.certs_data_len,
      .events = IWN_POLLIN,
      .events_mod = IWN_POLLET,
      .fd = fd,
      .on_dispose = _client_on_poller_adapter_dispose,
      .on_event = _client_on_poller_adapter_event,
      .poller = server->spec.poller,
      .private_key = server->spec.private_key,
      .private_key_in_buffer = server->spec.private_key_in_buffer,
      .private_key_len = server->spec.private_key_len,
      .timeout_sec = server->spec.request_timeout_sec,
      .user_data = client,
    }));
  } else {
    RCC(rc, finish,
        iwn_direct_poller_adapter(
          server->spec.poller, fd,
          _client_on_poller_adapter_event,
          _client_on_poller_adapter_dispose,
          client, IWN_POLLIN, IWN_POLLET,
          server->spec.request_timeout_sec));
  }

  if (client->server->spec.on_connection) {
    client->server->spec.on_connection(&(struct iwn_http_server_connection) {
      .server = (void*) server,
      .fd = fd
    });
  }

finish:
  if (rc) {
    close(fd);
    if (client) {
      _client_destroy(client);
    } else {
      iwpool_destroy(pool);
    }
  }

  return rc;
}

///////////////////////////////////////////////////////////////////////////
//								      Client Public API                                //
///////////////////////////////////////////////////////////////////////////

bool iwn_http_request_is_streamed(struct iwn_http_request *request) {
  struct client *client = (void*) request;
  return (client->flags & HTTP_STREAMED);
}

void iwn_http_request_free(struct iwn_http_request *request) {
  struct client *client = (void*) request;
  _stream_free_buffer(client);
  _tokens_free_buffer(client);
}

struct iwn_http_val iwn_http_request_target(struct iwn_http_request *request) {
  return _token_get_string((void*) request, HS_TOK_TARGET);
}

bool iwn_http_request_target_is(struct iwn_http_request *request, const char *target, ssize_t target_len) {
  struct iwn_http_val val = iwn_http_request_target(request);
  if (target_len < 0) {
    target_len = strlen(target);
  }
  return val.len == target_len && memcmp(val.buf, target, target_len) == 0;
}

struct iwn_http_val iwn_http_request_method(struct iwn_http_request *request) {
  return _token_get_string((void*) request, HS_TOK_METHOD);
}

struct iwn_http_val iwn_http_request_body(struct iwn_http_request *request) {
  return _token_get_string((void*) request, HS_TOK_BODY);
}

void iwn_http_request_chunk_next(
  struct iwn_http_request *request,
  void (*chunk_cb)(struct iwn_http_request*, void*),
  void *user_data) {
  struct client *client = (void*) request;
  client->chunk_cb = chunk_cb;
  client->chunk_cb_user_data = user_data;
  _client_read(client);
}

struct iwn_http_val iwn_http_request_chunk_get(struct iwn_http_request *request) {
  struct client *client = (void*) request;
  struct token *token = &client->tokens.buf[client->tokens.size - 1];
  return (struct iwn_http_val) {
           .buf = &client->stream.buf[token->index],
           .len = token->len
  };
}

void iwn_http_connection_set_automatic(struct iwn_http_request *request) {
  struct client *client = (void*) request;
  client->flags |= HTTP_AUTOMATIC;
  client->flags &= ~HTTP_KEEP_ALIVE;
}

void iwn_http_connection_set_keep_alive(struct iwn_http_request *request, bool keep_alive) {
  struct client *client = (void*) request;
  client->flags &= ~HTTP_AUTOMATIC;
  if (keep_alive) {
    client->flags |= HTTP_KEEP_ALIVE;
  } else {
    client->flags &= ~HTTP_KEEP_ALIVE;
  }
}

struct iwn_http_val iwn_http_request_header_get(struct iwn_http_request *request, const char *header_name) {
  struct client *client = (void*) request;
  size_t len = strlen(header_name);
  for (int i = 0; i < client->tokens.size; ++i) {
    struct token token = client->tokens.buf[i];
    if (token.type == HS_TOK_HEADER_KEY && token.len == len) {
      token = client->tokens.buf[i + 1];
      return (struct iwn_http_val) {
               .buf = &client->stream.buf[token.index],
               .len = token.len
      };
    }
  }
  return (struct iwn_http_val) {};
}

static bool _iteration_headers_assign(
  struct client       *client,
  struct iwn_http_val *key,
  struct iwn_http_val *val,
  int                 *iter) {

  struct token token = client->tokens.buf[*iter];
  if (client->tokens.buf[*iter].type == HS_TOK_BODY) {
    return false;
  }
  *key = (struct iwn_http_val) {
    .buf = &client->stream.buf[token.index],
    .len = token.len
  };
  (*iter)++;
  token = client->tokens.buf[*iter];
  *val = (struct iwn_http_val) {
    .buf = &client->stream.buf[token.index],
    .len = token.len
  };
  return true;
}

bool iwn_http_request_headers_iterate(
  struct iwn_http_request *request,
  struct iwn_http_val     *key,
  struct iwn_http_val     *val,
  int                     *iter) {

  struct client *client = (void*) request;
  if (*iter == 0) {
    for ( ; *iter < client->tokens.size; (*iter)++) {
      struct token token = client->tokens.buf[*iter];
      if (token.type == HS_TOK_HEADER_KEY) {
        return _iteration_headers_assign(client, key, val, iter);
      }
    }
    return false;
  } else {
    (*iter)++;
    return _iteration_headers_assign(client, key, val, iter);
  }
}

int iwn_http_response_code_get(struct iwn_http_request *request) {
  return ((struct client*) request)->response.code;
}

iwrc iwn_http_response_code_set(struct iwn_http_request *request, int code) {
  if (code < 0 || code > 599) {
    return IW_ERROR_INVALID_ARGS;
  }
  if (code == 0) {
    code = 200;
  }
  struct client *client = (void*) request;
  client->response.code = code;
  return 0;
}

struct iwn_http_val iwn_http_response_header_get(struct iwn_http_request *request, const char *header_name) {
  struct client *client = (void*) request;
  for (struct header *h = client->response.headers; h; h = h->next) {
    if (strcasecmp(h->name, header_name) == 0) {
      return (struct iwn_http_val) {
               .buf = h->value,
               .len = strlen(h->value)
      };
    }
  }
  return (struct iwn_http_val) {};
}

iwrc iwn_http_response_header_set(struct iwn_http_request *request, const char *header_name, const char *header_value) {
  iwrc rc = 0;
  struct client *client = (void*) request;
  struct response *response = &client->response;
  struct header *header = 0;

  if (!response->pool) {
    RCA(response->pool = iwpool_create_empty(), finish);
  }
  for (struct header *h = response->headers; h; h = h->next) {
    if (strcasecmp(h->name, header_name) == 0) {
      header = h;
      break;
    }
  }
  if (IW_LIKELY(header == 0)) {
    RCA(header = iwpool_alloc(sizeof(*header), response->pool), finish);
    RCA(header->name = iwpool_strdup2(response->pool, header_name), finish);
    for (char *p = header->name; *p != '\0'; ++p) {
      *p = (char) tolower((unsigned char) *p);
    }
    RCA(header->value = iwpool_strdup2(response->pool, header_value), finish);
    header->next = response->headers;
    response->headers = header;
  } else {
    RCA(header->value = iwpool_strdup2(response->pool, header_value), finish);
  }

finish:
  return rc;
}

void iwn_http_response_body_clear(struct iwn_http_request *request) {
  struct client *client = (void*) request;
  if (client->response.body) {
    if (client->response.body_free) {
      client->response.body_free((void*) client->response.body);
      client->response.body_free = 0;
    }
    client->response.body = 0;
  }
}

void iwn_http_response_body_set(
  struct iwn_http_request *request,
  const char              *body,
  ssize_t                  body_len,
  void (                  *body_free )(void*)) {

  if (!body || body_len == 0) {
    return;
  }
  struct client *client = (void*) request;
  if (body_len < 0) {
    body_len = strlen(body);
  }
  iwn_http_response_body_clear(request);
  client->response.body = body;
  client->response.body_len = body_len;
  client->response.body_free = body_free;
}

static void _client_autodetect_keep_alive(struct client *client) {
  struct iwn_http_val val = _token_get_string(client, HS_TOK_VERSION);
  if (val.buf == 0) {
    return;
  }
  int version = val.buf[val.len - 1] == '1';
  val = iwn_http_request_header_get(&client->request, "connection");
  if (  (val.len == 5 && strncasecmp(val.buf, "close", 5) == 0)
     || (val.len == 0 && version == HTTP_1_0)) {
    client->flags &= ~HTTP_KEEP_ALIVE;
  } else {
    client->flags |= HTTP_KEEP_ALIVE;
  }
}

static iwrc _client_response_headers_write(struct client *client, IWXSTR *xstr) {
  iwrc rc = 0;
  for (struct header *h = client->response.headers; h; h = h->next) {
    RCC(rc, finish, iwxstr_printf(xstr, "%s: %s\r\n", h->name, h->value));
  }
  if (!(client->flags & HTTP_CHUNKED_RESPONSE)) {
    RCC(rc, finish, iwxstr_printf(xstr, "content-length: %d\r\n", (int) client->response.body_len));
  }
  rc = iwxstr_cat(xstr, "\r\n", sizeof("\r\n") - 1);

finish:
  return rc;
}

static iwrc _client_response_headers_write_http(struct client *client, IWXSTR *xstr) {
  iwrc rc = 0;
  if (client->flags & HTTP_AUTOMATIC) {
    _client_autodetect_keep_alive(client);
  }
  if (client->flags & HTTP_KEEP_ALIVE) {
    iwn_http_response_header_set(&client->request, "connection", "keep-alive");
  } else {
    iwn_http_response_header_set(&client->request, "connection", "close");
  }
  if (client->response.code == 0) {
    client->response.code = 200;
  }
  char dbuf[32];
  _server_time(client->server, dbuf);
  RCC(rc, finish, iwxstr_printf(xstr, "HTTP/1.1 %d %s\r\nDate: %s\r\n",
                                client->response.code,
                                _status_text[client->response.code],
                                dbuf));

  rc = _client_response_headers_write(client, xstr);

finish:
  return rc;
}

static void _client_response_write(struct client *client, IWXSTR *xstr) {
  _stream_free_buffer(client);
  struct stream *s = &client->stream;
  s->length = iwxstr_size(xstr);
  s->capacity = iwxstr_asize(xstr);
  s->buf = iwxstr_ptr(xstr);
  s->bytes_total = 0;
  client->state = HTTP_SESSION_WRITE;
  iwxstr_destroy_keep_ptr(xstr);
  _response_data_reset(&client->response);
  _client_write(client);
}

iwrc iwn_http_response_end(struct iwn_http_request *request) {
  iwrc rc = 0;
  struct client *client = (void*) request;
  struct response *response = &client->response;
  IWXSTR *xstr = iwxstr_new2(client->server->spec.response_buf_size);
  if (!xstr) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  RCC(rc, finish, _client_response_headers_write_http(client, xstr));
  if (response->body) {
    RCC(rc, finish, iwxstr_cat(xstr, response->body, response->body_len));
  }
  _client_response_write(client, xstr);

finish:
  if (rc) {
    iwxstr_destroy(xstr);
  }
  return rc;
}

iwrc iwn_http_response_chunk_write(
  struct iwn_http_request *request,
  char *body,
  ssize_t body_len,
  void (*body_free)(void*),
  void (*chunk_cb)(struct iwn_http_request*, void*),
  void *chunk_cb_user_data) {

  iwrc rc = 0;
  struct client *client = (void*) request;
  struct response *response = &client->response;
  IWXSTR *xstr = iwxstr_new2(client->server->spec.response_buf_size);
  if (!xstr) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  if (!(client->flags & HTTP_CHUNKED_RESPONSE)) {
    iwn_http_response_header_set(request, "transfer-encoding", "chunked");
    RCC(rc, finish, _client_response_headers_write_http(client, xstr));
  }
  iwn_http_response_body_set(request, body, body_len, body_free);
  client->chunk_cb = chunk_cb;
  client->chunk_cb_user_data = chunk_cb_user_data;
  RCC(rc, finish, iwxstr_printf(xstr, "%X\r\n", response->body_len));
  RCC(rc, finish, iwxstr_cat(xstr, response->body, response->body_len));
  RCC(rc, finish, iwxstr_cat(xstr, "\r\n", sizeof("\r\n") - 1));

  _client_response_write(client, xstr);

finish:
  if (rc) {
    iwxstr_destroy(xstr);
  }
  return rc;
}

iwrc iwn_http_response_chunk_end(struct iwn_http_request *request) {
  iwrc rc = 0;
  struct client *client = (void*) request;
  IWXSTR *xstr = iwxstr_new2(client->server->spec.response_buf_size);
  if (!xstr) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  RCC(rc, finish, iwxstr_cat(xstr, "0\r\n", sizeof("0\r\n") - 1));
  RCC(rc, finish, _client_response_headers_write(client, xstr));
  RCC(rc, finish, iwxstr_cat(xstr, "\r\n", sizeof("\r\n") - 1));

  _client_response_write(client, xstr);

finish:
  if (rc) {
    iwxstr_destroy(xstr);
  }
  return rc;
}

iwrc iwn_http_response_write_simple(
  struct iwn_http_request *request,
  int                      status_code,
  const char              *content_type,
  char                    *body,
  ssize_t                  body_len,
  void (                  *body_free )(void*)) {

  iwrc rc = 0;
  RCC(rc, finish, iwn_http_response_code_set(request, status_code));
  if (!content_type) {
    content_type = "text/plain";
  }
  RCC(rc, finish, iwn_http_response_header_set(request, "content-type", content_type));
  iwn_http_response_body_set(request, body, body_len, body_free);
  rc = iwn_http_response_end(request);

finish:
  return rc;
}

///////////////////////////////////////////////////////////////////////////
//								             Server                                    //
///////////////////////////////////////////////////////////////////////////

static void _server_destroy(struct server *server) {
  if (!server) {
    return;
  }
  if (server->spec.on_server_dispose) {
    server->spec.on_server_dispose((void*) server);
  }
  if (server->fd > -1) {
    close(server->fd);
  }
  pthread_mutex_destroy(&server->mtx);
  iwpool_destroy(server->pool);
}

static int64_t _server_on_ready(const struct iwn_poller_task *t, uint32_t events) {
  struct server *server = t->user_data;
  int client_fd = 0;

  do {
    client_fd = accept(t->fd, 0, 0);
    if (client_fd == -1) {
      break;
    }
    iwrc rc = _client_accept(server, client_fd);
    if (rc) {
      iwlog_ecode_error(rc, "Failed to initiate client connection fd: %d", client_fd);
    }
  } while (1);

  return 0;
}

static iwrc _server_ref(struct server *server, struct server **out) {
  iwrc rc = 0;
  pthread_mutex_lock(&server->mtx);
  if (server->refs == 0) {
    *out = 0;
    rc = IW_ERROR_ASSERTION;
    iwlog_ecode_error(rc, "Server instance fd: %d is already disposed", server->fd);
    assert(server->refs);
  } else {
    *out = server;
    ++server->refs;
  }
  pthread_mutex_unlock(&server->mtx);
  return rc;
}

static void _server_unref(struct server *server) {
  int refs;
  pthread_mutex_lock(&server->mtx);
  refs = --server->refs;
  pthread_mutex_unlock(&server->mtx);
  if (refs < 1) {
    _server_destroy(server);
  }
}

static void _server_on_dispose(const struct iwn_poller_task *t) {
  struct server *server = t->user_data;
  _server_unref(server);
}

iwrc iwn_http_server_create(const struct iwn_http_server_spec *spec_, int *out_fd) {
  iwrc rc = 0;
  if (out_fd) {
    *out_fd = 0;
  }
  int optval;
  struct server *server;
  struct iwn_http_server_spec *spec;

  IWPOOL *pool = iwpool_create_empty();
  if (!pool) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  RCA(server = iwpool_calloc(sizeof(*server), pool), finish);
  memcpy(&server->mtx, &(pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER, sizeof(server->mtx));
  server->pool = pool;
  server->refs = 1;
  spec = &server->spec;
  memcpy(spec, spec_, sizeof(*spec));

  if (!spec->request_handler) {
    rc = IW_ERROR_INVALID_ARGS;
    iwlog_ecode_error2(rc, "No request_handler specified");
    goto finish;
  }
  if (!spec->poller) {
    rc = IW_ERROR_INVALID_ARGS;
    iwlog_ecode_error2(rc, "No poller specified");
    goto finish;
  }
  if (spec->http_socket_queue_size < 1) {
    spec->http_socket_queue_size = 64;
  }
  if (spec->request_buf_size < 1024) {
    spec->request_buf_size = 1024;
  }
  if (spec->request_timeout_sec == 0) {
    spec->request_timeout_sec = 20;
  }
  if (spec->request_timeout_keepalive_sec == 0) {
    spec->request_timeout_keepalive_sec = 120;
  }
  if (spec->request_token_max_len < 8192) {
    spec->request_token_max_len = 8192;
  }
  if (spec->request_max_header_count < 1) {
    spec->request_max_header_count = 127;
  }
  if (spec->request_buf_max_size < 1024 * 1024) {
    spec->request_buf_max_size = 8 * 1024 * 1024;
  }
  if (spec->response_buf_size < 1) {
    spec->response_buf_size = 1024;
  }

  server->https = spec->certs_data && spec->certs_data_len && spec->private_key && spec->private_key_len;
  if (server->https) {
    spec->certs_data = iwpool_strndup(pool, spec->certs_data, spec->certs_data_len, &rc);
    RCGO(rc, finish);
    spec->private_key = iwpool_strndup(pool, spec->private_key, spec->private_key_len, &rc);
    RCGO(rc, finish);
  }

  if (!spec->port) {
    spec->port = server->https ? 8443 : 8080;
  }
  if (!spec->listen) {
    spec->listen = "localhost";
  }
  RCA(spec->listen = iwpool_strdup2(pool, spec->listen), finish);

  struct iwn_poller_task task = {
    .user_data  = server,
    .on_ready   = _server_on_ready,
    .on_dispose = _server_on_dispose,
    .events     = IWN_POLLIN,
    .events_mod = IWN_POLLET,
    .poller     = spec->poller
  };

  struct addrinfo hints = {
    .ai_socktype = SOCK_STREAM,
    .ai_family   = AF_UNSPEC,
    .ai_flags    = AI_PASSIVE | AI_NUMERICSERV
  };

  struct addrinfo *result, *rp;
  char port[32];
  snprintf(port, sizeof(port), "%d", spec->port);

  int rci = getaddrinfo(spec->listen, port, &hints, &result);
  if (rci) {
    rc = IW_ERROR_FAIL;
    iwlog_error("Error getting local address and port: %s", gai_strerror(rci));
    goto finish;
  }

  optval = 1;
  for (rp = result; rp; rp = rp->ai_next) {
    task.fd = socket(rp->ai_family, rp->ai_socktype | SOCK_CLOEXEC, rp->ai_protocol);
    server->fd = task.fd;
    if (task.fd < 0) {
      continue;
    }
    if (setsockopt(task.fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &optval, sizeof(optval)) < 0) {
      iwlog_error("Error setsockopt: %s", strerror(errno));
    }
    if (bind(task.fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    } else {
      iwlog_error("Error binding socket: %s", strerror(errno));
    }
    close(task.fd);
  }

  freeaddrinfo(result);
  if (!rp) {
    rc = iwrc_set_errno(IW_ERROR_ERRNO, errno);
    iwlog_ecode_error2(rc, "Could not find any suitable address to bind");
    goto finish;
  }
  RCN(finish, optval = fcntl(task.fd, F_GETFL, 0));
  RCN(finish, fcntl(task.fd, F_SETFL, optval | O_NONBLOCK));
  RCN(finish, listen(task.fd, spec->http_socket_queue_size));

  server->server.listen = spec->listen;
  server->server.fd = task.fd;
  server->server.port = spec->port;
  server->server.user_data = spec->user_data;

  rc = iwn_poller_add(&task);

finish:
  if (rc) {
    if (server) {
      _server_destroy(server);
    } else {
      iwpool_destroy(pool);
    }
  } else if (out_fd) {
    *out_fd = server->fd;
  }
  return rc;
}