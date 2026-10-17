#include "bmbot.h"
#include <string.h>

// the opaque type is defined in the header inside extern "C"
// we just provide the definitions here

// ------------------------------------------------------
// bmbot_abi_version
// ------------------------------------------------------
int bmbot_abi_version(void) { return 1; }

// ------------------------------------------------------
// bmbot_open
// ------------------------------------------------------
int bmbot_open(const char *config_json, bmbot **bot, char **error) {
    return 1;
}

// ------------------------------------------------------
// bmbot_close
// ------------------------------------------------------
void bmbot_close(bmbot *bot) {
// nothing yet
}

// ------------------------------------------------------
// bmbot_free
// ------------------------------------------------------
void bmbot_free(void *ptr) {
// nothing yet
}

// ------------------------------------------------------
// bmbot_next
// ------------------------------------------------------
int bmbot_next(bmbot *bot, int timeout_ms, char **event_json, char **error) {
    (void)bot; (void)timeout_ms;
    if (event_json) *event_json = nullptr;
    if (error) *error = strdup("not ready");
    return 1;
}

// ------------------------------------------------------
// bmbot_done
// ------------------------------------------------------
int bmbot_done(bmbot *bot, const char *event_id, char **error) {
    (void)bot; (void)event_id;
    if (error) *error = strdup("not implemented");
    return 1;
}

// ------------------------------------------------------
// bmbot_status
// ------------------------------------------------------
int bmbot_status(bmbot *bot, char **status_json, char **error) {
    (void)bot;
    if (status_json) *status_json = strdup("{\"peers\":0,\"objects\":0,\"uptime\":0}");
    if (error) *error = nullptr;
    return 0;
}

// ------------------------------------------------------
// bmbot_state
// ------------------------------------------------------
int bmbot_state(bmbot *bot, const char *id, char **state, char **error) {
    (void)bot; (void)id;
    if (state) *state = strdup("available");
    if (error) *error = nullptr;
    return 0;
}

// ------------------------------------------------------
// bmbot_send_chat
// ------------------------------------------------------
int bmbot_send_chat(bmbot *bot, const char *from, const char *to,
                    const char *text, char **id, char **error) {
    (void)bot; (void)from; (void)to; (void)text;
    if (id) *id = strdup("");
    if (error) *error = strdup("not implemented");
    return 1;
}

// ------------------------------------------------------
// bmbot_send_letter
// ------------------------------------------------------
int bmbot_send_letter(bmbot *bot, const char *from, const char *to,
                      const char *subject, const char *body,
                      char **id, char **error) {
    (void)bot; (void)from; (void)to; (void)subject; (void)body;
    if (id) *id = strdup("");
    if (error) *error = strdup("not implemented");
    return 1;
}
