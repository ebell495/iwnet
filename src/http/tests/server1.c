
#include "utils/tests.h"
#include "http_server.h"

#include <iowow/iwxstr.h>

#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

static struct iwn_poller *poller;

static void _on_signal(int signo) {
  fprintf(stderr, "\nClosing on signal: %d\n", signo);
  if (poller) {
    iwn_poller_shutdown_request(poller);
  }
}

static void _server_on_dispose(const struct iwn_http_server *srv) {
  fprintf(stderr, "On server dispose\n");
}

static void _on_connection(const struct iwn_http_server_connection *conn) {
  fprintf(stderr, "On connection: %d\n", conn->fd);
}

static void _on_connection_close(const struct iwn_http_server_connection *conn) {
  fprintf(stderr, "On connection close: %d\n", conn->fd);
}

static void _chunk_req_cb(struct iwn_http_request *req, void *data) {
  IWXSTR *xstr = data;
  IWN_ASSERT_FATAL(xstr);
  struct iwn_http_val val = iwn_http_request_chunk_get(req);
  if (val.len > 0) {
    iwrc rc = iwxstr_cat(xstr, val.buf, val.len);
    IWN_ASSERT_FATAL(rc == 0);    
    iwn_http_request_chunk_next(req, _chunk_req_cb, xstr);
  } else {
    char *body = iwxstr_ptr(xstr);
    size_t body_len = iwxstr_size(xstr);
    iwxstr_destroy_keep_ptr(xstr);
    iwn_http_response_body_set(req, body, body_len, free);
    iwrc rc = iwn_http_response_end(req);
    IWN_ASSERT(rc == 0);
  }
}

static bool _request_handler(struct iwn_http_request *req) {
  iwrc rc = 0;

  if (iwn_http_request_target_is(req, "/empty", -1)) {
    ; // No body
  } else if (iwn_http_request_target_is(req, "/echo", -1)) {
    struct iwn_http_val val = iwn_http_request_body(req);
    RCC(rc, finish, iwn_http_response_header_set(req, "content-type", "text/plain"));
    iwn_http_response_body_set(req, val.buf, val.len, 0);
  } else if (iwn_http_request_target_is(req, "/host", -1)) {
    struct iwn_http_val val = iwn_http_request_header_get(req, "Host");
    iwn_http_response_body_set(req, val.buf, val.len, 0);
  } else if (iwn_http_request_target_is(req, "/large", -1)) {
    IWN_ASSERT(iwn_http_request_is_streamed(req));
    IWXSTR *xstr;
    RCA(xstr = iwxstr_new(), finish);
    iwn_http_request_chunk_next(req, _chunk_req_cb, xstr);
    goto finish;
  } else {
    RCC(rc, finish, iwn_http_response_header_set(req, "content-type", "text/plain"));
    iwn_http_response_body_set(req, "Hello!", -1, 0);
  }

  rc = iwn_http_response_end(req);

finish:
  if (rc) {
    iwlog_ecode_error3(rc);
  }
  return true;
}

int main(int argc, char *argv[]) {
  iwrc rc = 0;
  iwlog_init();

  signal(SIGPIPE, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  signal(SIGALRM, SIG_IGN);
  signal(SIGUSR1, SIG_IGN);
  signal(SIGUSR2, SIG_IGN);
  if (signal(SIGTERM, _on_signal) == SIG_ERR) {
    return EXIT_FAILURE;
  }
  if (signal(SIGINT, _on_signal) == SIG_ERR) {
    return EXIT_FAILURE;
  }

  RCC(rc, finish, iwn_poller_create(1, 1, &poller));
  RCC(rc, finish, iwn_http_server_create(&(struct iwn_http_server_spec) {
    .listen = "localhost",
    .port = 9292,
    .poller = poller,
    .user_data = poller,
    .request_handler = _request_handler,
    .on_connection = _on_connection,
    .on_connection_close = _on_connection_close,
    .on_server_dispose = _server_on_dispose,
    .request_timeout_sec = -1,
    .request_timeout_keepalive_sec = -1
  }, 0));

  iwn_poller_poll(poller);

finish:
  IWN_ASSERT(rc == 0);
  iwn_poller_destroy(&poller);
  return iwn_asserts_failed > 0 ? 1 : 0;
}