#include <algorithm>
#include <bmc/path.h>
#include <bmc/protocol.h>
#include <bmc/pybitmessage.h>
#include <bmc/qtstream.h>
#include <bmc/random.h>
#include <bmc/text.h>
#include <bmc/vault.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <sodium.h>
extern "C" {
#include "config.h"
#include "ntb-address.h"
#include "ntb-base58.h"
}

namespace bmc {
namespace {
Bytes readFile(const std::filesystem::path &p) {
    std::FILE *f = openFile(p, "rb");
    if (!f)
        throw Error(Code::Io, "Cannot open vault");
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    Bytes out(size > 0 ? size_t(size) : size_t(0));
    const bool read = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!read)
        throw Error(Code::Io, "Cannot open vault");
    return out;
}
Secret derive(const std::string &password, ByteView salt) {
    require(!password.empty(), "Enter a vault password");
    Secret key(32);
    require(crypto_pwhash(key.data(), 32, password.data(), password.size(), salt.data(), 3,
                          64 * 1024 * 1024, crypto_pwhash_ALG_ARGON2ID13) == 0,
            "Insufficient memory to unlock vault");
    return key;
}
// A secret as text for one call, wiped afterwards.
struct WipedText {
    std::string value;
    explicit WipedText(const std::string &text) : value(text) {}
    ~WipedText() {
        if (!value.empty())
            sodium_memzero(value.data(), value.size());
    }
};
} // namespace

Vault::~Vault() {
    lock();
}

void Vault::create(const std::string &path, const std::string &password) {
    require(!unlocked_, "Lock the current vault first");
    require(!password.empty(), "Enter a vault password");
    const auto target = bmc::path(path);
    reserveFile(target);
    try {
        path_ = path;
        salt_.assign(16, 0);
        randomBytes(salt_.data(), 16);
        wrapping_ = derive(password, salt_);
        unlocked_ = true;
        save();
    } catch (...) {
        lock();
        std::filesystem::remove(target);
        throw;
    }
}

void Vault::save() {
    require(unlocked_, "Vault is locked");
    require(identities_.size() <= 1024 && mailboxKeys_.size() <= 1024,
            "Vault capacity exceeded; existing vault preserved");
    qtstream out;
    out << uint32_t(identities_.size());
    for (const auto &i : identities_) {
        out << i.label << i.address << i.chan << i.isDefault;
        out.writeRaw(ByteView(i.keys.data(), 64));
    }
    out << uint32_t(mailboxKeys_.size());
    for (const auto &[id, key] : mailboxKeys_) {
        out << id;
        out.writeRaw(ByteView(key.data(), 32));
    }
    require(out.ok(), "Cannot serialize vault");
    const auto &plain = out.bytes();
    require(plain.size() + 64 <= 4 * 1024 * 1024, "Vault is too large; existing vault preserved");
    Bytes header = bytesOf("BMVAULT2");
    append(header, salt_);
    Bytes nonce(24, 0);
    randomBytes(nonce.data(), nonce.size());
    append(header, nonce);
    Bytes cipher(plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, 0);
    unsigned long long n = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(cipher.data(), &n, plain.data(), plain.size(),
                                               header.data(), header.size(), nullptr, nonce.data(),
                                               wrapping_.data());
    Bytes file = header;
    append(file, cipher);
    writeFileAtomic(bmc::path(path_), file);
}

