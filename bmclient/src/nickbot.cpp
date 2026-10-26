#include <algorithm>
#include <bmc/chat_format.h>
#include <bmc/error.h>
#include <bmc/json.h>
#include <bmc/nickbot.h>
#include <bmc/wire.h>
#include <set>
#include <utility>

namespace bmc::nickbot {
namespace {
bool isLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool isNameChar(char c) {
    return isLetter(c) || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
bool isSigil(char c) {
    return c == '@' || c == '/' || c == '#';
}
std::string lower(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}
[[noreturn]] void wrong(const char *what) {
    throw Error(Code::Argument, what); // not require(): a bot meets this, the catalogue skips it
}
} // namespace

std::string Nick::text() const {
    return std::string(1, char(sigil)) + name;
}

std::string Nick::key() const {
    return std::string(1, char(sigil)) + (sigil == Sigil::Chan ? name : lower(name));
}

std::optional<Nick> parseNick(std::string_view text) {
    if (text.size() < size_t(1 + kMinName) || text.size() > size_t(1 + kMaxName) ||
        !isSigil(text[0]))
        return {};
    const auto name = text.substr(1);
    if (!isLetter(name[0]))
        return {};
    for (const char c : name.substr(1))
        if (!isNameChar(c))
            return {};
    return Nick{Sigil(text[0]), std::string(name)};
}

TermResult parseTerm(std::string_view text, Term *out) {
    char sigil = 0;
    if (!text.empty() && isSigil(text[0])) {
        sigil = text[0];
        text.remove_prefix(1);
    }
    if (text.size() > size_t(kMaxName))
        return TermResult::Syntax;
    for (const char c : text)
        if (!isNameChar(c))
            return TermResult::Syntax;
    if (text.size() < size_t(kMinTerm))
        return TermResult::TooShort;
    if (out)
        *out = {sigil, lower(std::string(text))};
    return TermResult::Ok;
}

// --- the commands -----------------------------------------------------------------------------

std::string registerRequest(const Nick &n) {
    return "/register " + n.text();
}
std::string unregisterRequest(const Nick &n) {
    return "/unregister " + n.text();
}
std::string addressRequest(const Nick &n) {
    return "/address " + n.text();
}
std::string nicknameRequest(const std::vector<std::string> &addresses) {
    if (addresses.empty() || addresses.size() > size_t(kBatch))
        wrong("A nickname lookup asks for 1 to 8 addresses");
    std::string out = "/nickname";
    for (const auto &a : addresses) {
        if (!Wire::validAddress(a))
            wrong("A nickname lookup needs valid addresses");
        out += " " + a;
    }
    return out;
}
std::string searchRequest(const Term &t) {
    return "/search " + (t.sigil ? std::string(1, t.sigil) : std::string()) + t.text;
}
std::string whoamiRequest() {
    return "/whoami";
}

std::vector<std::vector<std::string>> nicknameBatches(const std::vector<std::string> &addresses) {
    std::vector<std::vector<std::string>> batches;
    std::set<std::string> seen;
    for (const auto &a : addresses) {
        if (!seen.insert(a).second)
            continue;
        if (batches.empty() || batches.back().size() == size_t(kBatch))
            batches.emplace_back();
        batches.back().push_back(a);
    }
    return batches;
}

// --- keys -------------------------------------------------------------------------------------

std::string keyOfAddressRequest(const Nick &n) {
    return "address:" + n.key();
}
std::string keyOfNicknameRequest(std::vector<std::string> addresses) {
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    std::string out = "nickname:";
    for (size_t i = 0; i < addresses.size(); ++i)
        out += (i ? "," : "") + addresses[i];
    return out;
}
std::string keyOfSearchRequest(const Term &t) {
    return "search:" + (t.sigil ? std::string(1, t.sigil) : std::string()) + t.text;
}
std::optional<std::string> keyOfReply(const Reply &r) {
    if (!r.ok)
        return {};
    if (r.cmd == "address") {
        const auto n = parseNick(r.query);
        if (!n)
            return {};
        return keyOfAddressRequest(*n);
    }
    if (r.cmd == "nickname") {
        std::vector<std::string> addresses;
        for (const auto &entry : r.byAddress)
            addresses.push_back(entry.address);
        return keyOfNicknameRequest(addresses);
    }
    if (r.cmd == "search") {
        Term t;
        if (parseTerm(r.query, &t) != TermResult::Ok)
            return {};
        return keyOfSearchRequest(t);
    }
    return {};
}

// --- the answer -------------------------------------------------------------------------------

namespace {
using json::Value;

bool knownCommand(const std::string &cmd) {
    static const std::set<std::string> known{"register", "unregister", "address", "nickname",
                                             "search",   "whoami",     "help"};
    return cmd.empty() || known.count(cmd) > 0;
}
bool plain(const std::string &s, size_t most) {
    if (s.size() > most)
        return false;
    for (const char c : s)
        if (c < 0x20 || c > 0x7e)
            return false;
    return true;
}
bool errorCode(const std::string &s) {
    if (s.size() > 24)
        return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || c == '_'))
            return false;
    return true;
}
bool validNick(const Value &v, std::string *out) {
    if (!v.isString() || !parseNick(v.toString()))
        return false;
    *out = v.toString();
    return true;
}
bool validAddress(const Value &v, std::string *out) {
    if (!v.isString() || !Wire::validAddress(v.toString()))
        return false;
    *out = v.toString();
    return true;
}
bool nickList(const Value &v, std::vector<std::string> *out, size_t most) {
    if (!v.isArray() || v.items().size() > most)
        return false;
    for (const auto &item : v.items()) {
        std::string nick;
        if (!validNick(item, &nick))
            return false;
        out->push_back(nick);
    }
    return true;
}
} // namespace

