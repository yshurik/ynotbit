// The app's side of the nickbot protocol (docs/nickbot.md), against the files the Go service is
// tested with: the table of nicknames and the trailers a scripted conversation gets.
#include <bmc/error.h>
#include <bmc/nickbot.h>
#include <bmc/protocol.h>
#include <bmc/test.h>
#include <fstream>
#include <set>

using namespace bmc::nickbot;

namespace {
const std::string A = "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN",
                  B = "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6",
                  C = "BM-2cUqyPSNiBCvTg51hztE4uWrxET3386Ezo";

// The lines of a file the Go tests share, split at tabs, without comments and empty lines. Nothing
// is trimmed: a space at the end of a line is part of the case.
std::vector<std::vector<std::string>> tabbed(const std::string &name) {
    std::ifstream in(std::string(BMC_NICKBOT_TESTDATA) + "/" + name, std::ios::binary);
    CHECK(in.good());
    std::vector<std::vector<std::string>> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> fields;
        size_t from = 0;
        for (;;) {
            const auto tab = line.find('\t', from);
            fields.push_back(line.substr(from, tab == std::string::npos ? tab : tab - from));
            if (tab == std::string::npos)
                break;
            from = tab + 1;
        }
        rows.push_back(fields);
    }
    return rows;
}
} // namespace

TEST(nicknames_follow_the_table_the_go_service_is_tested_with) {
    int count = 0;
    for (const auto &row : tabbed("nick_cases.txt")) {
        const auto parsed = parseNick(row.at(1));
        if (row.at(0) == "ok") {
            CHECK_EQ(row.size(), size_t(3));
            CHECK(parsed.has_value());
            if (parsed) {
                CHECK_EQ(parsed->key(), row.at(2));
                CHECK_EQ(parsed->text(), row.at(1));
            }
        } else {
            CHECK_EQ(row.at(0), "no");
            CHECK(!parsed.has_value());
        }
        ++count;
    }
    CHECK(count >= 40);
}

TEST(a_nickname_has_a_kind_a_name_and_a_key) {
    const auto person = parseNick("@Ben"), bot = parseNick("/Ben"), chan = parseNick("#Ben"),
               chanLower = parseNick("#ben");
    CHECK(person && bot && chan && chanLower);
    CHECK(person->sigil == Sigil::Person && bot->sigil == Sigil::Bot && chan->sigil == Sigil::Chan);
    CHECK_EQ(person->name, "Ben");
    CHECK(person->key() != bot->key());
    CHECK(chan->key() != chanLower->key()); // two chans
    CHECK_EQ(parseNick("@bEN")->key(), person->key());
    CHECK_EQ(kMinName, 3);
    CHECK_EQ(kMaxName, 32);
}

TEST(search_terms_have_a_least_length_and_a_narrow_alphabet) {
    struct Case {
        const char *in;
        TermResult result;
        char sigil;
        const char *text;
    };
    const Case cases[] = {
        {"bena", TermResult::Ok, 0, "bena"},        {"BENA", TermResult::Ok, 0, "bena"},
        {"@bena", TermResult::Ok, '@', "bena"},     {"/bena", TermResult::Ok, '/', "bena"},
        {"#Bena", TermResult::Ok, '#', "bena"},     {"1234", TermResult::Ok, 0, "1234"},
        {"_-_-", TermResult::Ok, 0, "_-_-"},        {"ben", TermResult::TooShort, 0, ""},
        {"@ben", TermResult::TooShort, 0, ""},      {"", TermResult::TooShort, 0, ""},
        {"@", TermResult::TooShort, 0, ""},         {"abc%", TermResult::Syntax, 0, ""},
        {"ab cd", TermResult::Syntax, 0, ""},       {"';DROP", TermResult::Syntax, 0, ""},
        {"\\", TermResult::Syntax, 0, ""},          {"@@bena", TermResult::Syntax, 0, ""},
        {"b\xc3\xa9na", TermResult::Syntax, 0, ""},
    };
    for (const auto &c : cases) {
        Term term;
        CHECK(parseTerm(c.in, &term) == c.result);
        if (c.result == TermResult::Ok) {
            CHECK_EQ(term.sigil, c.sigil);
            CHECK_EQ(term.text, std::string(c.text));
        }
    }
    Term term;
    CHECK(parseTerm(std::string(32, 'a'), &term) == TermResult::Ok);
    CHECK(parseTerm(std::string(33, 'a'), &term) == TermResult::Syntax);
    CHECK_EQ(kMinTerm, 4);
}

