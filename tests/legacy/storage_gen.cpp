// Makes the golden mailbox files with the SQL the Qt app's Mailbox wrote, character for
// character (src/storage.cpp's create and src/mailbox_ops.cpp's migrate), through a raw
// SQLCipher connection so the generator needs neither Qt nor the client.
//
// mailbox-v1.bmmail: the file Mailbox::create leaves before migrate() runs (user_version 1,
// no unread/previous_folder columns, no outbox or chat tables).
// mailbox-v2.bmmail: the file Mailbox::create leaves after migrate(), with letters, a draft,
// a chat, a subscription, a contact and a public key in it.
//
// The key is the fixed 32 bytes 0x00..0x1f (fixtures::goldenMailboxKey).
//
// Build: c++ -std=c++20 -DSQLITE_HAS_CODEC storage_gen.cpp $(pkg-config --cflags --libs sqlcipher)
// Usage: storage_gen <outdir>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sqlcipher/sqlite3.h>
#include <string>
#include <vector>

namespace {
[[noreturn]] void die(sqlite3 *db, const std::string &what) {
    std::fprintf(stderr, "%s: %s\n", what.c_str(), db ? sqlite3_errmsg(db) : "no handle");
    std::exit(1);
}
void exec(sqlite3 *db, const std::string &sql) {
    char *err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string message = err ? err : "";
        sqlite3_free(err);
        std::fprintf(stderr, "exec failed: %s\n%s\n", sql.c_str(), message.c_str());
        std::exit(1);
    }
}
struct Binder {
    sqlite3_stmt *s;
    int n = 1;
    Binder &text(const std::string &t) {
        sqlite3_bind_text(s, n++, t.data(), int(t.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Binder &number(int64_t i) {
        sqlite3_bind_int64(s, n++, i);
        return *this;
    }
    Binder &blob(const std::vector<uint8_t> &b) {
        sqlite3_bind_blob(s, n++, b.data(), int(b.size()), SQLITE_TRANSIENT);
        return *this;
    }
};
void run(sqlite3 *db, const char *sql, const std::function<void(Binder &)> &bind) {
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
        die(db, "prepare");
    Binder b{s};
    bind(b);
    if (sqlite3_step(s) != SQLITE_DONE)
        die(db, "step");
    sqlite3_finalize(s);
}
sqlite3 *open(const std::string &path, const std::vector<uint8_t> &key) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
        die(db, "open");
    sqlite3_busy_timeout(db, 3000);
    if (sqlite3_key(db, key.data(), int(key.size())) != SQLITE_OK)
        die(db, "key");
    exec(db, "PRAGMA cipher_memory_security=ON; PRAGMA temp_store=MEMORY; PRAGMA foreign_keys=ON;");
    return db;
}

// The v1 schema Mailbox::create writes (src/storage.cpp:160-164), then user_version=1.
const char *kV1Schema =
    "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; BEGIN IMMEDIATE; CREATE TABLE "
    "meta(id TEXT NOT NULL, checkpoint INTEGER NOT NULL, cache_id TEXT NOT NULL DEFAULT "
    "'', identities TEXT NOT NULL DEFAULT ''); CREATE TABLE messages(hash TEXT PRIMARY KEY, sender "
    "TEXT NOT NULL, recipient TEXT NOT NULL, subject TEXT NOT NULL, body TEXT NOT NULL, folder "
    "TEXT NOT NULL, received INTEGER NOT NULL); PRAGMA user_version=1;";
const char *kInsertMeta = "INSERT INTO meta(id,checkpoint) VALUES(?,0)";
// Mailbox::store (src/storage.cpp:216-219).
const char *kInsertLetter =
    "INSERT OR IGNORE INTO messages(hash,sender,recipient,subject,body,folder,received) "
    "VALUES(?,?,?,?,?,?,?)";

// The tables migrate() creates (src/mailbox_ops.cpp:44-77), in their order.
const char *kMigrateTables =
    "CREATE TABLE IF NOT EXISTS outbox(id TEXT PRIMARY KEY REFERENCES messages(hash) ON DELETE "
    "CASCADE,kind TEXT NOT NULL,state TEXT NOT NULL,error TEXT NOT NULL DEFAULT '',object_hash "
    "TEXT NOT NULL DEFAULT '',ack_token BLOB NOT NULL DEFAULT X'',ack_object BLOB NOT NULL "
    "DEFAULT X'',expires INTEGER NOT NULL,next_attempt INTEGER NOT NULL DEFAULT 0,attempts "
    "INTEGER NOT NULL DEFAULT 1);"
    "CREATE TABLE IF NOT EXISTS delivery_events(seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT "
    "NOT NULL REFERENCES messages(hash) ON DELETE CASCADE,ts INTEGER NOT NULL,state TEXT NOT "
    "NULL,detail TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS network_jobs(id TEXT PRIMARY KEY,owner TEXT NOT NULL,kind TEXT "
    "NOT NULL,address TEXT NOT NULL,state TEXT NOT NULL,hash TEXT NOT NULL,error TEXT NOT "
    "NULL,payload BLOB NOT NULL,expires INTEGER NOT NULL,trials INTEGER NOT NULL,extra INTEGER "
    "NOT NULL);"
    "CREATE TABLE IF NOT EXISTS public_keys(address TEXT PRIMARY KEY,signing BLOB NOT "
    "NULL,encryption BLOB NOT NULL,trials INTEGER NOT NULL,extra INTEGER NOT NULL,behaviors "
    "INTEGER NOT NULL,expires INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS subscriptions(address TEXT PRIMARY KEY,label TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS contacts(address TEXT PRIMARY KEY,label TEXT NOT NULL,added "
    "INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS settings(name TEXT PRIMARY KEY,value TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS delivery_events_id ON delivery_events(id,seq);";
const char *kMigrateChatTables =
    "CREATE TABLE IF NOT EXISTS chat_messages(id TEXT PRIMARY KEY,local_address TEXT NOT NULL,"
    "peer_address TEXT NOT NULL,direction TEXT NOT NULL CHECK(direction IN ('in','out')),"
    "seqno INTEGER NOT NULL,body TEXT NOT NULL,received INTEGER NOT NULL,expires INTEGER NOT NULL,"
    "state TEXT NOT NULL,unread INTEGER NOT NULL DEFAULT 0,"
    "UNIQUE(local_address,peer_address,direction,seqno));"
    "CREATE INDEX IF NOT EXISTS chat_conversation ON "
    "chat_messages(local_address,peer_address,received);"
    "CREATE INDEX IF NOT EXISTS chat_expiry ON chat_messages(expires);"
    "CREATE TABLE IF NOT EXISTS chat_sequences(local_address TEXT NOT NULL,peer_address TEXT NOT "
    "NULL,"
    "last_seq INTEGER NOT NULL,PRIMARY KEY(local_address,peer_address));"
    "CREATE TABLE IF NOT EXISTS chat_outbox(id TEXT PRIMARY KEY REFERENCES chat_messages(id) ON "
    "DELETE CASCADE,"
    "state TEXT NOT NULL,error TEXT NOT NULL DEFAULT '',object_hash TEXT NOT NULL DEFAULT '',"
    "ack_token BLOB NOT NULL DEFAULT X'',ack_object BLOB NOT NULL DEFAULT X'',"
    "expires INTEGER NOT NULL,next_attempt INTEGER NOT NULL DEFAULT 0,attempts INTEGER NOT NULL "
    "DEFAULT 1);";

void letters(sqlite3 *db) {
    run(db, kInsertLetter, [](Binder &b) {
        b.text("hash-welcome").text("BM-2cSenderOne").text("BM-2cRecipientOne").text("Welcome");
        b.text("The first letter.").text("Inbox").number(1700000000);
    });
    run(db, kInsertLetter, [](Binder &b) {
        b.text("hash-second")
            .text("BM-2cSenderTwo")
            .text("BM-2cRecipientOne")
            .text("Second letter");
        b.text("A letter with a bit more text in it.").text("Inbox").number(1700000100);
    });
    // Mailbox::saveDraft (src/mailbox_ops.cpp:100-104): the 8-column insert with unread=0.
    run(db,
        "INSERT INTO messages(hash,sender,recipient,subject,body,folder,received,unread) "
        "VALUES(?,?,?,?,?,'Drafts',?,0) ON CONFLICT(hash) DO UPDATE SET "
        "sender=excluded.sender,recipient=excluded.recipient,subject=excluded.subject,body="
        "excluded.body,received=excluded.received",
        [](Binder &b) {
            b.text("draft-golden").text("BM-2cRecipientOne").text("BM-2cSenderOne").text("A draft");
            b.text("Not sent yet.").number(1700000200);
        });
    // A letter in a folder that was not Inbox: migrate() clears its unread.
    run(db, kInsertLetter, [](Binder &b) {
        b.text("hash-archived").text("BM-2cSenderOne").text("BM-2cRecipientOne").text("Old news");
        b.text("Put away.").text("Archive").number(1699999000);
    });
}
void chats(sqlite3 *db, int64_t now) {
    // Mailbox::storeChat (src/chat_store.cpp:65-69), an incoming chat.
    run(db,
        "INSERT OR IGNORE INTO chat_messages(id,local_address,peer_address,direction,seqno,body,"
        "received,expires,state,unread) VALUES(?,?,?, ?,?,?,?,?,'received',?)",
        [now](Binder &b) {
            b.text("chat-golden-in").text("BM-2cRecipientOne").text("BM-2cSenderTwo").text("in");
            b.number(1700000300000).text("hello from the peer").number(1700000300);
            b.number(now).number(1);
        });
    // Mailbox::queueChat (src/chat_store.cpp:46-54), an outgoing one.
    run(db,
        "INSERT INTO chat_sequences(local_address,peer_address,last_seq) VALUES(?,?,?) "
        "ON CONFLICT(local_address,peer_address) DO UPDATE SET last_seq=excluded.last_seq",
        [](Binder &b) {
            b.text("BM-2cRecipientOne").text("BM-2cSenderTwo").number(1700000300001);
        });
    run(db,
        "INSERT INTO chat_messages(id,local_address,peer_address,direction,seqno,body,received,"
        "expires,state) VALUES(?,?,?,'out',?,?,?,?,'queued')",
        [](Binder &b) {
            b.text("chat-golden-out").text("BM-2cRecipientOne").text("BM-2cSenderTwo");
            b.number(1700000300001).text("hi there").number(1700000400).number(1700000400);
        });
    run(db, "INSERT INTO chat_outbox(id,state,expires) VALUES(?,'queued',?)",
        [](Binder &b) { b.text("chat-golden-out").number(1700000400); });
}
void theRest(sqlite3 *db) {
    // Mailbox::subscribe (src/mailbox_ops.cpp:437-438).
    run(db,
        "INSERT INTO subscriptions VALUES(?,?) ON CONFLICT(address) DO UPDATE SET "
        "label=excluded.label",
        [](Binder &b) { b.text("BM-2cBroadcaster").text("Release notes"); });
    // Mailbox::saveContact (src/mailbox_ops.cpp:460-461).
    run(db,
        "INSERT INTO contacts VALUES(?,?,?) ON CONFLICT(address) DO UPDATE SET "
        "label=excluded.label",
        [](Binder &b) { b.text("BM-2cSenderOne").text("Ada").number(1700000000); });
    // Mailbox::savePublicKey (src/mailbox_ops.cpp:399-402).
    const std::vector<uint8_t> signing(65, 0x02), encryption(65, 0x03);
    run(db,
        "INSERT INTO public_keys VALUES(?,?,?,?,?,?,?) ON CONFLICT(address) DO UPDATE SET "
        "signing=excluded.signing,encryption=excluded.encryption,trials=excluded.trials,"
        "extra=excluded.extra,behaviors=excluded.behaviors,expires=excluded.expires",
        [&](Binder &b) {
            b.text("BM-2cSenderTwo").blob(signing).blob(encryption).number(1000).number(1000);
            b.number(1).number(2000000000);
        });
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <outdir>\n", argv[0]);
        return 1;
    }
    const std::string dir = argv[1];
    std::vector<uint8_t> key(32);
    for (int i = 0; i < 32; ++i)
        key[i] = uint8_t(i);
    const int64_t now = 1700000500;

    {
        // v1: the create() schema and a letter, then user_version stays 1.
        sqlite3 *db = open(dir + "/mailbox-v1.bmmail", key);
        exec(db, kV1Schema);
        run(db, kInsertMeta, [](Binder &b) { b.text("key-1"); });
        run(db, kInsertLetter, [](Binder &b) {
            b.text("hash-welcome").text("BM-2cSenderOne").text("BM-2cRecipientOne").text("Welcome");
            b.text("The first letter.").text("Inbox").number(1700000000);
        });
        exec(db, "COMMIT");
        sqlite3_close(db);
    }
    {
        // v2: the create() schema, migrate() in its order, then the rows.
        sqlite3 *db = open(dir + "/mailbox-v2.bmmail", key);
        exec(db, kV1Schema);
        run(db, kInsertMeta, [](Binder &b) { b.text("key-1"); });
        exec(db, "COMMIT");
        exec(db, "BEGIN IMMEDIATE");
        exec(db,
             "CREATE INDEX IF NOT EXISTS messages_folder_received ON messages(folder,received)");
        exec(db, "CREATE INDEX IF NOT EXISTS messages_channel_received ON "
                 "messages(folder,recipient,received)");
        exec(db, "ALTER TABLE messages ADD COLUMN unread INTEGER NOT NULL DEFAULT 1");
        exec(db,
             "UPDATE messages SET unread=0 WHERE folder NOT IN ('Inbox','Channels','Broadcasts')");
        exec(db, "ALTER TABLE messages ADD COLUMN previous_folder TEXT NOT NULL DEFAULT 'Inbox'");
        exec(db, kMigrateTables);
        exec(db, kMigrateChatTables);
        exec(db, "CREATE INDEX IF NOT EXISTS chat_unread ON chat_messages(received) "
                 "WHERE direction='in' AND unread=1");
        exec(db, "PRAGMA user_version=2");
        run(db, "UPDATE meta SET checkpoint=?", [](Binder &b) { b.number(42); });
        letters(db);
        chats(db, now);
        theRest(db);
        exec(db, "COMMIT");
        sqlite3_close(db);
    }
    std::printf("wrote golden mailboxes in %s\n", dir.c_str());
    return 0;
}
