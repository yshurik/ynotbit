# bmbot

A Bitmessage bot library with a C ABI, for programs in any language. It opens a vault and a mailbox from a phrase, runs the node
(`bmnode`) as a child process that it watches and starts again, and hands out the chats and letters that come as JSON events. No Qt.

```
cmake -S . -B build-core -DYNOTBIT_QT=OFF && cmake --build build-core --target bmbot bmnode chat_responder
cmake --install build-core --prefix /usr/local     # libbmbot, bmbot.h, bmbot.pc, chat_responder, bmnode
```

`bmbot.h` is the whole interface: ten functions, nothing but `bmbot *`, `int`, `const char *` and `char **` across it, no callback, no
thread-local state. A handle may be used from any thread; calls on one handle are serialised inside. Every function that can fail
returns `0` ok, `1` timeout (`bmbot_next` only) or a negative code (`BMBOT_E_ARGUMENT`, `STATE`, `AUTH`, `IO`, `NETWORK`,
`INTERNAL`), and takes an optional `char **error` with English text. Every string it returns is freed with `bmbot_free`.

```c
bmbot *bot; char *error;
bmbot_open("{\"data_dir\":\"/var/lib/echo\",\"phrase_file\":\"/etc/echo.phrase\",\"node\":{\"public\":true}}", &bot, &error);
for (;;) {
    char *event;
    if (bmbot_next(bot, 1000, &event, &error) == BMBOT_OK) {   /* {"id":..,"kind":"chat","from":..,"to":..,"text":..} */
        /* answer it with bmbot_send_chat, then */
        bmbot_done(bot, id, &error);                            /* read only now: a crash before this answers it again */
        bmbot_free(event);
    }
}
```

- **Config** (JSON): `data_dir`; `phrase_file` (the secret phrase; it also makes the vault's password) or `phrase` (public); `label`;
  `hang_seconds` (abort when the driver loop stalls, so that Docker or systemd starts it again); `node`: `bin` (`"auto"`, a path, or
  `""` for no node here), `public` (default true; false links to `peer` alone), `peer`, `proxy`, `listen`, `port`, `max_outgoing`,
  `max_incoming`, `watchdog_minutes`, `retention_mb`, `retention_days`. The full list is in `bmbot.h`.
- **Events**: `chat` and `letter` (`id`, `to`, `from`, `subject`, `text`, `seqno`, `received`), and `note` (`text`, `time`: a problem or
  a watchdog restart; it has no `id`, is not kept and needs no `bmbot_done`). The inbox is the mailbox's unread chats and letters,
  oldest first. It is durable and **at least once**: an event is handed out once per run and read only by `bmbot_done`; what was not
  done comes back after a crash or a restart.
- **States** (`bmbot_state`) are the strings ynotbit shows for a message's delivery: `queued`, `awaiting_pubkey`,
  `calculating_message`, `awaiting_ack`, `acknowledged`, `failed`, ...
- **One process to a data directory** (`bmbot_open` fails with `BMBOT_E_STATE` for the second), and the phrase has to be the one the
  data was made with (`BMBOT_E_AUTH`).
- Limits on how much a bot answers are the bot's: `tools/chat_bot.h` is one (rates per peer and in all, a bounded queue, retries; a letter is answered with a letter, once a day to a sender), on
  top of this ABI.

Examples: `bmbot/examples/echo.py` (a complete echo bot on ctypes, the library of `tests/bmbot/ctypes_test.py`) and
`tools/chat_responder.cpp` (C++, the reference). Go: `import "C"` with `#cgo LDFLAGS: -lbmbot` and `#include <bmbot.h>` is all it takes;
the ABI has nothing a cgo caller has to wrap.

Tests: `tests/bmbot/` (`bmbot_tests` for the runtime, the ABI and the chat bot; `responder_e2e_test.py` for two responders over two real
nodes; `ctypes_test.py` for Python). POSIX only for now (the node supervisor and the data directory lock).
