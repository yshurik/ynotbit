#include "ntb-object-db.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlcipher/sqlite3.h>
#ifdef _WIN32
#include <direct.h>
#define rmdir _rmdir
#else
#include <unistd.h>
#endif

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

struct ntb_object_db {
        sqlite3 *db;
        sqlite3_stmt *insert;
        int64_t count, bytes;
};

static char *
join_path(const char *directory, const char *name)
{
        size_t length = strlen(directory);
        char *path = malloc(length + strlen(name) + 2);

        if (path == NULL)
                return NULL;
        memcpy(path, directory, length);
        if (length == 0 || directory[length - 1] != '/')
                path[length++] = '/';
        strcpy(path + length, name);
        return path;
}

/* Before this store, each object was a file in <directory>/objects. */
static void
remove_object_folder(const char *directory)
{
        char *folder = join_path(directory, "objects");
        struct dirent *entry;
        DIR *dir;

        if (folder == NULL)
                return;
        dir = opendir(folder);
        if (dir) {
                while ((entry = readdir(dir))) {
                        char *file;

                        if (entry->d_name[0] == '.')
                                continue;
                        file = join_path(folder, entry->d_name);
                        if (file) {
                                remove(file);
                                free(file);
                        }
                }
                closedir(dir);
                rmdir(folder);
        }
        free(folder);
}

static bool
exec(sqlite3 *db, const char *sql)
{
        return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

static int64_t
user_version(sqlite3 *db)
{
        sqlite3_stmt *stmt;
        int64_t version = -1;

        if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) != SQLITE_OK)
                return -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
                version = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return version;
}

/* Empties the file in place rather than deleting it: the app may have it
 * open, and Windows refuses to delete an open file. Version 0 while this
 * runs tells readers the store isn't ready. */
static bool
create_schema(sqlite3 *db)
{
        return exec(db,
                    "PRAGMA user_version=0;"
                    "DROP TABLE IF EXISTS objects;"
                    "DROP TABLE IF EXISTS meta;") &&
                exec(db, "PRAGMA auto_vacuum=INCREMENTAL;") &&
                exec(db, "VACUUM;") &&
                exec(db,
                     "BEGIN;"
                     NTB_OBJECT_DB_SCHEMA
                     "INSERT INTO meta VALUES('cache_id', lower(hex(randomblob(16))));"
                     "PRAGMA user_version=" STRINGIFY(NTB_OBJECT_DB_VERSION) ";"
                     "COMMIT;");
}

static void
load_totals(struct ntb_object_db *db)
{
        sqlite3_stmt *stmt;

        db->count = 0;
        db->bytes = 0;
        if (sqlite3_prepare_v2(db->db,
                               "SELECT count(*), coalesce(sum(size), 0) "
                               "FROM objects INDEXED BY objects_listing",
                               -1, &stmt, NULL) != SQLITE_OK)
                return;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
                db->count = sqlite3_column_int64(stmt, 0);
                db->bytes = sqlite3_column_int64(stmt, 1);
        }
        sqlite3_finalize(stmt);
}

struct ntb_object_db *
ntb_object_db_open(const char *directory)
{
        struct ntb_object_db *db = calloc(1, sizeof *db);
        char *path = join_path(directory, NTB_OBJECT_DB_FILE);

        if (db == NULL || path == NULL)
                goto error;

        remove_object_folder(directory);

        if (sqlite3_open_v2(path, &db->db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            NULL) != SQLITE_OK)
                goto error;
        sqlite3_busy_timeout(db->db, 5000);
        if (!exec(db->db, "PRAGMA journal_mode=WAL;") ||
            !exec(db->db, "PRAGMA synchronous=NORMAL;"))
                goto error;
        if (user_version(db->db) != NTB_OBJECT_DB_VERSION &&
            !create_schema(db->db))
                goto error;
        if (sqlite3_prepare_v2(db->db,
                               "INSERT OR IGNORE INTO "
                               "objects(hash, expires, received, size, payload) "
                               "VALUES(?, ?, ?, ?, ?)",
                               -1, &db->insert, NULL) != SQLITE_OK)
                goto error;
        load_totals(db);
        free(path);
        return db;

error:
        free(path);
        if (db) {
                sqlite3_finalize(db->insert);
                sqlite3_close(db->db);
                free(db);
        }
        return NULL;
}

void
ntb_object_db_close(struct ntb_object_db *db)
{
        if (db == NULL)
                return;
        sqlite3_finalize(db->insert);
        sqlite3_close(db->db);
        free(db);
}

bool
ntb_object_db_begin(struct ntb_object_db *db)
{
        return exec(db->db, "BEGIN");
}

bool
ntb_object_db_commit(struct ntb_object_db *db)
{
        if (exec(db->db, "COMMIT"))
                return true;
        exec(db->db, "ROLLBACK");
        load_totals(db);
        return false;
}

static int64_t
read_be64(const uint8_t *p)
{
        uint64_t value = 0;
        int i;

        for (i = 0; i < 8; i++)
                value = (value << 8) | p[i];
        return (int64_t) value;
}

