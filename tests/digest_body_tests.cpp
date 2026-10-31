// The body a digest issue is published with: the footer asking for submissions
// belongs to a broadcast, and a letter to chans goes without it.
#include "digest_body.h"
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    try {
        const QString address = "BM-5oSrHVwbsQYPKmePHfqQJZr6eibiqj9";
        const QString issue = "One story this time.\n\n## Main story\n\nText.";
        const auto footer = tools::digestFooter(address);
        require(footer.startsWith("---\n\n**Bitmessage digest**") && footer.count(address) == 2,
                "the footer is a rule, the title, and the address twice");

        const auto sent = tools::digestBody(issue, address, false);
        require(sent == issue + "\n\n" + footer,
                "a broadcast ends with the footer, one blank line after the issue");
        require(tools::digestBody(sent, address, false) == sent,
                "an issue drafted from a sent one does not get the footer twice");

        require(tools::digestBody(issue, address, true) == issue,
                "a letter to chans is the issue as written, without the footer");
        require(tools::digestBody(sent, address, true) == issue,
                "a letter to chans loses the footer an issue carries");
        require(!tools::digestBody(issue, address, true).contains("Reply privately"),
                "nothing in a chan letter speaks of replying to a broadcast");

        std::cout << "PASS: digest body with and without the footer\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
