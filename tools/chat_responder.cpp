// A chat peer that stays up: it has an address, answers the chats it gets, and says what it does.
// A letter it gets (a client with no chats, PyBitmessage, writes letters) is answered with a letter
// that says where chats are, once a day to a sender.
// Runs headless, on a vault of its own and with a node of its own, so ynotbit and it chat as two
// people would. It is a test peer for ynotbit's chats, and made to run for months as a service:
// what came while it was down is answered when it is back, what it remembers is bounded, a peer
// cannot make it answer without limit, and a node that stops doing its work is started again.
//
// It uses no Qt, and nothing of the core but the C ABI of bmbot.h: it is the reference bot on the
// bot library, and what a bot in Go or Python does is what it does here.
//
//   cmake --build build --target chat_responder bmnode
//   build/chat_responder
//
// It prints its address. In ynotbit: Chats > New chat, paste the address, write to it; say /help
// to it. By default its node links only to ynotbit's own node on this machine (127.0.0.1:8444)
// and leaves the public network alone, so it works offline; it dials that node again every
// half minute at most, so quitting and starting ynotbit costs it seconds.
//
// For months, on a machine of its own:
//
//   build/chat_responder --public --listen --phrase-file secret.txt --data-dir /var/lib/responder \
//                        --log responder.log --status-file status.json
//
//   --phrase <text>       what its address comes from: one phrase, one address (default "ynotbit
//                         chat responder", which is public: anyone can read what is sent to it)
//   --phrase-file <file>  the phrase from a file, or from $YNOTBIT_RESPONDER_PHRASE: not in the
//                         process list, and the vault's password comes from it, so what is on the
//                         disk is of no use without it
//   --peer <ip:port>      the node to link to (default 127.0.0.1:8444, ynotbit's); "" for none
//   --public              also join the public network, not only the peer, with more peers
//   --listen              accept incoming connections (with --public); --port <n> for a port
//                         other than 8444. The router must forward it: there is no UPnP here
//   --max-outgoing <n>    outgoing peers wanted (8; 12 with --public), at most 64
//   --max-incoming <n>    incoming peers accepted (8; 24 with --public), at most 512
//   --watchdog <minutes>  restart its node when it has had no peer that long (0 never; 15 with
//                         --public); a node whose status stopped is always restarted
//   --hang <seconds>      abort when its driver loop stalls that long, so that Docker or systemd
//                         starts it again (default 120, 0 never)
//   --retention-mb <n>, --retention-days <n>
//                         how much of the network's objects the node keeps (2048 MB, 90 days)
//   --node-bin <path>     the node (default: bmnode beside this program, or on the PATH)
//   --delay <seconds>     how long it takes to answer (default 1)
//   --rate <n>            answers a peer gets a minute, after a first burst of --burst (30 and
//                         30); --global-rate and --global-burst for all peers together (300, 200);
//                         0 sets no limit
//   --silent              answer nothing, to letters too: a peer that only listens and logs what
//                         it is sent
//   --hello <address>     say hello to an address once, when it starts: a test of a link, or
//                         the probe of a monitor that waits for the answer
//   --hello-letter <address>
//                         the same with a letter instead of a chat: it tests what a bot answers
//                         to a letter
//   --log <file>          also write what it says to a file, rotated at 10 MB, five kept
//   --status-file <file>  a JSON file made again every ten seconds: peers, counts, uptime
//   --data-dir <dir>      where its vault, mailbox and objects are kept, as they are given; with
//                         none, a folder for each phrase under the user's data directory
//   --fresh               start from nothing: forget them first
//   --check <status-file> [--max-age <seconds>] [--peers <n>]
//                         exit 0 if the status file is that fresh (60) and says that many peers
//                         (1), else 1: a health check that needs no shell tools
#include "chat_bot.h"
#include "letter_reply.h"
#include "rotating_log.h"
#include <algorithm>
#include <bmbot.h>
#include <bmc/bytes.h>
#include <bmc/hash.h>
#include <bmc/json.h>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
volatile std::sig_atomic_t interrupted = 0;
namespace fs = std::filesystem;
using tools::ChatBot;
using tools::ChatBotOptions;

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S ", &local);
    return text;
}

