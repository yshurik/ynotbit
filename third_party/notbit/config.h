#pragma once
#define _GNU_SOURCE 1
#define VERSION "0.7-desktop"
#define HAVE_LITTLE_ENDIAN 1
#define HAVE_SYNC_REF_COUNT 1
#define HAVE_STATIC_ASSERT 1
#define ALIGNOF_NAME __alignof__
#ifdef __APPLE__
#define HAVE_GETPEEREID 1
#endif
