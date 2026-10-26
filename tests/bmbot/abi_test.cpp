// The C ABI of bmbot.h: codes, ownership of strings, null handles.
#include <atomic>
#include <bmbot.h>
#include <bmc/json.h>
#include <bmc/phrase.h>
#include <bmc/test.h>
#include <chrono>
#include <string>
#include <thread>

namespace {
// Takes ownership of a string the library returned.
std::string take(char *&p) {
    std::string s = p ? p : "";
    bmbot_free(p);
    p = nullptr;
    return s;
}
std::string config(const std::string &dir) {
    return R"({"data_dir":)" + bmc::json::quote(dir) +
           R"(,"phrase":"abi test phrase","node":{"bin":""},"step_ms":20})";
}
} // namespace

TEST(the_abi_has_a_version_and_refuses_bad_arguments_with_a_code_and_a_reason) {
    CHECK_EQ(bmbot_abi_version(), 1);
    bmbot *bot = reinterpret_cast<bmbot *>(1);
    char *error = nullptr;
    CHECK_EQ(bmbot_open("not json", &bot, &error), BMBOT_E_ARGUMENT);
    CHECK(bot == nullptr);
    CHECK(take(error).find("JSON object") != std::string::npos);
    CHECK_EQ(bmbot_open(nullptr, &bot, &error), BMBOT_E_ARGUMENT);
    take(error);
    CHECK_EQ(bmbot_open("{}", nullptr, nullptr), BMBOT_E_ARGUMENT); // the error is optional
    CHECK_EQ(bmbot_next(nullptr, 0, nullptr, nullptr), BMBOT_E_ARGUMENT);
    char *text = nullptr;
    CHECK_EQ(bmbot_next(nullptr, 0, &text, &error), BMBOT_E_ARGUMENT);
    CHECK(text == nullptr);
    take(error);
    CHECK_EQ(bmbot_done(nullptr, "x", &error), BMBOT_E_ARGUMENT);
    take(error);
    bmbot_close(nullptr); // harmless
    bmbot_free(nullptr);
}

TEST(a_bot_opens_reports_its_status_times_out_and_closes) {
    bmbot *bot = nullptr;
    char *error = nullptr;
    const auto json = config(bmc::test::tempDir() + "/abi");
    CHECK_EQ(bmbot_open(json.c_str(), &bot, &error), BMBOT_OK);
    CHECK(bot != nullptr && error == nullptr);

    char *status = nullptr;
    CHECK_EQ(bmbot_status(bot, &status, &error), BMBOT_OK);
    const auto parsed = bmc::json::parse(take(status));
    CHECK(parsed.has_value());
    CHECK(parsed->at("address").toString().rfind("BM-", 0) == 0);

    char *event = nullptr;
    CHECK_EQ(bmbot_next(bot, 100, &event, &error), BMBOT_TIMEOUT);
    CHECK(event == nullptr);

    char *state = nullptr;
    CHECK_EQ(bmbot_state(bot, "no such id", &state, &error), BMBOT_OK);
    CHECK_EQ(take(state), std::string());
    CHECK_EQ(bmbot_done(bot, "no such id", &error), BMBOT_E_ARGUMENT);
    CHECK(take(error).find("Unknown event") != std::string::npos);

    char *id = nullptr;
    CHECK_EQ(bmbot_send_chat(bot, nullptr, "not an address", "hi", &id, &error), BMBOT_E_ARGUMENT);
    CHECK(id == nullptr);
    CHECK(take(error).find("valid Bitmessage recipient") != std::string::npos);
    CHECK_EQ(bmbot_send_chat(bot, "", nullptr, "hi", &id, &error),
             BMBOT_E_ARGUMENT); // to is required
    take(error);

    // The data directory is held while the bot is open.
    bmbot *second = nullptr;
    CHECK_EQ(bmbot_open(json.c_str(), &second, &error), BMBOT_E_STATE);
    CHECK(second == nullptr);
    take(error);
    bmbot_close(bot);
    CHECK_EQ(bmbot_open(json.c_str(), &second, &error), BMBOT_OK); // and free again after close
    bmbot_close(second);
}

