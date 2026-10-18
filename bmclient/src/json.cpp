#include <algorithm>
#include <bmc/json.h>
#include <bmc/text.h>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <map>

namespace bmc::json {
Value Value::makeNull() {
    Value v;
    v.type_ = Type::Null;
    return v;
}
Value Value::makeBool(bool value) {
    Value v;
    v.type_ = Type::Bool;
    v.bool_ = value;
    return v;
}
Value Value::makeInteger(int64_t value) {
    Value v;
    v.type_ = Type::Number;
    v.whole_ = true;
    v.integer_ = value;
    return v;
}
Value Value::makeReal(double value) {
    Value v;
    v.type_ = Type::Number;
    v.real_ = value;
    return v;
}
Value Value::makeString(std::string value) {
    Value v;
    v.type_ = Type::String;
    v.text_ = std::move(value);
    return v;
}
Value Value::makeArray(std::vector<Value> items) {
    Value v;
    v.type_ = Type::Array;
    v.items_ = std::move(items);
    return v;
}
Value Value::makeObject(std::vector<std::string> sortedKeys, std::vector<Value> values) {
    Value v;
    v.type_ = Type::Object;
    v.keys_ = std::move(sortedKeys);
    v.items_ = std::move(values);
    return v;
}

const Value &Value::at(std::string_view key) const {
    static const Value undefined;
    if (type_ != Type::Object)
        return undefined;
    const auto it = std::lower_bound(
        keys_.begin(), keys_.end(), key,
        [](const std::string &k, std::string_view wanted) { return std::string_view(k) < wanted; });
    if (it == keys_.end() || *it != key)
        return undefined;
    return items_[size_t(it - keys_.begin())];
}

double Value::toDouble(double fallback) const {
    if (type_ != Type::Number)
        return fallback;
    return whole_ ? double(integer_) : real_;
}

int64_t Value::toInteger(int64_t fallback) const {
    if (type_ != Type::Number)
        return fallback;
    if (whole_)
        return integer_;
    // A whole double inside the range of int64_t: [-2^63, 2^63).
    if (std::isfinite(real_) && std::floor(real_) == real_ && real_ >= -9223372036854775808.0 &&
        real_ < 9223372036854775808.0)
        return int64_t(real_);
    return fallback;
}

namespace {
constexpr int kMaxDepth = 1024; // object and array levels; Qt's limit (checked by the parity test)

struct Bad {}; // thrown inside the parser, caught by parse()
[[noreturn]] void bad() {
    throw Bad{};
}

void appendUtf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xc0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xe0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(char(0xf0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    }
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// The grammar of what the number scan took: -? (digits+ (. digits*)? | . digits+) ([eE] [+-]?
// digits+)?
bool numberToken(const std::string &t) {
    size_t i = 0;
    if (i < t.size() && t[i] == '-')
        ++i;
    size_t digits = 0;
    while (i < t.size() && isDigit(t[i])) {
        ++i;
        ++digits;
    }
    if (i < t.size() && t[i] == '.') {
        ++i;
        size_t fraction = 0;
        while (i < t.size() && isDigit(t[i])) {
            ++i;
            ++fraction;
        }
        if (digits == 0 && fraction == 0)
            return false;
    } else if (digits == 0) {
        return false;
    }
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i < t.size() && (t[i] == '+' || t[i] == '-'))
            ++i;
        size_t exponent = 0;
        while (i < t.size() && isDigit(t[i])) {
            ++i;
            ++exponent;
        }
        if (exponent == 0)
            return false;
    }
    return i == t.size();
}

class Parser {
  public:
    explicit Parser(std::string_view text) : in_(text) {}

    Value document() {
        if (in_.size() >= 3 && in_.compare(0, 3, "\xEF\xBB\xBF") == 0)
            pos_ = 3; // a UTF-8 byte order mark
        skipSpace();
        if (!at('{') && !at('['))
            bad();
        Value v = value(1);
        skipSpace();
        if (pos_ != in_.size())
            bad();
        return v;
    }

  private:
    std::string_view in_;
    size_t pos_ = 0;

    bool at(char c) const {
        return pos_ < in_.size() && in_[pos_] == c;
    }
    void skipSpace() {
        while (pos_ < in_.size() &&
               (in_[pos_] == ' ' || in_[pos_] == '\t' || in_[pos_] == '\n' || in_[pos_] == '\r'))
            ++pos_;
    }
    void literal(std::string_view word) {
        if (in_.substr(pos_, word.size()) != word)
            bad();
        pos_ += word.size();
    }

    Value value(int depth) {
        if (pos_ >= in_.size())
            bad();
        switch (in_[pos_]) {
        case '{':
            return object(depth);
        case '[':
            return array(depth);
        case '"':
            return Value::makeString(string());
        case 't':
            literal("true");
            return Value::makeBool(true);
        case 'f':
            literal("false");
            return Value::makeBool(false);
        case 'n':
            literal("null");
            return Value::makeNull();
        default:
            return number();
        }
    }