// Where it says what it does: standard output and, with --log, a file kept to a size.
class Logger {
  public:
    void open(const std::string &path) {
        if (!file_.open(path))
            std::cerr << "cannot write the log " << path << "\n";
    }
    void say(const std::string &message) {
        const auto line = timestamp() + message;
        std::cout << line << std::endl;
        file_.write(line);
    }

  private:
    tools::RotatingLog file_; // 10 MB, five kept
};

// What it says to what it is told: a few commands to try a chat with, else an echo.
std::vector<std::string> answers(const std::string &text) {
    std::istringstream in(text);
    std::vector<std::string> words;
    for (std::string w; in >> w;)
        words.push_back(w);
    std::string command = words.empty() ? "" : words[0];
    std::transform(command.begin(), command.end(), command.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (command == "/help")
        return {"I answer chats. Try:\n\n"
                "- `/ping`: pong\n"
                "- `/time`: the time here\n"
                "- `/md`: some Markdown\n"
                "- `/long`: a long answer\n"
                "- `/many [n]`: n short answers in a row\n\n"
                "Anything else I say back."};
    if (command == "/ping")
        return {"pong"};
    if (command == "/time") {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char shown[96];
        std::strftime(shown, sizeof shown, "%A %-d %B %Y, %H:%M:%S", &local);
        return {shown};
    }
    if (command == "/md")
        return {"# Markdown\n\n**Bold**, *italic*, ~~struck~~ and `code`.\n\n"
                "1. one\n2. two\n3. three\n\n> a quote\n\n```\na block of code\n```\n\n"
                "A [link](https://example.org)."};
    if (command == "/long") {
        std::string joined;
        for (int i = 1; i <= 6; ++i)
            joined += std::string(i > 1 ? "\n\n" : "") + "**Paragraph " + std::to_string(i) +
                      ".** The quick brown fox jumps over the lazy dog, and keeps on running "
                      "through the whole afternoon, until the line must break more than once, "
                      "and then again.";
        return {joined};
    }
    if (command == "/many") {
        int n = 5;
        if (words.size() > 1) {
            const int given = std::atoi(words[1].c_str());
            if (given > 0)
                n = given;
        }
        n = std::clamp(n, 1, 20);
        std::vector<std::string> each;
        for (int i = 1; i <= n; ++i)
            each.push_back("Message " + std::to_string(i) + " of " + std::to_string(n));
        return each;
    }
    // The chat itself is 8192 bytes at most; the quote leaves room for the words around it.
    std::string quoted = text.substr(0, 4000);
    while (!quoted.empty() && (static_cast<unsigned char>(quoted.back()) & 0xC0) == 0x80 &&
           quoted.size() < text.size())
        quoted.pop_back(); // never end inside a UTF-8 sequence
    if (quoted.size() < text.size() && !quoted.empty() &&
        static_cast<unsigned char>(quoted.back()) >= 0xC0)
        quoted.pop_back();
    return {"You said: " + quoted};
}

// --- the command line ---------------------------------------------------------------------------

class Args {
  public:
    Args(int argc, char **argv) : args_(argv + 1, argv + argc) {}
    bool has(const std::string &name) const {
        return std::find(args_.begin(), args_.end(), name) != args_.end();
    }
    // "--name value", or empty.
    std::string value(const std::string &name) const {
        for (size_t i = 0; i + 1 < args_.size(); ++i)
            if (args_[i] == name)
                return args_[i + 1];
        return {};
    }

  private:
    std::vector<std::string> args_;
};

int fail(const std::string &message) {
    std::cerr << "chat_responder: " << message << "\n";
    return 1;
}

// The home folders a user's tool keeps its data in.
std::string defaultDataDir() {
    const char *home = std::getenv("HOME");
    const std::string h = home && *home ? home : ".";
#ifdef __APPLE__
    return h + "/Library/Application Support/YnotbitTools/chat_responder";
#else
    const char *xdg = std::getenv("XDG_DATA_HOME");
    return (xdg && *xdg ? std::string(xdg) : h + "/.local/share") + "/YnotbitTools/chat_responder";
#endif
}

std::string firstLine(const std::string &path, bool &ok) {
    std::ifstream in(path, std::ios::binary);
    ok = bool(in);
    std::string line;
    std::getline(in, line);
    const char *space = " \t\r\n";
    line.erase(0, std::min(line.find_first_not_of(space), line.size()));
    const auto last = line.find_last_not_of(space);
    line.erase(last == std::string::npos ? 0 : last + 1);
    return line;
}

// --- --check: the container's health check -----------------------------------------------------

int check(const Args &args) {
    const auto file = args.value("--check");
    const double maxAge = args.has("--max-age") ? std::atof(args.value("--max-age").c_str()) : 60;
    const int peers = args.has("--peers") ? std::atoi(args.value("--peers").c_str()) : 1;
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const auto parsed = bmc::json::parse(text.str());
    if (!in || !parsed || !parsed->isObject()) {
        std::cerr << "unhealthy: no status in " << file << "\n";
        return 1;
    }
    const auto age = double(std::time(nullptr)) - double(parsed->at("time").toInteger());
    if (age > maxAge) {
        std::cerr << "unhealthy: the status is " << int(age) << " s old\n";
        return 1;
    }
    if (parsed->at("peers").toInteger() < peers) {
        std::cerr << "unhealthy: " << parsed->at("peers").toInteger() << " peers, " << peers
                  << " wanted\n";
        return 1;
    }
    std::cout << "healthy: " << parsed->at("peers").toInteger() << " peers, status " << int(age)
              << " s old\n";
    return 0;
}

// --- the bot library as the chat bot's host ----------------------------------------------------

std::string takeString(char *&p) {
    std::string s = p ? p : "";
    bmbot_free(p);
    p = nullptr;
    return s;
}

class AbiHost : public tools::ChatHost {
  public:
    AbiHost(bmbot *bot, std::function<void(const std::string &)> say) : bot_(bot), say_(say) {}

    // The bot hands out chats and letters in one stream, oldest first. Each asker takes its own
    // kind and keeps the other's for the other asker, a few at most: past that the stream waits.
    // Notes (problems, watchdog restarts) go to the log.
    bool nextChat(Chat &chat) override {
        while (chats_.empty() && letters_.size() < kHeld && pull()) {
        }
        if (chats_.empty())
            return false;
        chat = std::move(chats_.front());
        chats_.pop_front();
        return true;
    }
    bool nextLetter(Letter &letter) override {
        while (letters_.empty() && chats_.size() < kHeld && pull()) {
        }
        if (letters_.empty())
            return false;
        letter = std::move(letters_.front());
        letters_.pop_front();
        return true;
    }
    void done(const std::string &id) override {
        finish(id);
    }
    bool send(const std::string &local, const std::string &peer, const std::string &text,
              std::string &id, std::string &error) override {
        char *made = nullptr, *why = nullptr;
        const int code =
            bmbot_send_chat(bot_, local.c_str(), peer.c_str(), text.c_str(), &made, &why);
        id = takeString(made);
        error = takeString(why);
        return code == BMBOT_OK;
    }
    bool sendLetter(const std::string &local, const std::string &peer, const std::string &subject,
                    const std::string &body, std::string &id, std::string &error) override {
        char *made = nullptr, *why = nullptr;
        const int code = bmbot_send_letter(bot_, local.c_str(), peer.c_str(), subject.c_str(),
                                           body.c_str(), &made, &why);
        id = takeString(made);
        error = takeString(why);
        return code == BMBOT_OK;
    }
    std::string state(const std::string &id) override {
        char *state = nullptr, *error = nullptr;
        bmbot_state(bot_, id.c_str(), &state, &error);
        takeString(error);
        return takeString(state);
    }

  private:
    static constexpr size_t kHeld = 50; // events of the other kind kept for the other asker
    bmbot *bot_;
    std::function<void(const std::string &)> say_;
    std::string lastTrouble_;
    std::deque<Chat> chats_;
    std::deque<Letter> letters_;
    // Files the next event under its kind; false when there is none.
    bool pull() {
        for (;;) {
            char *event = nullptr, *error = nullptr;
            const int code = bmbot_next(bot_, 0, &event, &error);
            const auto text = takeString(event);
            const auto why = takeString(error);
            if (code == BMBOT_TIMEOUT)
                return false;
            if (code != BMBOT_OK) {
                if (!why.empty() && why != lastTrouble_) {
                    lastTrouble_ = why;
                    say_("Problem: " + why);
                }
                return false;
            }
            const auto parsed = bmc::json::parse(text);
            if (!parsed)
                continue;
            const auto kind = parsed->at("kind").toString();
            if (kind == "note") {
                say_(parsed->at("text").toString());
                continue;
            }
            if (kind == "letter")
                letters_.push_back({parsed->at("id").toString(), parsed->at("to").toString(),
                                    parsed->at("from").toString(),
                                    parsed->at("subject").toString(),
                                    parsed->at("text").toString()});
            else
                chats_.push_back({parsed->at("id").toString(), parsed->at("to").toString(),
                                  parsed->at("from").toString(), parsed->at("text").toString()});
            return true;
        }
    }
    void finish(const std::string &id) {
        char *error = nullptr;
        bmbot_done(bot_, id.c_str(), &error);
        takeString(error);
    }
};

} // namespace

