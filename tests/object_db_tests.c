#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlcipher/sqlite3.h>
#ifdef _WIN32
#include <direct.h>
#define make_directory(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_directory(path) mkdir(path, 0700)
#endif
#include "ntb-object-db.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* nonce (8 bytes), expiry (8, big-endian), object type (4), then filler */
static void
make_object(uint8_t *object, size_t size, int64_t expires, uint32_t type, uint8_t fill)
{
        int i;

        memset(object, fill, size);
        for (i = 0; i < 8; i++)
                object[8 + i] = (uint8_t) ((uint64_t) expires >> (56 - 8 * i));
        for (i = 0; i < 4; i++)
                object[16 + i] = (uint8_t) (type >> (24 - 8 * i));
}

static void
make_hash(uint8_t *hash, int n)
{
        memset(hash, 0, NTB_OBJECT_DB_HASH_SIZE);
        hash[0] = (uint8_t) n;
        hash[1] = (uint8_t) (n >> 8);
}

struct capture {
        int calls;
        int64_t expires;
        size_t size;
        uint8_t first;
};

static void
capture_cb(const uint8_t *hash, int64_t expires, const uint8_t *data, size_t size, void *user_data)
{
        struct capture *c = user_data;

        c->calls++;
        c->expires = expires;
        c->size = size;
        c->first = data ? data[0] : 0;
}

static int64_t
query_int(const char *path, const char *sql)
{
        sqlite3 *db = NULL;
        sqlite3_stmt *stmt;
        int64_t value = -1;

        if (sqlite3_open(path, &db) == SQLITE_OK &&
            sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW)
                        value = sqlite3_column_int64(stmt, 0);
                sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
        return value;
}

static void
query_text(const char *path, const char *sql, char *out, size_t out_size)
{
        sqlite3 *db = NULL;
        sqlite3_stmt *stmt;

        out[0] = '\0';
        if (sqlite3_open(path, &db) == SQLITE_OK &&
            sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW)
                        snprintf(out, out_size, "%s", (const char *) sqlite3_column_text(stmt, 0));
                sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
}

