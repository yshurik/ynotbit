#pragma once

// bmbot C ABI — a bot library with its own node process.
// All strings are UTF-8 char*.  Every function that can fail returns an int
// (0 ok, 1 timeout, negative errors: argument, state, auth, io, network,
// internal).  An optional char **error is returned (English text, freed with
// bmbot_free).  Nothing but bmbot *, int, const char * and char ** crosses
// the ABI.  The library has no thread-local state and no callbacks.
// All functions use C linkage so Python ctypes and Go cgo can call them.

#ifdef __cplusplus
extern "C" {
// opaque per-bot state; the public type is a pointer to this struct.
struct impl;
struct bmbot { struct impl *data; };

// ABI functions — C linkage, callable from Python ctypes / Go cgo.
int  bmbot_abi_version(void);
int  bmbot_open(const char *config_json, bmbot **bot, char **error);
void bmbot_close(bmbot *bot);                       /* wakes waiters, which get a state error */
int  bmbot_next(bmbot *bot, int timeout_ms, char **event_json, char **error);
int  bmbot_done(bmbot *bot, const char *event_id, char **error);
int  bmbot_send_chat(bmbot *bot, const char *from, const char *to,
                     const char *text, char **id, char **error);
int  bmbot_send_letter(bmbot *bot, const char *from, const char *to,
                       const char *subject, const char *body,
                       char **id, char **error);
int  bmbot_state(bmbot *bot, const char *id, char **state, char **error);
int  bmbot_status(bmbot *bot, char **status_json, char **error);
void bmbot_free(void *ptr);
} // extern "C"
#endif
