#pragma once
// Names from coolname's vocabulary (github.com/alexanderlukanin13/coolname,
// BSD-2-Clause), as its default generator makes them: the same lists, the same
// order, the same rules. The vocabulary is pinned in third_party/coolname.
#include <QSet>
#include <QStringList>
#include <memory>
namespace bm {
class CoolName {
  public:
    // coolname's default vocabulary, less these words and the phrases using them.
    explicit CoolName(const QSet<QString> &excluded = {});
    ~CoolName();
    // How many names there are of this many words (2, 3 or 4), rules aside.
    quint64 combinations(int words) const;
    // The index-th of them (index < combinations(words)), as its words; the
    // same name upstream has at that index.
    QStringList name(int words, quint64 index) const;
    // coolname's rules: no word twice, no two words starting with the same four
    // letters, at most 50 characters as a slug.
    bool acceptable(const QStringList &name) const;

  private:
    struct Tree;
    std::shared_ptr<const Tree> tree_;
};
// The words ynotbit leaves out of names (third_party/coolname/excluded.txt).
QSet<QString> excludedWords();
// An address's name: three words, the same for the same address in every
// ynotbit. A leading "BM-" (any case) and surrounding spaces do not count; an
// empty address has none.
QStringList addressName(const QString &address);
// The same, with another list of excluded words.
QStringList addressName(const QString &address, const QSet<QString> &excluded);
} // namespace bm