std::optional<Reply> parseTrailer(std::string_view text) {
    if (text.size() > size_t(kMaxChatBodyBytes))
        return {};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '\t'))
        text.remove_suffix(1);
    const auto newline = text.rfind('\n');
    const auto last = newline == std::string_view::npos ? text : text.substr(newline + 1);
    constexpr std::string_view marker = "nickbot: ";
    if (last.substr(0, marker.size()) != marker)
        return {};
    const auto doc = json::parse(last.substr(marker.size()));
    if (!doc || !doc->isObject() || !doc->at("v").equalsNumber(1))
        return {};
    Reply r;
    const auto &cmd = doc->at("cmd");
    if (!cmd.isString() || !knownCommand(cmd.toString()) || !doc->at("ok").isBool())
        return {};
    r.cmd = cmd.toString();
    r.ok = doc->at("ok").toBool();
    if (const auto &e = doc->at("error"); !e.isUndefined()) {
        if (!e.isString() || !errorCode(e.toString()))
            return {};
        r.error = e.toString();
    }
    if (const auto &q = doc->at("query"); !q.isUndefined()) {
        if (!q.isString() || !plain(q.toString(), 64))
            return {};
        r.query = q.toString();
    }
    if (const auto &f = doc->at("found"); !f.isUndefined()) {
        if (!f.isBool())
            return {};
        r.found = f.toBool();
    }
    if (const auto &n = doc->at("nick"); !n.isUndefined() && !validNick(n, &r.nick))
        return {};
    if (const auto &a = doc->at("address"); !a.isUndefined() && !validAddress(a, &r.address))
        return {};
    for (const auto &[key, flag] : {std::pair<const char *, bool *>{"already", &r.already},
                                    std::pair<const char *, bool *>{"more", &r.more}}) {
        const auto &v = doc->at(key);
        if (v.isUndefined())
            continue;
        if (!v.isBool())
            return {};
        *flag = v.toBool();
    }
    if (const auto &p = doc->at("replaced"); !p.isUndefined() && !validNick(p, &r.replaced))
        return {};
    if (const auto &n = doc->at("nicks");
        !n.isUndefined() && !nickList(n, &r.nicks, size_t(kBatch)))
        return {};
    if (const auto &results = doc->at("results"); !results.isUndefined()) {
        if (!results.isArray())
            return {};
        if (r.cmd == "nickname") {
            if (results.items().size() > size_t(kBatch))
                return {};
            for (const auto &item : results.items()) {
                AddressNicks entry;
                if (!item.isObject() || !validAddress(item.at("address"), &entry.address) ||
                    !nickList(item.at("nicks"), &entry.nicks, size_t(kBatch)))
                    return {};
                r.byAddress.push_back(std::move(entry));
            }
        } else if (r.cmd == "search") {
            if (results.items().size() > size_t(kSearchLimit))
                return {};
            for (const auto &item : results.items()) {
                Hit hit;
                if (!item.isObject() || !validNick(item.at("nick"), &hit.nick) ||
                    !validAddress(item.at("address"), &hit.address))
                    return {};
                r.hits.push_back(std::move(hit));
            }
        } else {
            return {};
        }
    }
    return r;
}
} // namespace bmc::nickbot
