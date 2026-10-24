// The vault: the identities and mailbox keys of one user, in one encrypted file. The bytes are
// Qt's (a QDataStream at Qt_6_0 inside XChaCha20-Poly1305), so the previous release opens what this
// writes and the other way round. bmc/qtstream writes and reads that payload.
#pragma once
#include <bmc/records.h>
#include <bmc/secret.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bmc {
namespace pybm {
struct KeyEntry;
}
// What became of one identity of another client's keys file.
struct IdentityImport {
    enum class Result { Added, AlreadyThere, Unusable };
    enum class Why { None, MissingKey, BadKey, WrongKeys, Unsupported, Full };
    std::string address, label; // the label as the vault keeps it (a chan is named as a chan)
    bool chan = false, wasDisabled = false, mailingList = false;
    Result result = Result::Unusable;
    Why why = Why::None;
};

class Vault {
    std::string path_;
    Secret wrapping_;
    Bytes salt_;
    bool unlocked_ = false;
    std::vector<Identity> identities_;
    std::map<std::string, Secret> mailboxKeys_;
    void save();

  public:
    ~Vault();
    Vault() = default;
    Vault(Vault &&) = default;
    Vault &operator=(Vault &&) = default;

    void create(const std::string &path, const std::string &password);
    void unlock(const std::string &path, const std::string &password);
    void changePassword(const std::string &password);
    void lock();
    bool unlocked() const {
        return unlocked_;
    }
    const std::vector<Identity> &identities() const {
        return identities_;
    }
    std::string addIdentity(const std::string &label);
    // An identity made beforehand (the new vault dialog's): its keys are checked against its
    // address and kept as they are.
    std::string addIdentity(const Identity &identity);
    // label empty: "#<phrase>"; any other label is made a channelName().
    std::string addChannel(const std::string &phrase, const std::string &label,
                           const std::string &expectedAddress = {});
    void importKeys(const std::string &path);
    // The identities and chans of another client's keys file (pybm::readKeys), saved once. One that
    // cannot be used is reported and the rest go in; one the vault has is left as it is; one that
    // was switched off over there comes in all the same. If the save fails nothing is added.
    std::vector<IdentityImport> importEntries(const std::vector<pybm::KeyEntry> &entries);
    std::string addMailboxKey();
    void renameIdentity(const std::string &address, const std::string &label);
    void deleteIdentity(const std::string &address);
    void setDefaultIdentity(const std::string &address);
    std::vector<std::string> mailboxIds() const;
    const Secret &mailboxKey(const std::string &id) const;
    std::string path() const {
        return path_;
    }
};
} // namespace bmc
