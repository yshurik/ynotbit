#pragma once
#define _GNU_SOURCE 1
#define VERSION "0.7-desktop"
#define HAVE_LITTLE_ENDIAN 1
#define HAVE_STATIC_ASSERT 1
#define HAVE_SYNC_REF_COUNT 1
#ifdef _MSC_VER
/* MSVC has no GCC __sync builtins; its interlocked intrinsics do the same
 * (int and long are both 32 bits on Windows). ntb-ref-count.h's non-__sync
 * fallback does not compile, so it is not an option. */
#include <intrin.h>
#define __sync_fetch_and_add(p, v) _InterlockedExchangeAdd((volatile long *) (p), (long) (v))
#define __sync_fetch_and_sub(p, v) _InterlockedExchangeAdd((volatile long *) (p), -(long) (v))
#define ALIGNOF_NAME __alignof
#else
#define ALIGNOF_NAME __alignof__
#endif
#ifdef __APPLE__
#define HAVE_GETPEEREID 1
#endif