int
main(int argc, char **argv)
{
        char path[4096], legacy[4096], legacy_file[4096], id_before[64], id_after[64];
        uint8_t object[4000], hash[NTB_OBJECT_DB_HASH_SIZE];
        const int64_t now = 2000000000;
        struct ntb_object_db *db;
        struct capture c;
        int64_t count, bytes, limit, last_seq, pages_before;
        FILE *file;
        int n;

        if (argc < 2)
                return 2;
        snprintf(path, sizeof path, "%s/" NTB_OBJECT_DB_FILE, argv[1]);
        make_directory(argv[1]);
        remove(path);
        snprintf(legacy, sizeof legacy, "%s-wal", path);
        remove(legacy);
        snprintf(legacy, sizeof legacy, "%s-shm", path);
        remove(legacy);

        /* A leftover folder from the file-per-object store goes. */
        snprintf(legacy, sizeof legacy, "%s/objects", argv[1]);
        make_directory(legacy);
        snprintf(legacy_file, sizeof legacy_file, "%s/objects/0123abcd", argv[1]);
        file = fopen(legacy_file, "wb");
        CHECK(file != NULL);
        fputs("old", file);
        fclose(file);

        db = ntb_object_db_open(argv[1]);
        CHECK(db != NULL);
        file = fopen(legacy_file, "rb");
        if (file)
                fclose(file);
        CHECK(file == NULL);

        /* Save, then load the same bytes back; a duplicate is ignored. */
        make_object(object, sizeof object, now + 3600, 2, 7);
        make_hash(hash, 1);
        CHECK(ntb_object_db_save(db, hash, object, sizeof object, now));
        CHECK(ntb_object_db_save(db, hash, object, sizeof object, now));
        ntb_object_db_stats(db, &count, &bytes);
        CHECK(count == 1 && bytes == (int64_t) sizeof object);
        memset(&c, 0, sizeof c);
        CHECK(ntb_object_db_load(db, hash, capture_cb, &c));
        CHECK(c.calls == 1 && c.size == sizeof object && c.first == 7 && c.expires == now + 3600);
        make_hash(hash, 999);
        CHECK(!ntb_object_db_load(db, hash, capture_cb, &c));

        /* The startup listing leaves out long-expired objects. */
        make_object(object, sizeof object, now - 7 * 3600, 2, 8);
        make_hash(hash, 2);
        CHECK(ntb_object_db_save(db, hash, object, sizeof object, now));
        memset(&c, 0, sizeof c);
        ntb_object_db_for_each(db, now - 6 * 3600, false, capture_cb, &c);
        CHECK(c.calls == 1 && c.size == 0);
        memset(&c, 0, sizeof c);
        ntb_object_db_for_each(db, now - 6 * 3600, true, capture_cb, &c);
        CHECK(c.calls == 1 && c.size == sizeof object && c.first == 7);

        /* Over the size limit: oldest first, down to 95% of the limit. */
        for (n = 10; n < 110; n++) {
                make_object(object, sizeof object, now + 3600, 2, (uint8_t) n);
                make_hash(hash, n);
                CHECK(ntb_object_db_save(db, hash, object, sizeof object, now));
        }
        ntb_object_db_stats(db, &count, &bytes);
        limit = bytes - 1;
        CHECK(ntb_object_db_prune(db, limit, 0) > 0);
        ntb_object_db_stats(db, &count, &bytes);
        CHECK(bytes <= limit * 95 / 100);
        make_hash(hash, 1);
        CHECK(!ntb_object_db_load(db, hash, capture_cb, &c));
        make_hash(hash, 109);
        CHECK(ntb_object_db_load(db, hash, capture_cb, &c));
        CHECK(ntb_object_db_prune(db, bytes + 1, 0) == 0);

        /* By age: everything received before the cutoff goes. */
        make_object(object, sizeof object, now + 3600, 2, 9);
        make_hash(hash, 200);
        CHECK(ntb_object_db_save(db, hash, object, sizeof object, now + 2 * 86400));
        CHECK(ntb_object_db_prune(db, INT64_MAX, now + 86400) > 0);
        ntb_object_db_stats(db, &count, &bytes);
        CHECK(count == 1);
        CHECK(ntb_object_db_load(db, hash, capture_cb, &c));

        /* seq is never reused after deletes. */
        last_seq = query_int(path, "SELECT max(seq) FROM objects");
        CHECK(ntb_object_db_prune(db, 0, 0) == 1);
        make_hash(hash, 300);
        CHECK(ntb_object_db_save(db, hash, object, sizeof object, now));
        CHECK(query_int(path, "SELECT max(seq) FROM objects") > last_seq);

        /* Pruning gives the space back to the disk. */
        for (n = 400; n < 600; n++) {
                make_hash(hash, n);
                CHECK(ntb_object_db_save(db, hash, object, sizeof object, now));
        }
        pages_before = query_int(path, "PRAGMA page_count");
        ntb_object_db_prune(db, 0, 0);
        CHECK(query_int(path, "PRAGMA page_count") < pages_before);

        /* Another schema version: emptied in place, with a new cache id. */
        query_text(path, "SELECT value FROM meta WHERE key='cache_id'", id_before, sizeof id_before);
        CHECK(strlen(id_before) == 32);
        make_hash(hash, 700);
        CHECK(ntb_object_db_save(db, hash, object, sizeof object, now));
        ntb_object_db_close(db);
        {
                sqlite3 *raw = NULL;
                CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
                CHECK(sqlite3_exec(raw, "PRAGMA user_version=99", NULL, NULL, NULL) == SQLITE_OK);
                sqlite3_close(raw);
        }
        db = ntb_object_db_open(argv[1]);
        CHECK(db != NULL);
        ntb_object_db_stats(db, &count, &bytes);
        CHECK(count == 0 && bytes == 0);
        CHECK(query_int(path, "PRAGMA user_version") == NTB_OBJECT_DB_VERSION);
        query_text(path, "SELECT value FROM meta WHERE key='cache_id'", id_after, sizeof id_after);
        CHECK(strlen(id_after) == 32 && strcmp(id_before, id_after) != 0);
        ntb_object_db_close(db);

        printf("PASS: object store\n");
        return 0;
}