void Vault::unlock(const std::string &path, const std::string &password) {
    require(!unlocked_, "Lock the current vault first");
    const auto filePath = bmc::path(path);
    // The size is checked before the file is read: a vault that is not one must not be pulled into
    // memory in full and refused afterwards. A file that cannot even be measured is left to
    // readFile, which says that it cannot be opened.
    std::error_code ec;
    const auto measured = std::filesystem::file_size(filePath, ec);
    if (!ec)
        require(measured >= 64 && measured <= 4 * 1024 * 1024, "Invalid vault size");
    const auto file = readFile(filePath);
    require(file.size() >= 64 && file.size() <= 4 * 1024 * 1024, "Invalid vault size");
    const std::string magic = stringOf(ByteView(file.data(), 8));
    require(magic == "BMVAULT1" || magic == "BMVAULT2", "Unsupported vault format");
    const bool legacy = magic == "BMVAULT1";
    const ByteView salt(file.data() + 8, 16);
    auto key = derive(password, salt);
    Bytes plain(file.size() - 64, 0);
    unsigned long long n = 0;
    require(crypto_aead_xchacha20poly1305_ietf_decrypt(plain.data(), &n, nullptr, file.data() + 48,
                                                       file.size() - 48, file.data(), 48,
                                                       file.data() + 24, key.data()) == 0,
            "Wrong password or damaged vault");
    qtstream in(std::move(plain));
    uint32_t count = 0;
    in >> count;
    require(in.ok() && count <= 1024, "Too many vault identities");
    std::vector<Identity> identities;
    for (uint32_t j = 0; j < count; ++j) {
        Identity i;
        in >> i.label >> i.address >> i.chan;
        if (!legacy)
            in >> i.isDefault;
        const auto keys = in.readRaw(64);
        require(keys.size() == 64, "Truncated vault");
        std::memcpy(i.keys.data(), keys.data(), 64);
        identities.push_back(std::move(i));
    }
    if (legacy && !identities.empty())
        identities.front().isDefault = true;
    in >> count;
    require(in.ok() && count <= 1024, "Too many mailbox keys");
    std::map<std::string, Secret> keys;
    for (uint32_t j = 0; j < count; ++j) {
        std::string id;
        in >> id;
        Secret k(32);
        const auto raw = in.readRaw(32);
        require(raw.size() == 32, "Truncated vault key");
        std::memcpy(k.data(), raw.data(), 32);
        require(keys.emplace(id, std::move(k)).second, "Duplicate mailbox key");
    }
    require(in.ok() && in.atEnd(), "Invalid vault contents");
    path_ = path;
    salt_.assign(salt.begin(), salt.end());
    wrapping_ = std::move(key);
    identities_ = std::move(identities);
    mailboxKeys_ = std::move(keys);
    unlocked_ = true;
    // Chans were named "[chan] <phrase>" or "<name>" before names took a '#'.
    bool renamed = false;
    for (auto &i : identities_)
        if (i.chan && channelName(i.label) != i.label) {
            i.label = channelName(i.label);
            renamed = true;
        }
    if (renamed) {
        try {
            save();
        } catch (...) {
            // A read-only vault still opens; the names are fixed in memory.
        }
    }
}

void Vault::lock() {
    identities_.clear();
    mailboxKeys_.clear();
    wrapping_.clear();
    salt_.clear();
    path_.clear();
    unlocked_ = false;
}

void Vault::changePassword(const std::string &password) {
    require(unlocked_, "Vault is locked");
    auto oldSalt = salt_;
    Secret oldKey = std::move(wrapping_);
    try {
        salt_.assign(16, 0);
        randomBytes(salt_.data(), 16);
        wrapping_ = derive(password, salt_);
        save();
    } catch (...) {
        salt_ = std::move(oldSalt);
        wrapping_ = std::move(oldKey);
        throw;
    }
}

std::string Vault::addIdentity(const std::string &label) {
    require(unlocked_, "Vault is locked");
    identities_.push_back(Protocol::identity(label));
    if (identities_.size() == 1)
        identities_.back().isDefault = true;
    try {
        save();
    } catch (...) {
        identities_.pop_back();
        throw;
    }
    return identities_.back().address;
}

std::string Vault::addIdentity(const Identity &identity) {
    require(unlocked_, "Vault is locked");
    auto copy = Protocol::fromPrivate(identity.label, ByteView(identity.keys.data(), 32),
                                      ByteView(identity.keys.data() + 32, 32), 4, 1, false);
    require(copy.address == identity.address, "Identity keys do not match its address");
    for (const auto &i : identities_)
        require(i.address != copy.address, "Identity already in the vault");
    identities_.push_back(std::move(copy));
    if (identities_.size() == 1)
        identities_.back().isDefault = true;
    try {
        save();
    } catch (...) {
        identities_.pop_back();
        throw;
    }
    return identities_.back().address;
}

