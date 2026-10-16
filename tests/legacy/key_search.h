#pragma once
#include "storage.h"
#include <optional>

namespace legacy {
Identity phraseIdentity(const QString &phrase, const QString &label, quint64 pair);
std::optional<quint64> firstPair(const QString &phrase, int zeroBytes, quint64 limit = 1000000);
int zeroBytes(const QString &address);
} // namespace legacy
