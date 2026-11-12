#include <bmc/bot_trailer.h>
#include <bmc/identity.h>
#include <bmc/json.h>

namespace bmc {
namespace {
bool white(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
bool idChar(char c, bool first) {
    return (c >= 'a' && c <= 'z') || (!first && ((c >= '0' && c <= '9') || c == '_'));
}
} // namespace

std::optional<BotTrailer> splitBotTrailer(std::string_view text) {
    if (text.size() > size_t(kMaxLetterText))
        return {};
    while (!text.empty() && white(text.back()))
        text.remove_suffix(1);
    const auto newline = text.rfind('\n');
    const auto last = newline == std::string_view::npos ? text : text.substr(newline + 1);
    size_t idLength = 0;
    while (idLength < last.size() && idChar(last[idLength], idLength == 0))
        ++idLength;
    if (idLength < 2 || idLength > 32 || last.substr(idLength, 2) != ": ")
        return {};
    const auto jsonText = last.substr(idLength + 2);
    if (jsonText.size() < 2 || jsonText.front() != '{' || jsonText.back() != '}')
        return {};
    const auto doc = json::parse(jsonText);
    if (!doc || !doc->isObject() || !doc->at("v").isNumber() || doc->at("v").toInteger(0) < 1)
        return {};
    BotTrailer trailer;
    trailer.id = std::string(last.substr(0, idLength));
    trailer.json = std::string(jsonText);
    if (newline != std::string_view::npos) {
        auto words = text.substr(0, newline);
        while (!words.empty() && white(words.back()))
            words.remove_suffix(1);
        trailer.words = std::string(words);
    }
    return trailer;
}

std::string indentJson(std::string_view s) {
    if (!json::parse(s))
        return std::string(s);
    std::string out;
    int depth = 0;
    bool inString = false, escaped = false;
    const auto lineBreak = [&] {
        out += '\n';
        out.append(size_t(depth) * 2, ' ');
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inString) {
            out += c;
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        switch (c) {
        case '"':
            inString = true;
            out += c;
            break;
        case '{':
        case '[': {
            size_t next = i + 1;
            while (next < s.size() && white(s[next]))
                ++next;
            if (next < s.size() && (s[next] == '}' || s[next] == ']')) { // empty: stays as it is
                out += c;
                out += s[next];
                i = next;
                break;
            }
            out += c;
            ++depth;
            lineBreak();
            break;
        }
        case '}':
        case ']':
            --depth;
            lineBreak();
            out += c;
            break;
        case ',':
            out += c;
            lineBreak();
            break;
        case ':':
            out += ": ";
            break;
        default:
            if (!white(c))
                out += c;
        }
    }
    return out;
}
} // namespace bmc
