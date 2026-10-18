/* bmbot: a Bitmessage bot library with a C ABI, for programs in any language (Go cgo, Python
 * ctypes, ...). It opens a vault and a mailbox from a phrase, runs the node as a child process,
 * and hands out the chats and letters that come as JSON events.
 *
 * All strings are UTF-8 char*. Nothing but bmbot *, int, const char * and char ** crosses the
 * ABI: no struct, no callback, no thread-local state, so a handle may be used from any thread
 * (calls on one handle are serialised inside). Every function that can fail returns an int:
 * 0 ok, 1 timeout (bmbot_next only), negative for an error. An optional char **error receives
 * English text; every string a function returns, and every error, is freed with bmbot_free.
 *
 * Config (bmbot_open), a JSON object:
 *   data_dir       where the vault, the mailbox and the node's files are kept (made if missing;
 *                  one process at a time may hold it)
 *   phrase_file    a file whose first line is the secret phrase the address comes from; it also
 *                  makes the vault's password, so the disk is of no use without it
 *   phrase         or the phrase itself, which is public (a test or a demo): the vault keeps a
 *                  constant password
 *   secret         true or false, to override the two rules above
 *   label          the label of the identity (default "Bot")
 *   hang_seconds   abort() when the driver loop has not run for this long, so that Docker or
 *                  systemd starts the process again (default 0: never)
 *   node           an object:
 *     bin               "auto" (default: $BMBOT_NODE, bmnode beside the library or the program,
 *                       then the PATH), a path, or "" to run no node here
 *     peer, proxy       IP:port
 *     public            true (default) joins the public network; false links to peer alone
 *     listen, port      accept incoming connections, on this port (default 8444)
 *     max_outgoing, max_incoming, watchdog_minutes, retention_mb, retention_days
 *
 * Events (bmbot_next): one JSON object each.
 *   {"id":..., "kind":"chat"|"letter", "to":..., "from":..., "subject":..., "text":...,
 *    "seqno":..., "received":...}
 *   {"kind":"note", "text":..., "time":...}   a problem or a watchdog restart; no id, not kept
 * The inbox is the mailbox's unread chats and letters, oldest first. It is durable and at least
 * once: an event is handed out once per run and marked read only by bmbot_done; after a crash or a
 * restart the ones not done come back.
 */
#ifndef BMBOT_H
#define BMBOT_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define BMBOT_API __attribute__((visibility("default")))
#else
#define BMBOT_API
#endif

#define BMBOT_ABI_VERSION 1

typedef struct bmbot bmbot;

enum {
    BMBOT_OK = 0,
    BMBOT_TIMEOUT = 1,
    BMBOT_E_ARGUMENT = -1, /* bad config, address or text; unknown event */
    BMBOT_E_STATE = -2,    /* closed, or the data directory is in use */
    BMBOT_E_AUTH = -3,     /* the phrase is not the one the vault was made with */
    BMBOT_E_IO = -4,
    BMBOT_E_NETWORK = -5,
    BMBOT_E_INTERNAL = -6
};

BMBOT_API int bmbot_abi_version(void);

/* Opens the bot and starts its node and its driver thread. */
BMBOT_API int bmbot_open(const char *config_json, bmbot **bot, char **error);
/* Wakes the callers waiting in bmbot_next, which get BMBOT_E_STATE, stops the node and frees the
 * handle. Do not use the handle afterwards. */
BMBOT_API void bmbot_close(bmbot *bot);

/* Waits up to timeout_ms for an event: 0 with *event_json set (notes first), 1 when none came. */
BMBOT_API int bmbot_next(bmbot *bot, int timeout_ms, char **event_json, char **error);
/* Marks the event's chat or letter as read. Repeating it is harmless. */
BMBOT_API int bmbot_done(bmbot *bot, const char *event_id, char **error);

/* from may be NULL or "": the bot's own address. *id is the id to follow with bmbot_state. */
BMBOT_API int bmbot_send_chat(bmbot *bot, const char *from, const char *to, const char *text,
                              char **id, char **error);
BMBOT_API int bmbot_send_letter(bmbot *bot, const char *from, const char *to, const char *subject,
                                const char *body, char **id, char **error);
/* The delivery state of a chat or letter this bot sent ("queued", "awaiting_pubkey",
 * "calculating_message", "awaiting_ack", "acknowledged", ...), or "" if there is none. */
BMBOT_API int bmbot_state(bmbot *bot, const char *id, char **state, char **error);
/* JSON: time, uptime, address, network, working, up_to_date and node{managed, running, pid,
 * peers, outgoing, incoming, objects, pending, listen_port, status_age, restarts,
 * watchdog_restarts}. */
BMBOT_API int bmbot_status(bmbot *bot, char **status_json, char **error);

BMBOT_API void bmbot_free(void *ptr);

#ifdef __cplusplus
}
#endif
#endif