std::string Vault::addChannel(const std::string &phrase, const std::string &label,
                              const std::string &expectedAddress) {
    require(unlocked_, "Vault is locked");
    int version = 4;
    if (!expectedAddress.empty()) {
        ntb_address a;
        require(ntb_address_decode(&a, expectedAddress.c_str()) && a.stream == 1,
                "Invalid expected chan address");
        version = a.version;
    }
    // An empty label names the chan after its phrase.
    WipedText secret(phrase);
    auto channel =
        Protocol::channel(secret.value, channelName(label.empty() ? phrase : label), version);
    require(expectedAddress.empty() || channel.address == expectedAddress,
            "The shared phrase does not match that chan address");
    for (const auto &i : identities_)
        require(i.address != channel.address, "Channel already joined");
    identities_.push_back(std::move(channel));
    if (identities_.size() == 1)
        identities_.back().isDefault = true;
    try {
        save();
    } catch (...) {
        identities_.pop_back();
        throw;
    }
    return identities_.back().address;
}

void Vault::importKeys(const std::string &path) {
    require(unlocked_, "Vault is locked");
    std::FILE *f = openFile(bmc::path(path), "rb");
    require(f != nullptr, "Cannot read keys file");
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    require(size >= 0 && size <= 1024 * 1024, "Cannot read keys file");
    WipedText content{std::string{}};
    content.value.resize(size_t(size));
    const bool read =
        content.value.empty() ||
        std::fread(content.value.data(), 1, content.value.size(), f) == content.value.size();
    std::fclose(f);
    require(read, "Cannot read keys file");
    std::string_view remaining(content.value);
    std::string section, label;
    bool chan = false;
    Secret signing, encryption;
    std::vector<Identity> imported;
    auto flush = [&] {
        if (section.rfind("BM-", 0) != 0 || signing.size() != 32 || encryption.size() != 32)
            return;
        ntb_address a;
        require(ntb_address_decode(&a, section.c_str()), "Invalid imported address");
        auto identity =
            Protocol::fromPrivate(label.empty() ? section : label, ByteView(signing.data(), 32),
                                  ByteView(encryption.data(), 32), a.version, a.stream, chan);
        require(identity.address == section, "Imported keys do not match their address");
        for (const auto &existing : identities_)
            if (existing.address == identity.address)
                return;
        for (const auto &existing : imported)
            if (existing.address == identity.address)
                return;
        imported.push_back(std::move(identity));
    };
    while (!remaining.empty()) {
        const auto end = remaining.find('\n');
        auto line =
            text::trim(remaining.substr(0, end == std::string_view::npos ? remaining.size() : end));
        remaining = end == std::string_view::npos ? std::string_view() : remaining.substr(end + 1);
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            flush();
            section = std::string(line.substr(1, line.size() - 2));
            label.clear();
            chan = false;
            signing.clear();
            encryption.clear();
        } else {
            const auto equal = line.find('=');
            if (equal == std::string_view::npos || equal < 1)
                continue;
            const auto field = text::trim(line.substr(0, equal));
            const auto value = text::trim(line.substr(equal + 1));
            if (field == "privsigningkey")
                signing = pybm::decodeWif(value);
            else if (field == "privencryptionkey")
                encryption = pybm::decodeWif(value);
            else if (field == "label")
                label = std::string(value);
            else if (field == "chan")
                chan = value == "true";
        }
    }
    flush();
    require(!imported.empty(), "No new private identities found in this file");
    const auto count = identities_.size();
    for (auto &i : imported) {
        if (i.chan)
            i.label = channelName(i.label);
        identities_.push_back(std::move(i));
    }
    try {
        save();
    } catch (...) {
        identities_.resize(count);
        throw;
    }
}

std::string Vault::addMailboxKey() {
    require(unlocked_, "Vault is locked");
    auto id = uuid();
    Secret key(32);
    randomBytes(key.data(), 32);
    mailboxKeys_.emplace(id, std::move(key));
    try {
        save();
    } catch (...) {
        mailboxKeys_.erase(id);
        throw;
    }
    return id;
}

const Secret &Vault::mailboxKey(const std::string &id) const {
    require(unlocked_, "Vault is locked");
    const auto it = mailboxKeys_.find(id);
    require(it != mailboxKeys_.end(), "Mailbox key is not in this vault");
    return it->second;
}

