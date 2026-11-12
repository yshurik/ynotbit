// The last line of a bot's chat reply, for programs: "<id>: {json}". Any bot may end its reply so
// (docs/btcbot.md); nickbot does too.
#include <bmc/bot_trailer.h>
#include <bmc/identity.h>
#include <bmc/test.h>

using namespace bmc;

TEST(a_trailer_is_split_from_the_words_above_it) {
    const auto t =
        splitBotTrailer("Block height: 975,560\nbtcbot: "
                        "{\"v\":1,\"cmd\":\"getblockcount\",\"ok\":true,\"result\":975560}");
    CHECK(t.has_value());
    CHECK_EQ(t->words, "Block height: 975,560");
    CHECK_EQ(t->id, "btcbot");
    CHECK_EQ(t->json, "{\"v\":1,\"cmd\":\"getblockcount\",\"ok\":true,\"result\":975560}");
}

TEST(nickbots_own_trailer_is_one_too) {
    const auto t = splitBotTrailer(
        "Registered @x for you.\nnickbot: {\"v\":1,\"cmd\":\"register\",\"ok\":true}");
    CHECK(t && t->id == "nickbot" && t->words == "Registered @x for you.");
}

TEST(the_words_keep_their_lines_and_lose_only_the_white_space_that_ends_them) {
    const auto t = splitBotTrailer(
        "one  \ntwo\n\n| a | b |\n|---|---|\n| 1 | 2 |  \r\n \nbtcbot: {\"v\":1}\n\n ");
    CHECK(t.has_value());
    CHECK_EQ(t->words, "one  \ntwo\n\n| a | b |\n|---|---|\n| 1 | 2 |");
    CHECK_EQ(t->json, "{\"v\":1}");
}

TEST(a_message_that_is_only_a_trailer_has_no_words) {
    const auto t = splitBotTrailer("btcbot: {\"v\":1}");
    CHECK(t && t->words.empty() && t->id == "btcbot");
}

TEST(only_the_last_line_counts) {
    CHECK(!splitBotTrailer("btcbot: {\"v\":1}\nand then some words"));
    const auto t = splitBotTrailer("a: {\"v\":1}\nbtcbot: {\"v\":1,\"x\":2}");
    CHECK(t && t->id == "btcbot" && t->words == "a: {\"v\":1}");
    CHECK(!splitBotTrailer("words btcbot: {\"v\":1}")); // not at the start of its line
    CHECK(!splitBotTrailer(" btcbot: {\"v\":1}\n x"));
    CHECK(!splitBotTrailer(""));
    CHECK(!splitBotTrailer("\n\n"));
}

TEST(the_id_is_a_lower_case_word_of_two_to_thirty_two_characters) {
    const auto line = [](const std::string &id) {
        return splitBotTrailer("w\n" + id + ": {\"v\":1}");
    };
    CHECK(line("ab").has_value());
    CHECK(line("bot_2").has_value());
    CHECK(line("zz").has_value());
    CHECK(line("a9z_z").has_value());
    CHECK(line(std::string(32, 'a')).has_value());
    CHECK(!line("a"));
    CHECK(!line(std::string(33, 'a')));
    CHECK(!line("Btcbot"));
    CHECK(!line("2bot"));
    CHECK(!line("_bot"));
    CHECK(!line("bot-x"));
    CHECK(!line("bot x"));
    CHECK(!line(""));
}

TEST(a_colon_and_a_space_and_an_object_follow_the_id) {
    CHECK(!splitBotTrailer("w\nbtcbot:{\"v\":1}"));
    CHECK(!splitBotTrailer("w\nbtcbot:  {\"v\":1}"));
    CHECK(!splitBotTrailer("w\nbtcbot : {\"v\":1}"));
    CHECK(!splitBotTrailer("w\nbtcbot: [1]"));
    CHECK(!splitBotTrailer("w\nbtcbot: {\"v\":1"));
    CHECK(!splitBotTrailer("w\nbtcbot: {\"v\":1} x"));
    CHECK(!splitBotTrailer("w\nbtcbot: {}"));
    CHECK(!splitBotTrailer("w\nbtcbot: "));
    CHECK(!splitBotTrailer("w\nbtcbot: {not json}"));
}

