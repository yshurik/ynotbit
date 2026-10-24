// Data folders made the way PyBitmessage leaves them: its messages.dat schema (version 11, as in
// src/sql/initialize_schema.sql) and a keys.dat. The binary columns hold their bytes as TEXT, as a
// real messages.dat does (msgid, sighash, ackdata, toripe: typeof() says text, 32 bytes long).
#pragma once
#include <bmc/bytes.h>
#include <bmc/hash.h>
#include <bmc/protocol.h>
#include <bmc/test.h>
#include <cstdint>
#include <sqlcipher/sqlite3.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace pybmfix {
inline constexpr const char *kSchema = R"SQL(
CREATE TABLE `inbox` (`msgid` blob, `toaddress` text, `fromaddress` text, `subject` text,
   `received` text, `message` text, `folder` text, `encodingtype` int, `read` bool, `sighash` blob,
   UNIQUE(msgid) ON CONFLICT REPLACE);
CREATE TABLE `sent` (`msgid` blob, `toaddress` text, `toripe` blob, `fromaddress` text,
   `subject` text, `message` text, `ackdata` blob, `senttime` integer, `lastactiontime` integer,
   `sleeptill` integer, `status` text, `retrynumber` integer, `folder` text, `encodingtype` int,
   `ttl` int);
CREATE TABLE `subscriptions` (`label` text, `address` text, `enabled` bool);
CREATE TABLE `addressbook` (`label` text, `address` text, UNIQUE(address) ON CONFLICT IGNORE);
CREATE TABLE `blacklist` (`label` text, `address` text, `enabled` bool);
CREATE TABLE `whitelist` (`label` text, `address` text, `enabled` bool);
CREATE TABLE `pubkeys` (`address` text, `addressversion` int, `transmitdata` blob, `time` int,
   `usedpersonally` text, UNIQUE(address) ON CONFLICT REPLACE);
CREATE TABLE `inventory` (`hash` blob, `objecttype` int, `streamnumber` int, `payload` blob,
   `expirestime` integer, `tag` blob, UNIQUE(hash) ON CONFLICT REPLACE);
INSERT INTO subscriptions VALUES ('Bitmessage new releases/announcements',
   'BM-GtovgYdgs7qXPkoYaRgrLFuFKz1SFpsw', 1);
CREATE TABLE `settings` (`key` blob, `value` blob, UNIQUE(key) ON CONFLICT REPLACE);
INSERT INTO settings VALUES('version','11');
CREATE TABLE `objectprocessorqueue` (`objecttype` int, `data` blob,
   UNIQUE(objecttype, data) ON CONFLICT REPLACE);
)SQL";

// 32 bytes that are not text: every value from 0 to 255 appears over a few of them, NUL included.
inline bmc::Bytes rawHash(int seed) {
    bmc::Bytes b(32);
    for (int i = 0; i < 32; ++i)
        b[size_t(i)] = uint8_t((seed * 37 + i * 11) & 0xff);
    b[0] = uint8_t(seed); // distinct per seed, and a zero for seed 0
    return b;
}

// A messages.dat in a fresh folder. Everything goes in the way PyBitmessage's own inserts do.
class Folder {
  public:
    std::string dir, messages;
    sqlite3 *db = nullptr;

