#pragma once
// The body a digest issue is published with.
#include <QString>
namespace tools {
// Every broadcast issue ends with this, so readers always know how to contribute.
inline QString digestFooter(const QString &address) {
    return QString(
               "---\n\n"
               "**Bitmessage digest** is a weekly round-up from the Bitmessage network.\n\n"
               "Want something in the next issue? Reply privately to this broadcast, or write "
               "to `%1`, and send:\n\n"
               "- broadcast addresses you want promoted: yours, or ones worth following\n"
               "- news, offers, updates, new chans and tools\n\n"
               "Follow the digest: ynotbit subscribes to it by default. In PyBitmessage, add a "
               "subscription to `%1`.")
        .arg(address);
}
// The issue (trimmed) as published. A broadcast ends with the footer; a letter to chans
// goes without it, because the footer speaks of "this broadcast".
inline QString digestBody(const QString &issue, const QString &address, bool toChans) {
    const auto end = digestFooter(address);
    // An issue drafted from the last one may carry its footer already.
    const bool carriesFooter = issue.endsWith(end);
    if (toChans)
        return carriesFooter ? issue.chopped(end.size()).trimmed() : issue;
    return carriesFooter ? issue : issue + "\n\n" + end;
}
} // namespace tools
