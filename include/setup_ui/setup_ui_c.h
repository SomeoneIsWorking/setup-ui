// setup_ui.h — C ABI over the browser-based setup server for C consumers.
//
// Mirrors the C++ Server with explicit handles and fixed-capacity error
// buffers. One session per handle; polling fills a caller-provided event
// array. Validation runs through one callback with a stable shape: the
// consumer receives the staged file paths and returns 0 for success, nonzero
// for rejection with an optional message copied into its buffer.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// NOLINTNEXTLINE(performance-enum-size)
enum {
  SETUP_UI_EVENT_STARTED = 0,
  SETUP_UI_EVENT_UPLOADED = 1,
  SETUP_UI_EVENT_VALIDATED = 2,
  SETUP_UI_EVENT_FAILED = 3,
};

typedef struct setup_ui_event {
  const char *message;    /* Failed: validator text; Uploaded: file name */
  uint64_t uploaded_size; /* Uploaded only */
  int kind;               /* SETUP_UI_EVENT_* */
  uint16_t port;          /* Started only */
} setup_ui_event;

typedef struct setup_ui_file {
  const char *name;  /* exact expected file name */
  const char *label; /* human-readable step label */
} setup_ui_file;

typedef int (*setup_ui_validate_fn)(void *userdata, const char *const *paths,
                                    const char *const *names, size_t count, char *error,
                                    size_t error_capacity);

typedef struct setup_ui_config {
  const char *title;
  const char *message;
  const char *hint;
  const char *footer;
  const setup_ui_file *files;
  size_t file_count;
  int accepts_archive;           /* nonzero: one bounded ZIP is accepted */
  setup_ui_validate_fn validate; /* required; runs on the poll caller's thread */
  void *userdata;                /* passed back to the validator */
} setup_ui_config;

/* Validator: return 0 to accept the staged set. On rejection, copy a short
 * human-readable reason into error (NUL-terminated) and return nonzero. */
typedef struct setup_ui_server setup_ui_server;

setup_ui_server *setup_ui_server_new(const setup_ui_config *config, const char *staging_root,
                                     int lan_scope, /* nonzero: LocalNetwork */
                                     uint64_t max_file_bytes);
void setup_ui_server_free(setup_ui_server *server);

/* Returns 1 on success, 0 on failure (detail via setup_ui_last_error). */
int setup_ui_server_start(setup_ui_server *server);
void setup_ui_server_stop(setup_ui_server *server);

/* Bound port while running; 0 when stopped. */
uint16_t setup_ui_server_port(setup_ui_server *server);

/* Fills up to capacity events and returns the number written. */
size_t setup_ui_server_poll(setup_ui_server *server, setup_ui_event *events, size_t capacity);

/* Copies the browser URL into url (NUL-terminated). Returns 0 on truncation. */
int setup_ui_server_url(setup_ui_server *server, char *url, size_t url_capacity);

/* Short human-readable detail for the most recent failure, or "". */
const char *setup_ui_last_error(setup_ui_server *server);

#ifdef __cplusplus
} /* extern "C" */
#endif
