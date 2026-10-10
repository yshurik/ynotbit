#include "coolname.h"
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <deque>
namespace bm {
namespace {
QByteArray resource(const QString &path) {
    QFile file(":/coolname/" + path);
    if (!file.open(QIODevice::ReadOnly)) // a target built without the resource
        qFatal("coolname data missing: %s", qPrintable(path));
    return file.readAll();
}
// A word list's file: "#" starts a comment, "name = value" sets an option (none
// matters here), any other line is a word, or a phrase of words.
QList<QStringList> wordList(const QString &name) {
    static const QRegularExpression space("\\s+");
    QList<QStringList> items;
    for (auto line : QString::fromUtf8(resource("data/" + name + ".txt")).split('\n')) {
        line = line.section('#', 0, 0).trimmed();
        if (!line.isEmpty() && !line.contains('='))
            items << line.split(space, Qt::SkipEmptyParts);
    }
    return items;
}
} // namespace
// coolname's lists: words and phrases, a constant, lists one after another
// (nested) or one item from each list in turn (cartesian).
struct CoolName::Tree {
    struct List {
        enum Kind { Items, Constant, Nested, Cartesian } kind = Items;
        QList<QStringList> items;  // Items: a word, or a phrase's words, each
        QString value;             // Constant
        QList<const List *> lists; // Nested: longest first; Cartesian: in order
        QList<quint64> divisors;   // Cartesian: the lengths of the lists after each, multiplied
        quint64 length = 0;
        void append(quint64 i, QStringList &out) const {
            switch (kind) {
            case Items:
                out << items[qsizetype(i)];
                break;
            case Constant:
                out << value;
                break;
            case Nested:
                for (auto list : lists) {
                    if (i < list->length)
                        return list->append(i, out);
                    i -= list->length;
                }
                break;
            case Cartesian:
                for (qsizetype j = 0; j < lists.size(); ++j) {
                    lists[j]->append(i / divisors[j], out);
                    i %= divisors[j];
                }
                break;
            }
        }
    };
    std::deque<List> lists; // grows at the end only: the pointers to it stay valid
    QHash<QString, const List *> named;
    const List *make(const QJsonObject &config, const QString &key, const QSet<QString> &excluded) {
        if (auto found = named.value(key))
            return found;
        auto &list = lists.emplace_back();
        const auto def = config.value(key).toObject();
        const auto type = def.value("type").toString();
        if (def.isEmpty()) { // not in config.json: a list of words of its own
            for (const auto &item : wordList(key))
                if (std::none_of(item.begin(), item.end(),
                                 [&](const QString &word) { return excluded.contains(word); }))
                    list.items << item;
            list.length = quint64(list.items.size());
        } else if (type == "const") {
            list.kind = List::Constant;
            list.value = def.value("value").toString();
            list.length = 1;
        } else {
            list.kind = type == "cartesian" ? List::Cartesian : List::Nested;
            for (const auto item : def.value("lists").toArray())
                list.lists << make(config, item.toString(), excluded);
            if (list.kind == List::Nested) {
                // Longest first, as upstream keeps them; ties stay in order.
                std::stable_sort(
                    list.lists.begin(), list.lists.end(),
                    [](const List *a, const List *b) { return a->length > b->length; });
                for (auto sub : list.lists)
                    list.length += sub->length;
            } else {
                list.divisors.resize(list.lists.size());
                quint64 after = 1;
                for (auto j = list.lists.size() - 1; j >= 0; --j) {
                    list.divisors[j] = after;
                    after *= list.lists[j]->length;
                }
                list.length = after;
            }
        }
        named.insert(key, &list);
        return &list;
    }
};
CoolName::CoolName(const QSet<QString> &excluded) {
    auto tree = std::make_shared<Tree>();
    const auto config = QJsonDocument::fromJson(resource("data/config.json")).object();
    for (const auto words : {"2", "3", "4"})
        tree->make(config, words, excluded);
    tree_ = std::move(tree);
}
CoolName::~CoolName() = default;
quint64 CoolName::combinations(int words) const {
    const auto list = tree_->named.value(QString::number(words));
    return list ? list->length : 0;
}
QStringList CoolName::name(int words, quint64 index) const {
    QStringList name;
    if (const auto list = tree_->named.value(QString::number(words)); list && index < list->length)
        list->append(index, name);
    return name;
}
bool CoolName::acceptable(const QStringList &name) const {
    QSet<QString> words, starts;
    auto length = name.size() - 1; // the hyphens
    for (const auto &word : name) {
        words << word;
        starts << word.left(4);
        length += word.size();
    }
    return words.size() == name.size() && starts.size() == name.size() && length <= 50;
}
QSet<QString> excludedWords() {
    static const QRegularExpression space("\\s+");
    QSet<QString> words;
    for (const auto &line : QString::fromUtf8(resource("excluded.txt")).split('\n'))
        for (const auto &word : line.section('#', 0, 0).split(space, Qt::SkipEmptyParts))
            words << word;
    return words;
}
namespace {
// Upstream's vocabulary, whole: an address's name is picked from all of it, and
// a name with an excluded word is picked again, like one breaking coolname's
// rules. So a word excluded later renames only the names that have it.
const CoolName &vocabulary() {
    static const CoolName names;
    return names;
}
} // namespace
QStringList addressName(const QString &address, const QSet<QString> &excluded) {
    auto key = address.trimmed();
    if (key.startsWith("BM-", Qt::CaseInsensitive))
        key = key.mid(3);
    if (key.isEmpty())
        return {};
    const auto &names = vocabulary();
    const auto count = names.combinations(3);
    // The first index, of a hash per attempt, whose name keeps the rules and has
    // no excluded word. A thousand misses would take a list excluding nearly all.
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const auto digest = QCryptographicHash::hash(
            ("ynotbit-name:" + key + ":" + QString::number(attempt)).toUtf8(),
            QCryptographicHash::Sha256);
        quint64 index = 0;
        for (int i = 0; i < 8; ++i)
            index = index << 8 | quint8(digest[i]);
        const auto name = names.name(3, index % count);
        if (names.acceptable(name) &&
            std::none_of(name.begin(), name.end(),
                         [&](const QString &word) { return excluded.contains(word); }))
            return name;
    }
    return {};
}
QStringList addressName(const QString &address) {
    static const auto excluded = excludedWords();
    return addressName(address, excluded);
}
} // namespace bm