TEST(the_version_is_a_whole_number_of_one_or_more) {
    CHECK(splitBotTrailer("w\nbtcbot: {\"v\":1}").has_value());
    CHECK(splitBotTrailer("w\nbtcbot: {\"v\":2,\"other\":[1,2]}").has_value());
    CHECK(splitBotTrailer("w\nbtcbot: {\"v\":1.0}")
              .has_value()); // a whole number written with a point is one
    CHECK(!splitBotTrailer("w\nbtcbot: {\"v\":0}"));
    CHECK(!splitBotTrailer("w\nbtcbot: {\"v\":-1}"));
    CHECK(!splitBotTrailer("w\nbtcbot: {\"v\":1.5}"));
    CHECK(!splitBotTrailer("w\nbtcbot: {\"v\":\"1\"}"));
    CHECK(!splitBotTrailer("w\nbtcbot: {\"v\":true}"));
    CHECK(!splitBotTrailer("w\nbtcbot: {\"v\":null}"));
    CHECK(!splitBotTrailer("w\nbtcbot: {\"cmd\":\"x\"}"));
}

TEST(a_trailer_with_numbers_and_strings_of_the_real_world_is_kept_as_written) {
    const std::string json =
        "{\"v\":1,\"cmd\":\"getrawtransaction\",\"ok\":true,\"params\":{\"txid\":\"ab\"},"
        "\"result\":{\"fee\":1e-05,"
        "\"value\":0.00012345,\"asm\":\"0 c07458\",\"s\":\"a \\\"quoted\\\" {brace} \\u00e9\"}}";
    const auto t = splitBotTrailer("words\nbtcbot: " + json);
    CHECK(t.has_value());
    CHECK_EQ(t->json, json);
}

TEST(a_text_over_the_size_of_a_letters_text_is_not_read) {
    // A chat is as long as a letter's text, and a text that no object could have carried is no
    // chat.
    const std::string trailer = "\nbtcbot: {\"v\":1}";
    const std::string whole(size_t(kMaxLetterText) - trailer.size(), 'a');
    CHECK(splitBotTrailer(whole + trailer).has_value());
    CHECK(!splitBotTrailer(whole + "a" + trailer)); // one byte more
}

TEST(json_is_written_with_two_spaces_of_indent_and_nothing_else_changed) {
    CHECK_EQ(indentJson("{\"a\":[1,2],\"b\":{},\"c\":[],\"d\":{\"e\":null}}"),
             "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": {},\n  \"c\": [],\n  \"d\": {\n    "
             "\"e\": null\n  }\n}");
    // numbers and strings are what they were
    const std::string in = "{\"fee\":1e-05,\"v\":0.00012345,\"n\":21000000.00000000,\"s\":\"a,b:{c}"
                           "[d]\\\"e\\\\\",\"u\":\"\\u00e9\"}";
    const auto out = indentJson(in);
    CHECK(out.find("1e-05") != std::string::npos);
    CHECK(out.find("0.00012345") != std::string::npos);
    CHECK(out.find("21000000.00000000") != std::string::npos);
    CHECK(out.find("\"a,b:{c}[d]\\\"e\\\\\"") != std::string::npos);
    CHECK(out.find("\"\\u00e9\"") != std::string::npos);
    // taking the white space away again gives the compact text back
    std::string squeezed;
    bool inString = false, escaped = false;
    for (const char c : out) {
        if (inString) {
            squeezed += c;
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        if (c != ' ' && c != '\n')
            squeezed += c;
    }
    CHECK_EQ(squeezed, in);
}

TEST(text_that_is_not_json_is_written_as_it_is) {
    CHECK_EQ(indentJson("not json"), "not json");
    CHECK_EQ(indentJson("{\"a\":"), "{\"a\":");
    CHECK_EQ(indentJson(""), "");
    CHECK_EQ(indentJson("[1, 2]"), "[\n  1,\n  2\n]");
    CHECK_EQ(indentJson("  { \"a\" : 1 }  "), "{\n  \"a\": 1\n}");
}