TEST(requests_are_the_commands_of_the_protocol) {
    const auto ben = *parseNick("@Ben");
    CHECK_EQ(registerRequest(ben), "/register @Ben");
    CHECK_EQ(unregisterRequest(ben), "/unregister @Ben");
    CHECK_EQ(addressRequest(ben), "/address @Ben");
    CHECK_EQ(nicknameRequest({A}), "/nickname " + A);
    CHECK_EQ(nicknameRequest({B, A}), "/nickname " + B + " " + A);
    CHECK_EQ(searchRequest(Term{0, "benn"}), "/search benn");
    CHECK_EQ(searchRequest(Term{'@', "benn"}), "/search @benn");
    CHECK_EQ(whoamiRequest(), "/whoami");
    CHECK_EQ(kAnonymousPhrase, "anonymous");
    CHECK_THROWS(nicknameRequest({}));
    CHECK_THROWS(nicknameRequest({A, "BM-nope"}));
    // Eight good addresses are the most one request holds, whatever the ninth is.
    std::vector<std::string> nine;
    for (int i = 0; i < 9; ++i)
        nine.push_back(bmc::Protocol::identity("nickbot test " + std::to_string(i)).address);
    const auto eight = nicknameRequest(std::vector<std::string>(nine.begin(), nine.begin() + 8));
    CHECK(eight.rfind("/nickname ", 0) == 0 && eight.find(nine[7]) != std::string::npos);
    CHECK_THROWS(nicknameRequest(nine));
}

TEST(the_anonymous_address_is_that_of_the_chan_of_the_anonymous_phrase) {
    CHECK_EQ(std::string(kAnonymousAddress),
             bmc::Protocol::channel(std::string(kAnonymousPhrase), "x", 4).address);
}

TEST(addresses_are_asked_for_eight_at_a_time_each_once) {
    std::vector<std::string> many{A, B, C, A, B};
    const auto one = nicknameBatches(many);
    CHECK_EQ(one.size(), size_t(1));
    CHECK_EQ(one[0].size(), size_t(3)); // the repeats are asked once
    CHECK(one[0][0] == A && one[0][1] == B && one[0][2] == C);
    std::vector<std::string> twenty;
    for (int i = 0; i < 20; ++i)
        twenty.push_back("BM-" + std::to_string(i)); // not valid: batching does not look
    const auto batches = nicknameBatches(twenty);
    CHECK_EQ(batches.size(), size_t(3));
    CHECK_EQ(batches[0].size(), size_t(8));
    CHECK_EQ(batches[2].size(), size_t(4));
    CHECK(nicknameBatches({}).empty());
}

TEST(the_trailers_the_go_service_wrote_are_read) {
    const auto rows = tabbed("trailers.txt");
    CHECK_EQ(rows.size(), size_t(22));
    std::vector<Reply> r;
    for (const auto &row : rows) {
        const auto reply = parseTrailer("some words\n" + row.at(1));
        CHECK(reply.has_value());
        r.push_back(reply.value_or(Reply{}));
    }
    CHECK(r.size() == rows.size());
    if (r.size() != rows.size())
        return;
    CHECK(r[0].cmd == "register" && r[0].ok && r[0].nick == "@Ben" && r[0].address == A &&
          !r[0].already);
    CHECK(r[1].already && r[1].replaced.empty());
    CHECK_EQ(r[3].replaced, "@Ben");
    CHECK(!r[4].ok && r[4].error == "taken" && r[4].query == "@benny");
    CHECK(!r[5].ok && r[5].error == "syntax" && r[5].query.empty());
    CHECK(r[6].cmd == "address" && r[6].found.value_or(false) && r[6].nick == "@benny" &&
          r[6].address == A);
    CHECK(r[7].ok && r[7].found.has_value() && !*r[7].found && r[7].query == "@nobody");
    CHECK_EQ(r[8].byAddress.size(), size_t(2));
    CHECK(r[8].byAddress[0].address == A && r[8].byAddress[0].nicks.size() == 2 &&
          r[8].byAddress[0].nicks[1] == "/benbot");
    CHECK(r[8].byAddress[1].address == C && r[8].byAddress[1].nicks.empty());
    CHECK_EQ(r[9].query, A);
    CHECK(r[10].cmd == "search" && r[10].hits.size() == 1 && r[10].hits[0].nick == "@benny" &&
          r[10].hits[0].address == A && !r[10].more);
    CHECK(r[11].ok && r[11].hits.empty());
    CHECK(!r[12].ok && r[12].error == "too_short");
    CHECK(r[13].cmd == "whoami" && r[13].nicks.size() == 2 && r[13].address == A);
    CHECK(r[14].ok && r[14].nicks.empty() && r[14].address == C);
    CHECK(!r[15].ok && r[15].error == "anonymous");
    CHECK(r[16].cmd == "unregister" && r[16].ok && r[16].nick == "@benny");
    CHECK(!r[17].ok && r[17].error == "not_found");
    CHECK(r[18].ok && r[18].nick == "#room");
    CHECK(!r[19].ok && r[19].error == "not_chan");
    CHECK(r[20].cmd == "help" && r[20].ok);
    CHECK(!r[21].ok && r[21].error == "unknown_command" && r[21].cmd.empty());
}