TEST(a_chat_sent_to_a_valid_address_gets_an_id_and_a_state) {
    bmbot *bot = nullptr;
    const auto json = config(bmc::test::tempDir() + "/abi");
    CHECK_EQ(bmbot_open(json.c_str(), &bot, nullptr), BMBOT_OK);
    char *id = nullptr, *error = nullptr;
    // Another bot's address: valid, and not ours.
    const char *peer = "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN";
    CHECK_EQ(bmbot_send_chat(bot, nullptr, peer, "hello", &id, &error), BMBOT_OK);
    const auto made = take(id);
    CHECK(!made.empty());
    char *state = nullptr;
    CHECK_EQ(bmbot_state(bot, made.c_str(), &state, &error), BMBOT_OK);
    CHECK(!take(state).empty()); // queued, or on its way to a key
    bmbot_close(bot);
}

TEST(closing_wakes_a_caller_waiting_in_next_with_a_state_error) {
    bmbot *bot = nullptr;
    CHECK_EQ(bmbot_open(config(bmc::test::tempDir() + "/abi").c_str(), &bot, nullptr), BMBOT_OK);
    std::atomic<int> code{99};
    std::thread waiter([&] {
        char *event = nullptr, *error = nullptr;
        code = bmbot_next(bot, 60000, &event, &error);
        bmbot_free(error);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bmbot_close(bot);
    waiter.join();
    CHECK_EQ(code.load(), int(BMBOT_E_STATE));
}

TEST(a_chan_address_comes_from_the_phrase_alone) {
    char *address = nullptr, *error = nullptr;
    // The chan "general" of PyBitmessage, version 4 (tests/bmclient/protocol_test.cpp has the
    // same).
    CHECK_EQ(bmbot_chan_address("general", &address, &error), BMBOT_OK);
    CHECK_EQ(take(address), "BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r");
    CHECK(error == nullptr);
    // A phrase is its bytes: another case is another chan.
    CHECK_EQ(bmbot_chan_address("General", &address, &error), BMBOT_OK);
    const auto capital = take(address);
    CHECK(capital.rfind("BM-", 0) == 0 && capital != "BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r");
    // The same phrase, the same address, whatever else the library is doing.
    CHECK_EQ(bmbot_chan_address("general", &address, &error), BMBOT_OK);
    CHECK_EQ(take(address), "BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r");
    // Refusals carry a code and a reason, and leave nothing to free.
    CHECK_EQ(bmbot_chan_address("", &address, &error), BMBOT_E_ARGUMENT);
    CHECK(address == nullptr);
    CHECK(take(error).find("cannot be empty") != std::string::npos);
    CHECK_EQ(bmbot_chan_address(nullptr, &address, &error), BMBOT_E_ARGUMENT);
    CHECK(address == nullptr);
    take(error);
    CHECK_EQ(bmbot_chan_address("general", nullptr, &error), BMBOT_E_ARGUMENT);
    take(error);
    CHECK_EQ(bmbot_chan_address("general", nullptr, nullptr),
             BMBOT_E_ARGUMENT); // the error is optional
}

TEST(a_bot_opens_from_a_keys_file_and_has_the_address_of_its_keys) {
    unsigned char sign[32], enc[32];
    bmc::phrase::key("abi test phrase", 0, sign);
    bmc::phrase::key("abi test phrase", 1, enc);
    const auto file = bmc::test::tempDir() + "/keys.txt";
    const std::string keys =
        bmc::hex(bmc::ByteView(sign, 32)) + "\n" + bmc::hex(bmc::ByteView(enc, 32)) + "\n";
    bmc::test::writeFile(file, bmc::view(keys));
    const auto json = R"({"data_dir":)" + bmc::json::quote(bmc::test::tempDir() + "/abi") +
                      R"(,"keys_file":)" + bmc::json::quote(file) +
                      R"(,"node":{"bin":""},"step_ms":20})";
    bmbot *bot = nullptr;
    char *error = nullptr, *status = nullptr;
    CHECK_EQ(bmbot_open(json.c_str(), &bot, &error), BMBOT_OK);
    CHECK_EQ(bmbot_status(bot, &status, &error), BMBOT_OK);
    const auto parsed = bmc::json::parse(take(status));
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->at("address").toString(),
             bmc::phrase::identity("abi test phrase", "x", 0).address);
    bmbot_close(bot);

    // A phrase beside the keys is a mistake, said so.
    const auto both =
        R"({"data_dir":"/x","phrase":"p","keys_file":)" + bmc::json::quote(file) + "}";
    CHECK_EQ(bmbot_open(both.c_str(), &bot, &error), BMBOT_E_ARGUMENT);
    CHECK(bot == nullptr);
    CHECK(take(error).find("keys_file") != std::string::npos);
}
