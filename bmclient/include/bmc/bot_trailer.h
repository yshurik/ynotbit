// The last line of a bot's chat reply, for programs: "<id>: {json}" (docs/btcbot.md). Any bot may
// end its reply so, and nickbot does ("nickbot: {...}"). A client shows the words above the line,
// and keeps the JSON for whoever wants it (a button that copies it).
#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace bmc {
struct BotTrailer {
    std::string words; // the text above the line, without the white space that ends it
    std::string id;    // "btcbot"
    std::string json;  // the JSON object, as the bot wrote it (one line)
};

// The last line that has anything on it, as a trailer: an id (a lower case letter, then 1 to 31
// lower case letters, digits or underscores), a colon and a space, and a JSON object whose "v" is a
// whole number of 1 or more. nullopt for anything else, and for a text over the size of a chat.
std::optional<BotTrailer> splitBotTrailer(std::string_view text);

// JSON written with a line for each member and two spaces of indent per level. Only the white space
// is changed: every string and number stays as it was written (0.00012345 and 1e-05 are not read
// and written again). Text that is not JSON is returned as it is.
std::string indentJson(std::string_view json);
} // namespace bmc
