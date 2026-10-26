// The C ABI of bmbot.h over bmbot_lib::Bot: nothing here but argument checks, the mapping of
// exceptions to codes and the ownership of the strings that cross.
#include "bmbot.h"
#include "bot.h"
#include <bmc/error.h>
#include <bmc/protocol.h>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

struct bmbot {
    std::unique_ptr<bmbot_lib::Bot> bot;
};

namespace {
char *copyOf(const std::string &text) {
    char *out = static_cast<char *>(std::malloc(text.size() + 1));
    if (!out)
        return nullptr;
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

int codeOf(bmc::Code code) {
    switch (code) {
    case bmc::Code::Invalid:
    case bmc::Code::Argument:
        return BMBOT_E_ARGUMENT;
    case bmc::Code::State:
        return BMBOT_E_STATE;
    case bmc::Code::Auth:
        return BMBOT_E_AUTH;
    case bmc::Code::Io:
        return BMBOT_E_IO;
    case bmc::Code::Network:
        return BMBOT_E_NETWORK;
    case bmc::Code::Internal:
        return BMBOT_E_INTERNAL;
    }
    return BMBOT_E_INTERNAL;
}

int refuse(int code, const std::string &text, char **error) {
    if (error)
        *error = copyOf(text);
    return code;
}

// Runs f, which returns a code, and turns what it throws into one.
template <class F> int guarded(char **error, F &&f) {
    if (error)
        *error = nullptr;
    try {
        return f();
    } catch (const bmc::Error &e) {
        return refuse(codeOf(e.code()), e.what(), error);
    } catch (const std::exception &e) {
        return refuse(BMBOT_E_INTERNAL, e.what(), error);
    } catch (...) {
        return refuse(BMBOT_E_INTERNAL, "Unknown error", error);
    }
}

// A handle, or the refusal that goes with a null one.
bmbot_lib::Bot *botOf(bmbot *handle) {
    if (!handle || !handle->bot)
        throw bmc::Error(bmc::Code::Argument, "The bot handle is null");
    return handle->bot.get();
}
const char *textOrEmpty(const char *s) {
    return s ? s : "";
}
void requireText(const char *s, const char *what) {
    if (!s)
        throw bmc::Error(bmc::Code::Argument, std::string(what) + " is required");
}
} // namespace

extern "C" {

int bmbot_abi_version(void) {
    return BMBOT_ABI_VERSION;
}

int bmbot_open(const char *config_json, bmbot **bot, char **error) {
    return guarded(error, [&]() -> int {
        if (!bot)
            throw bmc::Error(bmc::Code::Argument, "bot is required");
        *bot = nullptr;
        requireText(config_json, "The configuration");
        auto handle = std::make_unique<bmbot>();
        handle->bot = std::make_unique<bmbot_lib::Bot>(bmbot_lib::Options::fromJson(config_json));
        *bot = handle.release();
        return BMBOT_OK;
    });
}

void bmbot_close(bmbot *bot) {
    if (!bot)
        return;
    try {
        if (bot->bot)
            bot->bot->close();
    } catch (...) {
    }
    delete bot;
}

int bmbot_next(bmbot *bot, int timeout_ms, char **event_json, char **error) {
    return guarded(error, [&]() -> int {
        if (!event_json)
            throw bmc::Error(bmc::Code::Argument, "event_json is required");
        *event_json = nullptr;
        std::string event;
        if (botOf(bot)->next(timeout_ms, event) != 0)
            return BMBOT_TIMEOUT;
        *event_json = copyOf(event);
        return *event_json ? BMBOT_OK : BMBOT_E_INTERNAL;
    });
}

int bmbot_done(bmbot *bot, const char *event_id, char **error) {
    return guarded(error, [&]() -> int {
        requireText(event_id, "event_id");
        botOf(bot)->done(event_id);
        return BMBOT_OK;
    });
}

int bmbot_send_chat(bmbot *bot, const char *from, const char *to, const char *text, char **id,
                    char **error) {
    return guarded(error, [&]() -> int {
        if (id)
            *id = nullptr;
        requireText(to, "to");
        requireText(text, "text");
        const auto made = botOf(bot)->sendChat(textOrEmpty(from), to, text);
        if (id)
            *id = copyOf(made);
        return BMBOT_OK;
    });
}

int bmbot_send_letter(bmbot *bot, const char *from, const char *to, const char *subject,
                      const char *body, char **id, char **error) {
    return guarded(error, [&]() -> int {
        if (id)
            *id = nullptr;
        requireText(to, "to");
        requireText(body, "body");
        const auto made = botOf(bot)->sendLetter(textOrEmpty(from), to, textOrEmpty(subject), body);
        if (id)
            *id = copyOf(made);
        return BMBOT_OK;
    });
}

int bmbot_state(bmbot *bot, const char *id, char **state, char **error) {
    return guarded(error, [&]() -> int {
        if (!state)
            throw bmc::Error(bmc::Code::Argument, "state is required");
        *state = nullptr;
        requireText(id, "id");
        *state = copyOf(botOf(bot)->state(id));
        return *state ? BMBOT_OK : BMBOT_E_INTERNAL;
    });
}

int bmbot_status(bmbot *bot, char **status_json, char **error) {
    return guarded(error, [&]() -> int {
        if (!status_json)
            throw bmc::Error(bmc::Code::Argument, "status_json is required");
        *status_json = nullptr;
        *status_json = copyOf(botOf(bot)->status());
        return *status_json ? BMBOT_OK : BMBOT_E_INTERNAL;
    });
}

int bmbot_chan_address(const char *phrase, char **address, char **error) {
    return guarded(error, [&]() -> int {
        if (!address)
            throw bmc::Error(bmc::Code::Argument, "address is required");
        *address = nullptr;
        requireText(phrase, "phrase");
        // Version 4, as Vault::addChannel makes it; the label is not part of an address.
        *address = copyOf(bmc::Protocol::channel(phrase, "chan", 4).address);
        return *address ? BMBOT_OK : BMBOT_E_INTERNAL;
    });
}

void bmbot_free(void *ptr) {
    std::free(ptr);
}

} // extern "C"
