#pragma once
// What chat_responder says to a letter, which it does not read: where chats are. A user of a client
// with no chats (PyBitmessage), or of ynotbit who wrote a letter instead of chatting, gets it
// once a day (ChatBot's limits); the subject follows theirs.
#include "chat_bot.h"
#include <cctype>
#include <string>

namespace tools {

// address is the bot's own, which the letter came to.
inline ChatBot::LetterReply letterReply(const std::string &address, const std::string &subject) {
    auto title = ChatBot::oneLine(subject);
    const auto lower = [&](size_t i) { return std::tolower(static_cast<unsigned char>(title[i])); };
    const bool answer = title.size() >= 3 && lower(0) == 'r' && lower(1) == 'e' && title[2] == ':';
    if (title.empty())
        title = "I answer chats, not letters";
    else if (!answer)
        title = "Re: " + title;
    return {title,
            "Hello! I am /echobot, an echo bot. I do not read letters; I answer chats.\n\n"
            "To talk to me in ynotbit:\n"
            "1. Open Chats, then New chat.\n"
            "2. Paste my address: " + address + "\n"
            "3. Write to me. Say /help to see what I can do.\n\n"
            "Get ynotbit: https://github.com/yshurik/ynotbit/releases\n\n"
            "I send this once a day to anyone who writes to me, so a reply to this letter gets no "
            "answer."};
}

} // namespace tools