bool
ntb_object_db_save(struct ntb_object_db *db,
                   const uint8_t *hash,
                   const uint8_t *data,
                   size_t size,
                   int64_t received)
{
        bool saved;

        /* Every object starts with an 8-byte nonce and its 8-byte expiry. */
        if (size < 16)
                return false;

        sqlite3_bind_blob(db->insert, 1, hash, NTB_OBJECT_DB_HASH_SIZE, SQLITE_STATIC);
        sqlite3_bind_int64(db->insert, 2, read_be64(data + 8));
        sqlite3_bind_int64(db->insert, 3, received);
        sqlite3_bind_int64(db->insert, 4, (int64_t) size);
        sqlite3_bind_blob64(db->insert, 5, data, size, SQLITE_STATIC);
        saved = sqlite3_step(db->insert) == SQLITE_DONE;
        if (saved && sqlite3_changes(db->db) == 1) {
                db->count++;
                db->bytes += (int64_t) size;
        }
        sqlite3_reset(db->insert);
        sqlite3_clear_bindings(db->insert);
        return saved;
}

bool
ntb_object_db_load(struct ntb_object_db *db,
                   const uint8_t *hash,
                   ntb_object_db_object_func func,
                   void *user_data)
{
        sqlite3_stmt *stmt;
        bool found = false;

        if (sqlite3_prepare_v2(db->db,
                               "SELECT expires, payload FROM objects WHERE hash = ?",
                               -1, &stmt, NULL) != SQLITE_OK)
                return false;
        sqlite3_bind_blob(stmt, 1, hash, NTB_OBJECT_DB_HASH_SIZE, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
                func(hash,
                     sqlite3_column_int64(stmt, 0),
                     sqlite3_column_blob(stmt, 1),
                     (size_t) sqlite3_column_bytes(stmt, 1),
                     user_data);
                found = true;
        }
        sqlite3_finalize(stmt);
        return found;
}

void
ntb_object_db_for_each(struct ntb_object_db *db,
                       int64_t min_expires,
                       bool with_data,
                       ntb_object_db_object_func func,
                       void *user_data)
{
        const char *sql = with_data ?
                "SELECT hash, expires, payload FROM objects WHERE expires > ?" :
                "SELECT hash, expires, NULL FROM objects "
                "INDEXED BY objects_listing WHERE expires > ?";
        sqlite3_stmt *stmt;

        if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
                return;
        sqlite3_bind_int64(stmt, 1, min_expires);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (sqlite3_column_bytes(stmt, 0) != NTB_OBJECT_DB_HASH_SIZE)
                        continue;
                func(sqlite3_column_blob(stmt, 0),
                     sqlite3_column_int64(stmt, 1),
                     with_data ? sqlite3_column_blob(stmt, 2) : NULL,
                     with_data ? (size_t) sqlite3_column_bytes(stmt, 2) : 0,
                     user_data);
        }
        sqlite3_finalize(stmt);
}

static int64_t
delete_with(sqlite3 *db, const char *sql, int64_t value)
{
        sqlite3_stmt *stmt;
        int64_t deleted = 0;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
                return 0;
        sqlite3_bind_int64(stmt, 1, value);
        if (sqlite3_step(stmt) == SQLITE_DONE)
                deleted = sqlite3_changes(db);
        sqlite3_finalize(stmt);
        return deleted;
}

int64_t
ntb_object_db_prune(struct ntb_object_db *db,
                    int64_t max_bytes,
                    int64_t min_received)
{
        /* Once over the limit, free a margin below it, so the next arrival
         * doesn't trigger another prune straight away. */
        const int64_t target = db->bytes > max_bytes ? max_bytes * 95 / 100 : max_bytes;
        int64_t deleted = 0, freed = 0, last_seq = 0;
        sqlite3_stmt *stmt;

        if (!exec(db->db, "BEGIN"))
                return 0;
        deleted += delete_with(db->db, "DELETE FROM objects WHERE received < ?", min_received);
        load_totals(db);
        if (db->bytes > target &&
            sqlite3_prepare_v2(db->db, "SELECT seq, size FROM objects ORDER BY seq",
                               -1, &stmt, NULL) == SQLITE_OK) {
                while (db->bytes - freed > target && sqlite3_step(stmt) == SQLITE_ROW) {
                        last_seq = sqlite3_column_int64(stmt, 0);
                        freed += sqlite3_column_int64(stmt, 1);
                }
                sqlite3_finalize(stmt);
                if (last_seq > 0)
                        deleted += delete_with(db->db,
                                               "DELETE FROM objects WHERE seq <= ?",
                                               last_seq);
        }
        if (!exec(db->db, "COMMIT"))
                exec(db->db, "ROLLBACK");
        load_totals(db);
        if (deleted > 0)
                exec(db->db, "PRAGMA incremental_vacuum;");
        return deleted;
}

void
ntb_object_db_stats(struct ntb_object_db *db, int64_t *count, int64_t *bytes)
{
        *count = db->count;
        *bytes = db->bytes;
}