    explicit Folder(bool withSchema = true)
        : dir(bmc::test::tempDir()), messages(dir + "/messages.dat") {
        if (sqlite3_open_v2(messages.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot make messages.dat");
        if (withSchema)
            exec(kSchema);
    }
    ~Folder() {
        close();
    }
    // For a test that removes the file: Windows will not delete one that is open.
    void close() {
        sqlite3_close(db);
        db = nullptr;
    }
    Folder(const Folder &) = delete;
    Folder &operator=(const Folder &) = delete;

    void exec(const std::string &sql) {
        char *err = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string what = err ? err : "sql failed";
            sqlite3_free(err);
            throw std::runtime_error(what);
        }
    }
    void keys(const std::string &text) const {
        bmc::test::writeFile(dir + "/keys.dat", bmc::view(text));
    }

    // inbox: (msgid, toaddress, fromaddress, subject, received, message, folder, encodingtype,
    // read, sighash). `received` is text, as PyBitmessage's int(time.time()) lands in the column.
    void inbox(const bmc::Bytes &msgid, const std::string &to, const std::string &from,
               const std::string &subject, const std::string &received, const std::string &body,
               const std::string &folder = "inbox", bool read = false,
               const bmc::Bytes &sighash = rawHash(200)) {
        sqlite3_stmt *s = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO inbox VALUES (?,?,?,?,?,?,?,?,?,?)", -1, &s, nullptr);
        bindText(s, 1, std::string(msgid.begin(), msgid.end()));
        bindText(s, 2, to);
        bindText(s, 3, from);
        bindText(s, 4, subject);
        bindText(s, 5, received);
        bindText(s, 6, body);
        bindText(s, 7, folder);
        sqlite3_bind_int(s, 8, 2);
        sqlite3_bind_int(s, 9, read ? 1 : 0);
        bindText(s, 10, std::string(sighash.begin(), sighash.end()));
        step(s);
    }

    // sent: (msgid, toaddress, toripe, fromaddress, subject, message, ackdata, senttime,
    // lastactiontime, sleeptill, status, retrynumber, folder, encodingtype, ttl)
    void sent(const bmc::Bytes &msgid, const std::string &to, const std::string &from,
              const std::string &subject, const std::string &body, const bmc::Bytes &ackdata,
              int64_t senttime, const std::string &status, const std::string &folder = "sent") {
        sqlite3_stmt *s = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO sent VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &s,
                           nullptr);
        bindText(s, 1, std::string(msgid.begin(), msgid.end()));
        bindText(s, 2, to);
        bindText(s, 3, std::string(20, 'r'));
        bindText(s, 4, from);
        bindText(s, 5, subject);
        bindText(s, 6, body);
        bindText(s, 7, std::string(ackdata.begin(), ackdata.end()));
        sqlite3_bind_int64(s, 8, senttime);
        sqlite3_bind_int64(s, 9, senttime);
        sqlite3_bind_int64(s, 10, 0);
        bindText(s, 11, status);
        sqlite3_bind_int(s, 12, 0);
        bindText(s, 13, folder);
        sqlite3_bind_int(s, 14, 2);
        sqlite3_bind_int(s, 15, 345600);
        step(s);
    }

    void subscription(const std::string &label, const std::string &address, bool enabled = true) {
        exec("INSERT INTO subscriptions VALUES ('" + label + "','" + address + "'," +
             (enabled ? "1" : "0") + ")");
    }
    void contact(const std::string &label, const std::string &address) {
        exec("INSERT INTO addressbook VALUES ('" + label + "','" + address + "')");
    }

  private:
    static void bindText(sqlite3_stmt *s, int n, const std::string &t) {
        // The length is explicit: a NUL or a byte that is not UTF-8 goes in as it is.
        sqlite3_bind_text(s, n, t.data(), int(t.size()), SQLITE_TRANSIENT);
    }
    void step(sqlite3_stmt *s) {
        const int r = sqlite3_step(s);
        sqlite3_finalize(s);
        if (r != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
};

// A private key in the wallet import format keys.dat holds: 0x80, the 32 bytes, a four-byte
// checksum (double SHA-256), in base58.
inline std::string wifOf(const unsigned char *key) {
    bmc::Bytes payload{0x80};
    payload.insert(payload.end(), key, key + 32);
    const auto sum = bmc::hash(bmc::Digest::Sha256, bmc::hash(bmc::Digest::Sha256, payload));
    payload.insert(payload.end(), sum.begin(), sum.begin() + 4);
    static const char *alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    std::string digits;
    bmc::Bytes number = payload;
    size_t zeros = 0;
    while (zeros < number.size() && number[zeros] == 0)
        ++zeros;
    while (true) {
        // Divide the big-endian number by 58; the remainder is the next digit.
        int remainder = 0;
        bool allZero = true;
        for (auto &b : number) {
            const int value = remainder * 256 + b;
            b = uint8_t(value / 58);
            remainder = value % 58;
            allZero = allZero && b == 0;
        }
        digits.push_back(alphabet[remainder]);
        if (allZero)
            break;
    }
    digits.append(zeros, '1');
    return std::string(digits.rbegin(), digits.rend());
}

// A keys.dat section for the identity, the way PyBitmessage's address generator writes it.
inline std::string section(const bmc::Identity &id, const std::string &label, bool chan = false,
                           bool enabled = true) {
    return "[" + id.address + "]\nlabel = " + label +
           "\nenabled = " + (enabled ? "true" : "false") + "\ndecoy = false\n" +
           (chan ? "chan = true\n" : "") +
           "noncetrialsperbyte = 1000\npayloadlengthextrabytes = 1000\n"
           "privsigningkey = " +
           wifOf(id.keys.data()) + "\nprivencryptionkey = " + wifOf(id.keys.data() + 32) + "\n\n";
}

inline const std::string kSettings =
    "[bitmessagesettings]\nsettingsversion = 10\nport = 8444\nkeysencrypted = False\n"
    "messagesencrypted = False\n\n";
} // namespace pybmfix
