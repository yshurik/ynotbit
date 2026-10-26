// The app's side of the nickbot protocol (docs/nickbot.md): the syntax of a nickname, the commands
// the app sends, and the reading of an answer. The service is in Go (nickbot/); the two are tested
// against the same files (nickbot/testdata), so that they cannot drift apart.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bmc::nickbot {
constexpr int kMinName = 3, kMaxName = 32; // the name after the sigil
constexpr int kMinTerm = 4;                // a search term, not counting its sigil
constexpr int kBatch = 8;                  // addresses in one /nickname
constexpr int kSearchLimit = 10;           // names in one /search answer
constexpr int kMaxBots = 5;                // nickbots a mailbox lists
// The chan every lookup is sent from, so that the bot cannot tell who asks (docs/nickbot.md).
inline constexpr std::string_view kAnonymousPhrase = "anonymous";
// Its address, spelled out so that the app need not work it out (a chan takes a quarter of a
// second) every time it looks at the vault; a test holds the two together.
inline constexpr std::string_view kAnonymousAddress = "BM-2cW4RJRRtqNDx32Qq5P7VvtvpRmE35hy9B";
// How long a lookup lives on the network: far less work than a chat's day, and it cannot be
// replayed from the network later.
constexpr int64_t kLookupTtlSeconds = 3600;

enum class Sigil : char { Person = '@', Bot = '/', Chan = '#' };

struct Nick {
    Sigil sigil = Sigil::Person;
    std::string name;         // as it was written
    std::string text() const; // "@Ben"
    // What makes two nicknames the same: a person and a bot ignore case, a chan is its passphrase.
    std::string key() const;
};

// Exactly one nickname; nothing is trimmed.
std::optional<Nick> parseNick(std::string_view text);

struct Term {
    char sigil = 0;   // 0: any
    std::string text; // lower case
};
enum class TermResult { Ok, Syntax, TooShort };
TermResult parseTerm(std::string_view text, Term *out);

// An answer, as the trailer of a chat says it.
struct AddressNicks {
    std::string address;
    std::vector<std::string> nicks;
};
struct Hit {
    std::string nick, address;
};
struct Reply {
    std::string cmd; // register, unregister, address, nickname, search, whoami, help; "" if the bot
                     // understood none
    bool ok = false;
    std::string error; // a code such as "taken", when not ok
    std::string query; // what was asked, when it was valid
    std::optional<bool> found;
    std::string nick, address;
    bool already = false;
    std::string replaced;
    std::vector<std::string> nicks;      // whoami
    std::vector<AddressNicks> byAddress; // nickname
    std::vector<Hit> hits;               // search
    bool more = false;
};
// The last line of a chat, "nickbot: {...}". nullopt when there is none, when it is not version 1,
// and when anything in it that the app would show or keep is not what it should be (a nickname that
// is not one, an address that is not one, a list too long): such an answer is not believed at all.
std::optional<Reply> parseTrailer(std::string_view text);

// The commands the app sends.
std::string registerRequest(const Nick &);
std::string unregisterRequest(const Nick &);
std::string addressRequest(const Nick &);
std::string
nicknameRequest(const std::vector<std::string> &addresses); // 1 to kBatch valid addresses
std::string searchRequest(const Term &);
std::string whoamiRequest();
// The addresses split into requests of kBatch, each address once, in the order first met.
std::vector<std::vector<std::string>> nicknameBatches(const std::vector<std::string> &addresses);

// The key a lookup and its answer share, so that an answer finds its question.
std::string keyOfAddressRequest(const Nick &);
std::string keyOfNicknameRequest(std::vector<std::string> addresses);
std::string keyOfSearchRequest(const Term &);
// The key of an answer to a lookup; nullopt for anything else (an error, a register).
std::optional<std::string> keyOfReply(const Reply &);
} // namespace bmc::nickbot