namespace {
std::string line(const std::string &json) {
    return "nickbot: " + json;
}
} // namespace

TEST(only_the_last_line_is_a_trailer_and_it_has_to_be_a_good_one) {
    const std::string good = line(R"({"v":1,"cmd":"help","ok":true})");
    CHECK(parseTrailer(good).has_value());
    CHECK(parseTrailer("words\n" + good).has_value());
    CHECK(parseTrailer("words\r\n" + good + "\r\n").has_value());
    CHECK(parseTrailer("words\n" + good + "\n\n  \n").has_value());
    CHECK(!parseTrailer("").has_value());
    CHECK(!parseTrailer("only words").has_value());
    CHECK(!parseTrailer(good + "\nand then some words").has_value()); // not the last line
    CHECK(!parseTrailer("words nickbot: {\"v\":1}").has_value());     // not at a line's start
    CHECK(!parseTrailer(line("not json")).has_value());
    CHECK(!parseTrailer(line("[1,2]")).has_value());
    CHECK(!parseTrailer(line("{}")).has_value());
    CHECK(!parseTrailer(line(R"({"v":2,"cmd":"help","ok":true})")).has_value());
    CHECK(!parseTrailer(line(R"({"v":"1","cmd":"help","ok":true})")).has_value());
    CHECK(!parseTrailer(line(R"({"v":1,"cmd":"help"})")).has_value()); // no ok
    CHECK(!parseTrailer(line(R"({"v":1,"cmd":"help","ok":"yes"})")).has_value());
    CHECK(!parseTrailer(line(R"({"v":1,"ok":true})")).has_value()); // no cmd
    CHECK(!parseTrailer(line(R"({"v":1,"cmd":"explode","ok":true})")).has_value());
    // A spoof in the middle does not count: the last line decides.
    CHECK(!parseTrailer(good + "\nnickbot: nonsense").has_value());
    // What it does not know it passes over.
    const auto future = parseTrailer(
        line(R"({"v":1,"cmd":"help","ok":true,"future":{"x":[1,2]},"more_fields":1})"));
    CHECK(future.has_value());
}

