#include "ws_server.h"
#include "utils/base64.h"

#include <iowow/iwlog.h>
#include <iowow/iwxstr.h>
#include <bearssl_hash.h>
#include <wslay/wslay.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

struct msg {
  char       *buf;
  size_t      buf_len;
  struct msg *next;
};

struct ctx {
  struct iwn_ws_sess   sess;
  struct iwn_http_req *hreq;
  struct iwn_ws_handler_spec *spec;
  struct iwn_poller_adapter  *pa;
  struct msg *messages;
  wslay_event_context_ptr wc;
  pthread_mutex_t mtx;
};

static void _ctx_destroy(struct ctx *ctx) {
  if (ctx) {
    for (struct msg *m = ctx->messages; m; ) {
      struct msg *n = m->next;
      free(m->buf);
      free(m);
      m = n;
    }
    if (ctx->hreq && ctx->hreq->_ws_data == ctx) {
      ctx->hreq->_ws_data = 0;
    }
    if (ctx->spec && ctx->spec->on_session_dispose) {
      ctx->spec->on_session_dispose(&ctx->sess);
    }
    wslay_event_context_free(ctx->wc);
    pthread_mutex_destroy(&ctx->mtx);
    free(ctx);
  }
}

static void _route_handler_dispose(struct iwn_wf_ctx *ctx, void *user_data) {
  struct iwn_ws_handler_spec *spec = user_data;
  free(spec);
}

static void _on_request_dispose(struct iwn_http_req *hreq) {
  struct ctx *ctx = hreq->_ws_data;
  _ctx_destroy(ctx);
}

static ssize_t _wslay_recv_callback(
  wslay_event_context_ptr wctx,
  uint8_t                *buf,
  size_t                  len,
  int                     flags,
  void                   *user_data
  ) {
  ssize_t rci = -1;
  struct ctx *ctx = user_data;
  struct iwn_poller_adapter *pa = ctx->pa;
  assert(pa);

again:
  rci = pa->read(pa, buf, len);
  if (rci == -1) {
    if (errno == EINTR) {
      goto again;
    }
    if (errno == EAGAIN) {
      wslay_event_set_error(ctx->wc, WSLAY_ERR_WOULDBLOCK);
    } else {
      wslay_event_set_error(ctx->wc, WSLAY_ERR_CALLBACK_FAILURE);
    }
  } else if (rci == 0) {
    errno = EIO;
    rci = -1;
  }
  return rci;
}

static ssize_t _wslay_send_callback(
  wslay_event_context_ptr wctx,
  const uint8_t          *buf,
  size_t                  len,
  int                     flags,
  void                   *user_data
  ) {
  ssize_t rci = -1;
  struct ctx *ctx = user_data;
  struct iwn_poller_adapter *pa = ctx->pa;
  assert(pa);

again:
  rci = pa->write(pa, buf, len);
  if (rci == -1) {
    if (errno == EINTR) {
      goto again;
    }
    if (errno == EAGAIN) {
      wslay_event_set_error(ctx->wc, WSLAY_ERR_WOULDBLOCK);
    } else {
      wslay_event_set_error(ctx->wc, WSLAY_ERR_CALLBACK_FAILURE);
    }
  } else if (rci == 0) {
    errno = EIO;
    rci = -1;
  }
  return rci;
}

static int64_t _on_poller_adapter_event(struct iwn_poller_adapter *pa, void *user_data, uint32_t events) {
  assert(user_data);
  struct iwn_http_req *hreq = user_data;
  struct ctx *ctx = hreq->_ws_data;
  assert(ctx);
  int64_t ret = 0;
  struct msg *m = 0;

  if (ctx->pa != pa) {
    ctx->pa = pa;
  }

  pthread_mutex_lock(&ctx->mtx);

  if (wslay_event_want_write(ctx->wc) && wslay_event_send(ctx->wc) < 0) {
    goto finish;
  }
  if (wslay_event_want_read(ctx->wc) && wslay_event_recv(ctx->wc) < 0) {
    goto finish;
  }

  if (wslay_event_want_read(ctx->wc)) {
    ret |= IWN_POLLIN;
  }
  if (wslay_event_want_write(ctx->wc)) {
    ret |= IWN_POLLOUT;
  }

  if (ctx->messages) {
    m = ctx->messages;
    ctx->messages = 0; // Transfer ownership
  }

finish:
  pthread_mutex_unlock(&ctx->mtx);

  // In order to avoid deadlocks process message handlers out of `ctx->mtx`
  while (m) {
    struct msg *n = m->next;
    if (ret != -1 && !ctx->spec->msg_handler(&ctx->sess, m->buf, m->buf_len)) {
      ret = -1;
    }
    free(m->buf);
    free(m);
    m = n;
  }

  return ret == 0 ? -1 : ret;
}

static void _wslay_msg_recv_callback(
  wslay_event_context_ptr                   wctx,
  const struct wslay_event_on_msg_recv_arg *arg,
  void                                     *user_data
  ) {
  struct ctx *ctx = user_data;
  struct msg *m = 0;

  if (wslay_is_ctrl_frame(arg->opcode) || arg->msg_length == 0 || !ctx->spec->msg_handler) {
    return;
  }

  m = malloc(sizeof(*m));
  if (!m) {
    goto error;
  }
  m->next = 0;
  m->buf_len = arg->msg_length;
  m->buf = malloc(m->buf_len + 1);
  if (!m->buf) {
    goto error;
  }
  memcpy(m->buf, arg->msg, m->buf_len);
  m->buf[m->buf_len] = '\0';

  struct msg *mm = ctx->messages;
  if (mm) {
    while (mm->next) mm = m->next;
    mm->next = m;
  } else {
    ctx->messages = m;
  }

  return;

error:
  if (m) {
    free(m);
    free(m->buf);
  }
}

