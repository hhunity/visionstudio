#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ── state ── */
#define CAPTURE_STATE_DISCONNECTED  0
#define CAPTURE_STATE_CONNECTING    1
#define CAPTURE_STATE_CONNECTED     2
#define CAPTURE_STATE_ERROR         3

/* ── event kind ── */
#define CAPTURE_EVENT_DISCONNECTED  0
#define CAPTURE_EVENT_ERROR         1
#define CAPTURE_EVENT_CAPTURE_DONE  2

typedef struct capture_client capture_client_t;

typedef struct {
    int  kind;           /* CAPTURE_EVENT_* */
    char path[512];      /* CAPTURE_EVENT_CAPTURE_DONE */
    char message[512];   /* CAPTURE_EVENT_ERROR */
} capture_event_t;

typedef void (*capture_log_fn)(const char* msg, void* userdata);

/**
 * Create a new capture client.
 * All path strings must be non-null and null-terminated (e.g. "/connect").
 * The returned pointer must be freed with capture_client_free().
 */
capture_client_t* capture_client_new(
    const char* host,
    int         port,
    const char* connect_path,
    const char* start_path,
    const char* stop_path,
    const char* disconnect_path,
    const char* sse_path,
    int         timeout_ms
);

/** Disconnect, join all threads and free memory. */
void capture_client_free(capture_client_t* client);

/** Enqueue a connect command (SSE first, then PUT /connect). */
void capture_client_connect(capture_client_t* client);

/** Interrupt SSE and enqueue a disconnect command. */
void capture_client_disconnect(capture_client_t* client);

/** Enqueue POST /start (only when connected). */
void capture_client_start_capture(capture_client_t* client);

/** Enqueue POST /stop (only when connected). */
void capture_client_stop_capture(capture_client_t* client);

/** Returns CAPTURE_STATE_* without blocking. */
int  capture_client_get_state(const capture_client_t* client);

/**
 * Pop one event from the queue into *out.
 * Returns 1 if an event was available, 0 otherwise.
 */
int  capture_client_poll_event(capture_client_t* client, capture_event_t* out);

/** Copy the last error string into buf (null-terminated, truncated to buf_len). */
void capture_client_get_last_error(const capture_client_t* client,
                                    char* buf, int buf_len);

/**
 * Install a log callback.  log_fn may be NULL to remove the logger.
 * userdata is passed through unchanged; it must remain valid for the
 * lifetime of the client.
 */
void capture_client_set_logger(capture_client_t* client,
                                capture_log_fn log_fn, void* userdata);

#ifdef __cplusplus
}
#endif