    Value object(int depth) {
        if (depth > kMaxDepth)
            bad();
        ++pos_; // {
        std::map<std::string, Value> members;
        skipSpace();
        if (at('}')) {
            ++pos_;
        } else {
            for (;;) {
                skipSpace();
                if (!at('"'))
                    bad();
                std::string key = string();
                skipSpace();
                if (!at(':'))
                    bad();
                ++pos_;
                skipSpace();
                members[std::move(key)] =
                    value(depth + 1); // a later duplicate replaces an earlier one
                skipSpace();
                if (at(',')) {
                    ++pos_;
                    continue;
                }
                if (at('}')) {
                    ++pos_;
                    break;
                }
                bad();
            }
        }
        std::vector<std::string> keys;
        std::vector<Value> values;
        for (auto &[key, member] : members) {
            keys.push_back(key);
            values.push_back(std::move(member));
        }
        return Value::makeObject(std::move(keys), std::move(values));
    }

    Value array(int depth) {
        if (depth > kMaxDepth)
            bad();
        ++pos_; // [
        std::vector<Value> items;
        skipSpace();
        if (at(']')) {
            ++pos_;
            return Value::makeArray(std::move(items));
        }
        for (;;) {
            skipSpace();
            items.push_back(value(depth + 1));
            skipSpace();
            if (at(',')) {
                ++pos_;
                continue;
            }
            if (at(']')) {
                ++pos_;
                break;
            }
            bad();
        }
        return Value::makeArray(std::move(items));
    }

    std::string string() {
        ++pos_; // the opening quote
        std::string out;
        for (;;) {
            if (pos_ >= in_.size())
                bad();
            const auto c = uint8_t(in_[pos_]);
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c == '\\') {
                escape(out);
            } else if (c < 0x80) {
                out.push_back(char(c));
                ++pos_;
            } else {
                const size_t n = text::sequenceLength(in_.substr(pos_));
                if (n == 0)
                    bad();
                out.append(in_.substr(pos_, n));
                pos_ += n;
            }
        }
    }

    uint32_t hex4() {
        if (pos_ + 4 > in_.size())
            bad();
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = in_[pos_++];
            v <<= 4;
            if (h >= '0' && h <= '9')
                v |= uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f')
                v |= uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                v |= uint32_t(h - 'A' + 10);
            else
                bad();
        }
        return v;
    }

    void escape(std::string &out) {
        ++pos_; // the backslash
        if (pos_ >= in_.size())
            bad();
        const char c = in_[pos_++];
        switch (c) {
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u': {
            uint32_t u = hex4();
            if (u >= 0xd800 && u <= 0xdbff && pos_ + 1 < in_.size() && in_[pos_] == '\\' &&
                in_[pos_ + 1] == 'u') {
                const size_t before = pos_;
                pos_ += 2;
                const uint32_t low = hex4();
                if (low >= 0xdc00 && low <= 0xdfff) {
                    appendUtf8(out, 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00));
                    break;
                }
                pos_ = before; // not a pair: the next escape is read on its own
            }
            if (u >= 0xd800 && u <= 0xdfff)
                out.push_back('?'); // a surrogate alone: what QString::toUtf8() makes of it
            else
                appendUtf8(out, u);
            break;
        }
        default:
            // \" \\ \/ and, as Qt reads it, any other character stands for itself. A byte of 0x80
            // or more is taken as the Latin-1 character of that number.
            if (uint8_t(c) < 0x80)
                out.push_back(c);
            else
                appendUtf8(out, uint8_t(c));
            break;
        }
    }

    Value number() {
        const size_t start = pos_;
        bool whole = true;
        if (at('-'))
            ++pos_;
        if (at('0'))
            ++pos_;
        else
            while (pos_ < in_.size() && isDigit(in_[pos_]))
                ++pos_;
        if (at('.')) {
            whole = false;
            ++pos_;
            while (pos_ < in_.size() && isDigit(in_[pos_]))
                ++pos_;
        }
        if (at('e') || at('E')) {
            whole = false;
            ++pos_;
            if (at('-') || at('+'))
                ++pos_;
            while (pos_ < in_.size() && isDigit(in_[pos_]))
                ++pos_;
        }
        if (pos_ >= in_.size()) // Qt: a number must be followed by something
            bad();
        const std::string token(in_.substr(start, pos_ - start));
        if (!numberToken(token))
            bad();
        if (whole) {
            int64_t integer = 0;
            const auto [end, error] =
                std::from_chars(token.data(), token.data() + token.size(), integer);
            if (error == std::errc() && end == token.data() + token.size())
                return Value::makeInteger(integer);
        }
        // Not a whole number: strtod, written with the decimal point of the current locale.
        std::string text = token;
        const char point = *std::localeconv()->decimal_point;
        if (point != '.')
            std::replace(text.begin(), text.end(), '.', point);
        char *end = nullptr;
        const double d = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() || std::isinf(d) || std::isnan(d))
            bad(); // overflow (1e999)
        if (d == 0) {
            // Underflow to zero is an error for Qt; a zero written with a zero mantissa is not.
            for (const char c : token) {
                if (c == 'e' || c == 'E')
                    break;
                if (c >= '1' && c <= '9')
                    bad();
            }
        }
        return Value::makeReal(d);
    }
};
} // namespace

std::optional<Value> parse(std::string_view text) {
    try {
        return Parser(text).document();
    } catch (const Bad &) {
        return std::nullopt;
    }
}

std::string quote(std::string_view s) {
    static const char digits[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = uint8_t(ch);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(digits[c >> 4]);
                out.push_back(digits[c & 15]);
            } else {
                out.push_back(ch);
            }
        }
    }
    return out + "\"";
}
} // namespace bmc::json