int main(int argc, char **argv) {
    const Args args(argc, argv);
    if (args.has("--help") || args.has("-h")) {
        std::cout
            << "usage: chat_responder [--phrase <text> | --phrase-file <file>] [--peer <ip:port>]\n"
               "                      [--public] [--listen] [--port <n>] [--max-outgoing <n>]\n"
               "                      [--max-incoming <n>] [--watchdog <minutes>] [--hang "
               "<seconds>]\n"
               "                      [--retention-mb <n>] [--retention-days <n>] [--node-bin "
               "<path>]\n"
               "                      [--delay <seconds>] [--rate <n>] [--burst <n>]\n"
               "                      [--global-rate <n>] [--global-burst <n>] [--silent]\n"
               "                      [--hello <address>] [--hello-letter <address>]\n"
               "                      [--log <file>] [--status-file <file>]\n"
               "                      [--data-dir <dir>] [--fresh]\n"
               "       chat_responder --check <status-file> [--max-age <seconds>] [--peers <n>]\n";
        return 0;
    }
    if (args.has("--check"))
        return check(args);

    // A number option, or the fallback; false when it is not a number within the range.
    bool numbersOk = true;
    const auto number = [&](const char *name, double fallback, double low, double high) {
        const auto text = args.value(name);
        if (text.empty())
            return fallback;
        char *end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end || value < low || value > high) {
            std::cerr << "chat_responder: " << name << " takes a number from " << low << " to "
                      << high << "\n";
            numbersOk = false;
            return fallback;
        }
        return value;
    };

    // The phrase an address comes from: a file or the environment keep it out of the process list.
    std::string phrase = "ynotbit chat responder", phraseFile;
    bool secret = false;
    if (const auto file = args.value("--phrase-file"); !file.empty()) {
        bool ok = false;
        phrase = firstLine(file, ok);
        if (!ok)
            return fail("cannot read " + file);
        phraseFile = file;
        secret = true;
    } else if (const char *env = std::getenv("YNOTBIT_RESPONDER_PHRASE"); env && *env) {
        phrase = env;
        const char *space = " \t\r\n";
        phrase.erase(0, std::min(phrase.find_first_not_of(space), phrase.size()));
        const auto last = phrase.find_last_not_of(space);
        phrase.erase(last == std::string::npos ? 0 : last + 1);
        secret = true;
    } else if (!args.value("--phrase").empty()) {
        phrase = args.value("--phrase");
    }
    if (phrase.empty())
        return fail("the phrase is empty");

    const bool joinPublic = args.has("--public");
    const bool listen = args.has("--listen");
    const std::string peer = args.has("--peer") ? args.value("--peer") : "127.0.0.1:8444";
    if (peer.empty() && !joinPublic)
        return fail("--peer takes an IP:port, like 127.0.0.1:8444");
    const double delay = number("--delay", 1.0, 0, 3600);
    const int port = int(number("--port", 0, 1, 65535));
    const int maxOutgoing = int(number("--max-outgoing", joinPublic ? 12 : 0, 1, 64));
    const int maxIncoming = int(number("--max-incoming", joinPublic ? 24 : 0, 1, 512));
    const int watchdog = int(number("--watchdog", joinPublic ? 15 : 0, 0, 24 * 60));
    const int hang = int(number("--hang", 120, 0, 86400));
    const int retentionMB = int(number("--retention-mb", 2048, 64, 32768));
    const int retentionDays = int(number("--retention-days", 90, 1, 3650));
    ChatBotOptions options;
    options.delayMs = int(delay * 1000);
    options.ratePerMinute = number("--rate", options.ratePerMinute, 0, 1e6);
    options.rateBurst = int(number("--burst", options.rateBurst, 1, 1e6));
    options.globalPerMinute = number("--global-rate", options.globalPerMinute, 0, 1e6);
    options.globalBurst = int(number("--global-burst", options.globalBurst, 1, 1e6));
    if (!numbersOk)
        return 1;

    // Where its files are kept: as given, or a folder for each phrase under the user's data
    // directory (named by a hash of the phrase, as the first responder did).
    std::string root = args.value("--data-dir");
    if (root.empty()) {
        const auto digest = bmc::hex(bmc::hash(bmc::Digest::Sha256, bmc::view(phrase)));
        root = defaultDataDir() + "/" + digest.substr(0, 8);
    }
    if (args.has("--fresh")) {
        std::error_code ignored;
        const bool ours = fs::exists(root + "/bot.bmvault", ignored) ||
                          fs::exists(root + "/responder.bmvault", ignored);
        if (fs::exists(root, ignored) && !ours && !fs::is_empty(root, ignored))
            return fail("--fresh removes a bot's folder, and " + root + " holds something else");
        fs::remove_all(root, ignored);
    }

    std::string config = "{\"data_dir\":" + bmc::json::quote(root) +
                         ",\"label\":\"Chat responder\",\"hang_seconds\":" + std::to_string(hang);
    if (!phraseFile.empty())
        config += ",\"phrase_file\":" + bmc::json::quote(phraseFile);
    else
        config += ",\"phrase\":" + bmc::json::quote(phrase) +
                  ",\"secret\":" + (secret ? "true" : "false");
    config +=
        ",\"node\":{\"bin\":" +
        bmc::json::quote(args.has("--node-bin") ? args.value("--node-bin") : std::string("auto")) +
        ",\"peer\":" + bmc::json::quote(peer) + ",\"public\":" + (joinPublic ? "true" : "false") +
        ",\"listen\":" + (listen ? "true" : "false") +
        ",\"watchdog_minutes\":" + std::to_string(watchdog) +
        ",\"retention_mb\":" + std::to_string(retentionMB) +
        ",\"retention_days\":" + std::to_string(retentionDays);
    if (port > 0)
        config += ",\"port\":" + std::to_string(port);
    if (maxOutgoing > 0)
        config += ",\"max_outgoing\":" + std::to_string(maxOutgoing);
    if (maxIncoming > 0)
        config += ",\"max_incoming\":" + std::to_string(maxIncoming);
    config += "}}";

    bmbot *bot = nullptr;
    {
        char *error = nullptr;
        if (bmbot_open(config.c_str(), &bot, &error) != BMBOT_OK)
            return fail(takeString(error));
    }
    std::signal(SIGINT, [](int) { interrupted = 1; });
    std::signal(SIGTERM, [](int) { interrupted = 1; });

    Logger log;
    if (const auto path = args.value("--log"); !path.empty())
        log.open(path);
    const auto say = [&](const std::string &line) { log.say(line); };

    const auto status = [&]() -> bmc::json::Value {
        char *json = nullptr, *error = nullptr;
        bmbot_status(bot, &json, &error);
        takeString(error);
        const auto parsed = bmc::json::parse(takeString(json));
        return parsed ? *parsed : bmc::json::Value();
    };
    const auto first = status();
    const auto address = first.at("address").toString();
    say("Chat responder");
    say("Address:  " + address);
    say(joinPublic ? "Node:     joins the public network" +
                         (peer.empty() ? std::string() : ", and links to " + peer)
                   : "Node:     links to " + peer + " only, the public network left alone");
    say("Data:     " + root);
    say("In ynotbit: Chats > New chat > paste the address. Say /help to it. Ctrl-C quits.");

    const bool silent = args.has("--silent");
    AbiHost host(bot, say);
    ChatBot chatBot(
        host,
        [&](const std::string &, const std::string &, const std::string &text) {
            return silent ? std::vector<std::string>() : answers(text);
        },
        options, say,
        silent ? ChatBot::LetterHandler()
               : ChatBot::LetterHandler([](const std::string &local, const std::string &,
                                           const std::string &subject, const std::string &)
                                            -> std::optional<ChatBot::LetterReply> {
                     return tools::letterReply(local, subject);
                 }));
    if (const auto hello = args.value("--hello"); !hello.empty()) {
        char *id = nullptr, *error = nullptr;
        if (bmbot_send_chat(bot, "", hello.c_str(), ("hello from " + address).c_str(), &id,
                            &error) == BMBOT_OK)
            say("Hello:    sent to " + ChatBot::brief(hello));
        else
            say("Hello:    cannot be sent: " + takeString(error));
        takeString(id);
    }
    if (const auto hello = args.value("--hello-letter"); !hello.empty()) {
        char *id = nullptr, *error = nullptr;
        if (bmbot_send_letter(bot, "", hello.c_str(), "hello", ("hello from " + address).c_str(),
                              &id, &error) == BMBOT_OK)
            say("Hello:    a letter sent to " + ChatBot::brief(hello));
        else
            say("Hello:    the letter cannot be sent: " + takeString(error));
        takeString(id);
    }

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto millis = [&] {
        return int64_t(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
    };
    const auto stateLine = [&](const bmc::json::Value &s) {
        const auto stats = chatBot.stats();
        const auto &node = s.at("node");
        return "peers " + std::to_string(node.at("peers").toInteger()) + " (out " +
               std::to_string(node.at("outgoing").toInteger()) + ", in " +
               std::to_string(node.at("incoming").toInteger()) + ") · objects " +
               std::to_string(node.at("objects").toInteger()) +
               " · chats: " + std::to_string(stats.received) + " taken up, " +
               std::to_string(stats.answered) + " answered, " + std::to_string(stats.limited) +
               " limited, " + std::to_string(stats.failed) + " given up" +
               " · letters: " + std::to_string(stats.letters) + " taken up, " +
               std::to_string(stats.lettersAnswered) + " answered";
    };
    // For whoever watches it from outside: one small file, made again every ten seconds.
    const auto statusFile = args.value("--status-file");
    const auto writeStatusFile = [&](const bmc::json::Value &s) {
        const auto stats = chatBot.stats();
        const auto &node = s.at("node");
        const std::string out =
            "{\"time\":" + std::to_string(std::time(nullptr)) +
            ",\"uptime\":" + std::to_string(millis() / 1000) +
            ",\"address\":" + bmc::json::quote(address) +
            ",\"peers\":" + std::to_string(node.at("peers").toInteger()) +
            ",\"outgoing\":" + std::to_string(node.at("outgoing").toInteger()) +
            ",\"incoming\":" + std::to_string(node.at("incoming").toInteger()) +
            ",\"objects\":" + std::to_string(node.at("objects").toInteger()) +
            ",\"network\":" + bmc::json::quote(s.at("network").toString()) +
            ",\"received\":" + std::to_string(stats.received) +
            ",\"answered\":" + std::to_string(stats.answered) +
            ",\"limited\":" + std::to_string(stats.limited) +
            ",\"failed\":" + std::to_string(stats.failed) +
            ",\"letters\":" + std::to_string(stats.letters) +
            ",\"lettersAnswered\":" + std::to_string(stats.lettersAnswered) +
            ",\"working\":" + std::to_string(stats.working) +
            ",\"watchdogRestarts\":" + std::to_string(node.at("watchdog_restarts").toInteger()) +
            "}";
        const auto temporary = statusFile + ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << out;
        }
        std::error_code ignored;
        fs::rename(temporary, statusFile, ignored);
    };

    int64_t lastPeers = -1, nextReport = 0, nextHealth = 0, nextAlive = 10 * 60 * 1000;
    while (!interrupted) {
        const auto now = millis();
        chatBot.poll(now);
        if (now >= nextReport) {
            nextReport = now + 5000;
            const auto s = status();
            const auto peers = s.at("node").at("peers").toInteger();
            if (peers != lastPeers) {
                say((lastPeers < 0 ? std::string("Network:  ")
                                   : "Network:  peers " + std::to_string(lastPeers) + " -> " +
                                         std::to_string(peers) + "; ") +
                    s.at("network").toString());
                lastPeers = peers;
            }
            if (now >= nextAlive) {
                nextAlive = now + 10 * 60 * 1000;
                say("Alive:    " + stateLine(s));
            }
            if (!statusFile.empty() && now >= nextHealth) {
                nextHealth = now + 10000;
                writeStatusFile(s);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto last = status();
    say("Bye.     " + stateLine(last));
    bmbot_close(bot);
    return 0;
}
