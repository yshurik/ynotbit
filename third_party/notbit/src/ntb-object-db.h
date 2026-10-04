/* The node's network objects, in one SQLite file. The node is the only
 * writer; the desktop app reads the same file with its own connection
 * (src/cache.cpp), which is why the file name, version and layout live here. */
#ifndef NTB_OBJECT_DB_H
#define NTB_OBJECT_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NTB_OBJECT_DB_FILE "objects.sqlite"
#define NTB_OBJECT_DB_VERSION 1
#define NTB_OBJECT_DB_HASH_SIZE 32
#define NTB_OBJECT_DB_MAX_OBJECT_SIZE 262144

/* meta holds 'cache_id', which changes whenever the store is emptied, so a
 * reader knows to start over. objects_listing covers the startup listing and
 * the size totals, which would otherwise read every object's bytes. */
#define NTB_OBJECT_DB_SCHEMA \
        "CREATE TABLE objects(" \
        " seq INTEGER PRIMARY KEY AUTOINCREMENT," \
        " hash BLOB NOT NULL UNIQUE," \
        " expires INTEGER NOT NULL," \
        " received INTEGER NOT NULL," \
        " size INTEGER NOT NULL," \
        " payload BLOB NOT NULL);" \
        "CREATE INDEX objects_listing ON objects(expires, hash, size);" \
        "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"

struct ntb_object_db;

typedef void (*ntb_object_db_object_func)(const uint8_t *hash,
                                          int64_t expires,
                                          const uint8_t *data,
                                          size_t size,
                                          void *user_data);

struct ntb_object_db *
ntb_object_db_open(const char *directory);

void
ntb_object_db_close(struct ntb_object_db *db);

bool
ntb_object_db_begin(struct ntb_object_db *db);

bool
ntb_object_db_commit(struct ntb_object_db *db);

bool
ntb_object_db_save(struct ntb_object_db *db,
                   const uint8_t *hash,
                   const uint8_t *data,
                   size_t size,
                   int64_t received);

bool
ntb_object_db_load(struct ntb_object_db *db,
                   const uint8_t *hash,
                   ntb_object_db_object_func func,
                   void *user_data);

void
ntb_object_db_for_each(struct ntb_object_db *db,
                       int64_t min_expires,
                       bool with_data,
                       ntb_object_db_object_func func,
                       void *user_data);

int64_t
ntb_object_db_prune(struct ntb_object_db *db,
                    int64_t max_bytes,
                    int64_t min_received);

void
ntb_object_db_stats(struct ntb_object_db *db,
                    int64_t *count,
                    int64_t *bytes);

#ifdef __cplusplus
}
#endif

#endif /* NTB_OBJECT_DB_H */