std::vector<std::string> Vault::mailboxIds() const {
    std::vector<std::string> ids;
    for (const auto &[id, key] : mailboxKeys_)
        ids.push_back(id);
    return ids;
}

void Vault::renameIdentity(const std::string &address, const std::string &label) {
    require(unlocked_, "Vault is locked");
    require(label.size() <= 256, "Identity label is too long");
    for (auto &i : identities_)
        if (i.address == address) {
            auto old = i.label;
            i.label = i.chan ? channelName(label) : label;
            try {
                save();
            } catch (...) {
                i.label = old;
                throw;
            }
            return;
        }
    throw Error(Code::Invalid, "Identity not found");
}

void Vault::deleteIdentity(const std::string &address) {
    require(unlocked_, "Vault is locked");
    const auto it = std::find_if(identities_.begin(), identities_.end(),
                                 [&](const Identity &i) { return i.address == address; });
    require(it != identities_.end(), "Identity not found");
    auto removed = std::move(*it);
    const auto index = it - identities_.begin();
    identities_.erase(it);
    const auto hadDefault = removed.isDefault;
    if (hadDefault && !identities_.empty())
        identities_.front().isDefault = true;
    try {
        save();
    } catch (...) {
        identities_.insert(identities_.begin() + index, std::move(removed));
        if (hadDefault && identities_.size() > 1)
            identities_[1].isDefault = false;
        throw;
    }
}

void Vault::setDefaultIdentity(const std::string &address) {
    require(unlocked_, "Vault is locked");
    require(std::any_of(identities_.begin(), identities_.end(),
                        [&](const Identity &i) { return i.address == address; }),
            "Identity not found");
    std::vector<bool> previous;
    for (auto &i : identities_) {
        previous.push_back(i.isDefault);
        i.isDefault = (i.address == address);
    }
    try {
        save();
    } catch (...) {
        for (size_t j = 0; j < identities_.size(); ++j)
            identities_[j].isDefault = previous[j];
        throw;
    }
}

std::vector<IdentityImport> Vault::importEntries(const std::vector<pybm::KeyEntry> &entries) {
    require(unlocked_, "Vault is locked");
    using Result = IdentityImport::Result;
    using Why = IdentityImport::Why;
    std::vector<IdentityImport> items;
    const auto before = identities_.size();
    for (const auto &e : entries) {
        IdentityImport item;
        item.address = e.address;
        item.label = e.label;
        item.chan = e.chan;
        item.wasDisabled = !e.enabled;
        item.mailingList = e.mailingList;
        const auto refuse = [&](Why why) {
            item.result = Result::Unusable;
            item.why = why;
            items.push_back(item);
        };
        if (e.problem == pybm::KeyProblem::MissingKey)
            refuse(Why::MissingKey);
        else if (e.problem == pybm::KeyProblem::BadKey)
            refuse(Why::BadKey);
        else if (ntb_address a; !ntb_address_decode(&a, e.address.c_str()))
            refuse(Why::Unsupported);
        else if (std::any_of(identities_.begin(), identities_.end(),
                             [&](const Identity &i) { return i.address == e.address; })) {
            item.result = Result::AlreadyThere;
            items.push_back(item);
        } else if (identities_.size() >= 1024)
            refuse(Why::Full);
        else {
            try {
                auto identity = Protocol::fromPrivate(
                    e.label.empty() ? e.address : e.label, ByteView(e.signing.data(), 32),
                    ByteView(e.encryption.data(), 32), a.version, a.stream, e.chan);
                if (identity.address != e.address) {
                    refuse(Why::WrongKeys);
                    continue;
                }
                if (identity.chan)
                    identity.label = channelName(identity.label);
                item.label = identity.label;
                identities_.push_back(std::move(identity));
                item.result = Result::Added;
                items.push_back(item);
            } catch (const std::exception &) {
                refuse(Why::Unsupported);
            }
        }
    }
    if (identities_.size() == before)
        return items;
    if (before == 0)
        identities_.front().isDefault = true; // the first of an empty vault is its default
    try {
        save();
    } catch (...) {
        identities_.resize(before);
        throw;
    }
    return items;
}
} // namespace bmc
