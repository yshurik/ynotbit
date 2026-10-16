#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <bmc/bytes.h>
#include <bmc/json.h>
#include <bmc/test.h>
#include <cstdio>
#include <map>
#include <random>

namespace {
std::string hexOf(const std::string &s) {
    return bmc::hex(bmc::view(s));
}
std::string real(double d) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", d);
    return buf;
}
// One text for a whole tree, the same from Qt's value and from ours.
std::string canon(const QJsonValue &v) {
    switch (v.type()) {
    case QJsonValue::Null:
        return "null";
    case QJsonValue::Bool:
        return v.toBool() ? "true" : "false";
    case QJsonValue::Double:
        return "n(" + std::to_string(v.toInteger()) + "," + real(v.toDouble()) + ")";
    case QJsonValue::String:
        return "s(" + hexOf(v.toString().toUtf8().toStdString()) + ")";
    case QJsonValue::Array: {
        std::string out = "[";
        for (const auto &item : v.toArray())
            out += canon(item) + ",";
        return out + "]";
    }
    case QJsonValue::Object: {
        std::map<std::string, std::string> members;
        const auto object = v.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            members[hexOf(it.key().toUtf8().toStdString())] = canon(it.value());
        std::string out = "{";
        for (const auto &[key, value] : members)
            out += key + ":" + value + ",";
        return out + "}";
    }
    case QJsonValue::Undefined:
        break;
    }
    return "undefined";
}
std::string canon(const bmc::json::Value &v) {
    using T = bmc::json::Value::Type;
    switch (v.type()) {
    case T::Null:
        return "null";
    case T::Bool:
        return v.toBool() ? "true" : "false";
    case T::Number:
        return "n(" + std::to_string(v.toInteger()) + "," + real(v.toDouble()) + ")";
    case T::String:
        return "s(" + hexOf(v.toString()) + ")";
    case T::Array: {
        std::string out = "[";
        for (const auto &item : v.items())
            out += canon(item) + ",";
        return out + "]";
    }
    case T::Object: {
        std::map<std::string, std::string> members;
        for (size_t i = 0; i < v.keys().size(); ++i)
            members[hexOf(v.keys()[i])] = canon(v.values()[i]);
        std::string out = "{";
        for (const auto &[key, value] : members)
            out += key + ":" + value + ",";
        return out + "}";
    }
    case T::Undefined:
        break;
    }
    return "undefined";
}
// "bad" when Qt does not accept the text as a document (an object or an array), else the tree.
std::string qtAnswer(const std::string &text) {
    QJsonParseError error;
    const auto doc =
        QJsonDocument::fromJson(QByteArray(text.data(), qsizetype(text.size())), &error);
    if (error.error != QJsonParseError::NoError || !(doc.isObject() || doc.isArray()))
        return "bad";
    return doc.isObject() ? canon(QJsonValue(doc.object())) : canon(QJsonValue(doc.array()));
}
std::string ourAnswer(const std::string &text) {
    const auto doc = bmc::json::parse(text);
    return doc ? canon(*doc) : "bad";
}
void same(const std::string &text) {
    const auto qt = qtAnswer(text), ours = ourAnswer(text);
    if (qt != ours)
        bmc::test::fail(__FILE__, __LINE__,
                        "json differs for " + hexOf(text) + "\n  Qt:   " + qt +
                            "\n  core: " + ours);
}
} // namespace

