// JSON as the Qt 6.8 parser read it (QJsonDocument::fromJson), leniencies included, so that a
// peer's document is read the same after the move out of Qt. Rules: see
// tests/bmclient/json_test.cpp.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bmc::json {
class Value {
  public:
    enum class Type { Undefined, Null, Bool, Number, String, Array, Object };

    Value() = default; // Undefined: what at() gives for a member that is not there
    static Value makeNull();
    static Value makeBool(bool value);
    static Value makeInteger(int64_t value);
    static Value makeReal(double value);
    static Value makeString(std::string value);
    static Value makeArray(std::vector<Value> items);
    // The keys are sorted (byte order) and distinct.
    static Value makeObject(std::vector<std::string> sortedKeys, std::vector<Value> values);

    Type type() const {
        return type_;
    }
    bool isUndefined() const {
        return type_ == Type::Undefined;
    }
    bool isNull() const {
        return type_ == Type::Null;
    }
    bool isBool() const {
        return type_ == Type::Bool;
    }
    bool isNumber() const {
        return type_ == Type::Number;
    }
    bool isString() const {
        return type_ == Type::String;
    }
    bool isArray() const {
        return type_ == Type::Array;
    }
    bool isObject() const {
        return type_ == Type::Object;
    }

    // The member of an object; Undefined if there is none or this is no object.
    const Value &at(std::string_view key) const;
    const std::vector<Value> &items() const {
        return items_;
    }
    const std::vector<std::string> &keys() const {
        return keys_;
    }
    const std::vector<Value> &values() const {
        return items_;
    }

    // QJsonValue::toString(): the string, or "" for any other type.
    std::string toString() const {
        return type_ == Type::String ? text_ : std::string();
    }
    bool toBool(bool fallback = false) const {
        return type_ == Type::Bool ? bool_ : fallback;
    }
    double toDouble(double fallback = 0) const;
    // QJsonValue::toInteger(): the number if it is a whole number that fits 64 bits, else fallback.
    int64_t toInteger(int64_t fallback = 0) const;
    // QJsonValue == number, and == string.
    bool equalsNumber(double d) const {
        return type_ == Type::Number && toDouble() == d;
    }
    bool equalsString(std::string_view s) const {
        return type_ == Type::String && text_ == s;
    }

  private:
    Type type_ = Type::Undefined;
    bool bool_ = false;
    bool whole_ = false; // a number written without fraction or exponent that fits 64 bits
    int64_t integer_ = 0;
    double real_ = 0;
    std::string text_;
    std::vector<Value> items_;      // an array's items; an object's values, in the order of keys_
    std::vector<std::string> keys_; // an object's keys, sorted
};

// nullopt when Qt would not have accepted the text as a document.
std::optional<Value> parse(std::string_view text);
// A JSON string literal for UTF-8 text.
std::string quote(std::string_view utf8);
} // namespace bmc::json
