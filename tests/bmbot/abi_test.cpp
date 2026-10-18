// The C ABI of bmbot.h: codes, ownership of strings, null handles.
#include <atomic>
#include <bmbot.h>
#include <bmc/json.h>
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