TEST(json_parse_matches_qt_on_a_corpus) {
    for (const char *text : {"{\"a\":1,\"a\":2}",
                             "{\"a\":1.0}",
                             "{\"a\":1e0}",
                             "{\"a\":\"1\"}",
                             "{\"a\":2.5}",
                             "{\"a\":3.0}",
                             "{\"a\":1e2}",
                             "{\"a\":9223372036854775807}",
                             "{\"a\":9223372036854775808}",
                             "{\"a\":-4}",
                             "[1]",
                             "1",
                             "  {\"a\":null}  ",
                             "{\"a\":1} x",
                             "",
                             "{\"a\":\"\\ud83d\\ude00\"}",
                             "{\"a\":\"\\ud83d\"}",
                             "{\"a\":\"\\ude00\"}",
                             "{\"a\":\"\\ud83d\\u0041\"}",
                             "{\"a\":\"\\u0000\"}",
                             "{\"a\":01}",
                             "{'a':1}",
                             "{\"a\":\"x\ny\"}",
                             "{\"a\":\"x\ty\"}",
                             "{\"a\":\"\\/\\b\\f\\n\\r\\t\\\"\\\\\"}",
                             "{\"a\":\"\\x\"}",
                             "{\"a\":\"\\u12\"}",
                             "{\"a\":.5}",
                             "{\"a\":5.}",
                             "{\"a\":+1}",
                             "{\"a\":1e}",
                             "{\"a\":0x1}",
                             "{\"a\":-}",
                             "{\"a\":-0}",
                             "{\"a\":1E+2}",
                             "{\"a\":0.1}",
                             "{\"a\":1e999}",
                             "{\"a\":-1e999}",
                             "{\"a\":123456789012345678901234567890}",
                             "{\"a\":-9223372036854775808}",
                             "{\"a\":1.5e3}",
                             "{\"a\":Infinity}",
                             "{\"a\":NaN}",
                             "{\"a\":True}",
                             "{\"a\":nul}",
                             "{\"a\":true}",
                             "{\"a\":false}",
                             "{\"\":1}",
                             "{\"a\":1,}",
                             "{\"a\" 1}",
                             "{\"a\":1}\n",
                             "{\"a\":1}\f",
                             "\f{\"a\":1}",
                             "\v{\"a\":1}",
                             "\t\r\n {\"a\":1}",
                             "{\"a\":{}}",
                             "{\"a\":[]}",
                             "{\"a\":[1,2,]}",
                             "{a:1}",
                             "{\"a\":1}{\"a\":2}",
                             "{\"a\":-.5}",
                             "{\"a\":5.e1}",
                             "{\"a\":.e1}",
                             "{\"a\":-.}",
                             "{\"a\":0.}",
                             "{\"a\":00}",
                             "{\"a\":1e-999}",
                             "{\"a\":-1e-999}",
                             "{\"a\":1e-400}",
                             "{\"a\":4.9e-324}",
                             "{\"a\":2.4e-324}",
                             "{\"a\":1e-320}",
                             "{\"a\":0e999}",
                             "{\"a\":0.0e-999}",
                             "{\"a\":1e308}",
                             "{\"a\":1.8e308}",
                             "{\"a\":1e400}",
                             "\xef\xbb\xbf{\"a\":1}",
                             "{\"a\":\"\xc3\x28\"}",
                             "{\"a\":\"\\\xc3\xa9\"}",
                             "{\"a\":\"\\\xc3\"}",
                             "{\"\xc3\xa9\":1,\"z\":2,\"\xf0\x9f\x98\x80\":3}"})
        same(text);
}

TEST(json_depth_limit_matches_qt) {
    // An object holding d nested arrays: where Qt gives up, we give up.
    for (int d = 1018; d <= 1030; ++d) {
        std::string text = "{\"a\":";
        for (int i = 0; i < d; ++i)
            text += "[";
        text += "1";
        for (int i = 0; i < d; ++i)
            text += "]";
        same(text + "}");
    }
}

TEST(json_parse_matches_qt_on_mutated_documents) {
    std::mt19937_64 rng(0x5eed0004);
    const std::string seeds[] = {
        R"({"app":"ynotbit","body":"hi","format":"markdown","seqno":1,"type":"chat","version":1})",
        R"({"a":[1,2,{"b":"\u00e9\ud83d\ude00"}],"c":-1.5e3,"d":true,"e":null,"f":{}})",
        R"({"app":"ynotbit","body":"line\nbreak \"quoted\" \\ \/","format":"markdown","seqno":9007199254740993,"type":"chat","version":1})",
    };
    const std::string pool =
        "{}[]:,\"\\ \n\t\f\v.-+eE0123456789tfnulrsa/u\xc3\xa9\xf0\x9f\x98\x80\xff\x01";
    for (int round = 0; round < 120000; ++round) {
        std::string text = seeds[rng() % std::size(seeds)];
        const int edits = 1 + int(rng() % 3);
        for (int e = 0; e < edits && !text.empty(); ++e) {
            const size_t at = rng() % text.size();
            switch (rng() % 5) {
            case 0:
                text.erase(at, 1 + rng() % 3);
                break;
            case 1:
                text.insert(at, 1, pool[rng() % pool.size()]);
                break;
            case 2:
                text[at] = pool[rng() % pool.size()];
                break;
            case 3:
                text.resize(at);
                break;
            default:
                text.insert(at, text.substr(rng() % text.size(), 1 + rng() % 6));
                break;
            }
        }
        same(text);
    }
}