static bool _on_response_completed(struct iwn_http_req *hreq) {
  int lv = 1;
  struct ctx *ctx = hreq->_ws_data;
  if (!ctx) {
    return false;
  }
  if (wslay_event_context_server_init(&ctx->wc, &(struct wslay_event_callbacks) {
    .recv_callback = _wslay_recv_callback,
    .send_callback = _wslay_send_callback,
    .on_msg_recv_callback = _wslay_msg_recv_callback
  }, ctx)) {
    return false;
  }
  if (setsockopt(hreq->fd, IPPROTO_TCP, TCP_NODELAY, &lv, (socklen_t) sizeof(lv)) == -1) {
    return false;
  }
  iwn_poller_set_timeout(ctx->pa->poller, ctx->pa->fd, 0);
  iwn_http_inject_poller_events_handler(hreq, _on_poller_adapter_event);
  return true;
}

static int _route_handler(struct iwn_wf_req *req, void *user_data) {
  iwrc rc = 0;
  int rv = -1;

  struct ctx *ctx = 0;
  struct iwn_http_req *hreq = req->http;
  struct iwn_ws_handler_spec *spec = user_data;

  struct iwn_val val = iwn_http_request_header_get(hreq, "upgrade", IW_LLEN("upgrade"));
  if (val.len != IW_LLEN("websocket") || strncasecmp(val.buf, "websocket", val.len) != 0) {
    goto finish;
  }

  val = iwn_http_request_header_get(hreq, "sec-websocket-version", IW_LLEN("sec-websocket-version"));
  if (val.len != IW_LLEN("13") || strncmp(val.buf, "13", val.len) != 0) {
    goto finish;
  }

  struct iwn_val ws_key = iwn_http_request_header_get(hreq, "sec-websocket-key", IW_LLEN("sec-websocket-key"));
  if (!ws_key.len) {
    goto finish;
  }

  RCC(rc, finish, iwn_http_response_header_set(hreq, "upgrade", "websocket", IW_LLEN("websocket")));

  struct iwn_val ws_protocol
    = iwn_http_request_header_get(hreq, "sec-websocket-protocol", IW_LLEN("sec-websocket-protocol"));
  if (ws_protocol.len) {
    RCC(rc, finish, iwn_http_response_header_set(hreq, "sec-websocket-protocol", ws_protocol.buf, ws_protocol.len));
  }

  {
    size_t len = ws_key.len;
    unsigned char buf[len + IW_LLEN(WS_MAGIC13)];
    unsigned char hbuf[br_sha1_SIZE];
    char vbuf[br_sha1_SIZE * 2];
    memcpy(buf, ws_key.buf, len);
    memcpy(buf + len, WS_MAGIC13, IW_LLEN(WS_MAGIC13));

    br_sha1_context sha1;
    br_sha1_init(&sha1);
    br_sha1_update(&sha1, buf, sizeof(buf));
    br_sha1_out(&sha1, hbuf);

    if (!iwn_base64_encode(vbuf, sizeof(vbuf), &len, hbuf, sizeof(hbuf), base64_VARIANT_ORIGINAL)) {
      goto finish;
    }
    RCC(rc, finish, iwn_http_response_header_set(hreq, "sec-websocket-accept", vbuf, len));
  }

  RCA(ctx = calloc(1, sizeof(*ctx)), finish);
  ctx->hreq = hreq;
  ctx->sess.req = req;
  ctx->spec = ctx->sess.spec = spec;
  memcpy(&ctx->mtx, &(pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER, sizeof(ctx->mtx));

  hreq->_ws_data = ctx;
  hreq->on_request_dispose = _on_request_dispose;
  hreq->on_response_completed = _on_response_completed;

  iwn_http_connection_set_upgrade(hreq);
  if (iwn_http_response_write(hreq, 101, "", 0, 0, 0)) {
    rv = 1;
  }

finish:
  if (rc) {
    rv = -1;
    iwlog_ecode_error3(rc);
  }
  if (rv == -1) {
    _ctx_destroy(ctx);
  }
  return rv;
}

void iwn_ws_server_route_attach(struct iwn_wf_route *route, const struct iwn_ws_handler_spec *spec_) {
  assert(route && spec_);
  struct iwn_ws_handler_spec *spec = malloc(sizeof(*spec));
  if (spec) {
    memcpy(spec, spec_, sizeof(*spec));
    route->handler = _route_handler;
    route->handler_dispose = _route_handler_dispose;
    route->user_data = spec;
  }
}

bool iwn_ws_server_write_text(struct iwn_ws_sess *sess, const char *buf, size_t buf_len) {
  struct ctx *ctx = (void*) sess;
  if (!ctx || !buf) {
    return false;
  }
  if (buf_len == 0) {
    return true;
  }
  pthread_mutex_lock(&ctx->mtx);
  if (wslay_event_queue_msg(ctx->wc, &(struct wslay_event_msg) {
    .opcode = WSLAY_TEXT_FRAME,
    .msg = (void*) buf,
    .msg_length = buf_len
  })) {
    pthread_mutex_unlock(&ctx->mtx);
    return false;
  }
  bool ret = 0 == iwn_poller_arm_events(ctx->pa->poller, ctx->pa->fd, IWN_POLLOUT | IWN_POLLET);
  pthread_mutex_unlock(&ctx->mtx);
  return ret;
}