TEST(an_answer_with_anything_wrong_in_it_is_not_believed) {
    const auto bad = [](const std::string &json) { return !parseTrailer(line(json)).has_value(); };
    const std::string head = R"({"v":1,"cmd":"address","ok":true,)";
    CHECK(bad(head + R"("found":true,"nick":"@b","address":")" + A + R"("})")); // not a nickname
    CHECK(bad(head + R"("found":true,"nick":"ben","address":")" + A + R"("})"));
    CHECK(bad(head + R"("found":true,"nick":"@ben","address":"BM-nope"})"));
    CHECK(bad(head + R"("found":true,"nick":"@ben<script>","address":")" + A + R"("})"));
    CHECK(bad(head + R"("found":"yes","nick":"@ben","address":")" + A + R"("})"));
    CHECK(bad(head + R"("nick":5})"));
    CHECK(!bad(head + R"("found":true,"nick":"@ben","address":")" + A + R"("})"));
    // error codes are words
    CHECK(bad(R"({"v":1,"cmd":"register","ok":false,"error":"<b>taken</b>"})"));
    CHECK(!bad(R"({"v":1,"cmd":"register","ok":false,"error":""})")); // empty means none
    CHECK(!bad(R"({"v":1,"cmd":"register","ok":false,"error":"rate_limited"})"));
    // a query is short and plain
    CHECK(bad(R"({"v":1,"cmd":"search","ok":true,"query":")" + std::string(100, 'a') + R"("})"));
    CHECK(bad(R"({"v":1,"cmd":"search","ok":true,"query":"a\nb"})"));
    // lists are of the right kind and size
    CHECK(
        bad(R"({"v":1,"cmd":"nickname","ok":true,"results":[{"address":"BM-nope","nicks":[]}]})"));
    CHECK(bad(R"({"v":1,"cmd":"nickname","ok":true,"results":[{"address":")" + A +
              R"(","nicks":["ben"]}]})"));
    CHECK(bad(R"({"v":1,"cmd":"nickname","ok":true,"results":[{"address":")" + A + R"("}]})"));
    CHECK(bad(R"({"v":1,"cmd":"nickname","ok":true,"results":"none"})"));
    CHECK(bad(R"({"v":1,"cmd":"search","ok":true,"results":[{"nick":"@ben"}]})"));
    CHECK(bad(R"({"v":1,"cmd":"whoami","ok":true,"nicks":["@ben",5]})"));
    std::string many = R"({"v":1,"cmd":"search","ok":true,"results":[)";
    for (int i = 0; i < 11; ++i)
        many += std::string(i ? "," : "") + R"({"nick":"@ben","address":")" + A + R"("})";
    CHECK(bad(many + "]}"));
    std::string ten = R"({"v":1,"cmd":"search","ok":true,"results":[)";
    for (int i = 0; i < 10; ++i)
        ten += std::string(i ? "," : "") + R"({"nick":"@ben","address":")" + A + R"("})";
    CHECK(!bad(ten + "]}"));
    std::string nine = R"({"v":1,"cmd":"nickname","ok":true,"results":[)";
    for (int i = 0; i < 9; ++i)
        nine += std::string(i ? "," : "") + R"({"address":")" + A + R"(","nicks":[]})";
    CHECK(bad(nine + "]}"));
    // Too much text is not read at all.
    CHECK(!parseTrailer(std::string(20000, 'x') + "\n" + line(R"({"v":1,"cmd":"help","ok":true})"))
               .has_value());
}

TEST(a_request_and_its_answer_share_a_key) {
    CHECK_EQ(keyOfAddressRequest(*parseNick("@Ben")), "address:@ben");
    CHECK_EQ(keyOfAddressRequest(*parseNick("#Ben")), "address:#Ben");
    CHECK_EQ(keyOfSearchRequest(Term{0, "benn"}), "search:benn");
    CHECK_EQ(keyOfSearchRequest(Term{'@', "benn"}), "search:@benn");
    CHECK_EQ(keyOfNicknameRequest({B, A, B}), "nickname:" + A + "," + B); // sorted, each once
    CHECK_EQ(keyOfNicknameRequest({B, A, B}), keyOfNicknameRequest({A, B}));
    const auto rows = tabbed("trailers.txt");
    const auto reply = [&](size_t i) { return *parseTrailer(rows.at(i).at(1)); };
    CHECK_EQ(*keyOfReply(reply(6)), keyOfAddressRequest(*parseNick("@BENNY")));
    CHECK_EQ(*keyOfReply(reply(7)), keyOfAddressRequest(*parseNick("@nobody")));
    CHECK_EQ(*keyOfReply(reply(8)), keyOfNicknameRequest({A, C}));
    CHECK_EQ(*keyOfReply(reply(9)), keyOfNicknameRequest({A}));
    CHECK_EQ(*keyOfReply(reply(10)), keyOfSearchRequest(Term{0, "benn"}));
    CHECK(!keyOfReply(reply(12))
               .has_value()); // an error carries no key: the request is told apart by the command
    CHECK(!keyOfReply(reply(0)).has_value()); // nor does a register
}
