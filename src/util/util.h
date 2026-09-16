/* kache - small shared helpers and integer types */
#ifndef KACHE_UTIL_H
#define KACHE_UTIL_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

#define LEN(a)        (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)     ((a) < (b) ? (a) : (b))
#define MAX(a, b)     ((a) > (b) ? (a) : (b))
#define LIKELY(x)     __builtin_expect(!!(x), 1)
#define UNLIKELY(x)   __builtin_expect(!!(x), 0)
#define ALIGNUP(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGNDN(x, a) ((x) & ~((__typeof__(x))(a) - 1))

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define cpu_relax() __asm__ __volatile__("yield" ::: "memory")
#else
#define cpu_relax() __asm__ __volatile__("" ::: "memory")
#endif

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void verbosity(int level);

void *emalloc(size_t n);
void *ecalloc(size_t n, size_t sz);
void *erealloc(void *p, size_t n);

/* "512", "64k", "256M", "2G" -> bytes.  0 on success, -1 on garbage. */
int parse_size(const char *s, u64 *out);
/* strict decimal parsers over a counted buffer */
int parse_i64(const char *s, size_t n, i64 *out);
int parse_u64(const char *s, size_t n, u64 *out);
/* writes at most 20 digits plus sign, returns the length, no terminator */
size_t fmt_i64(char *dst, i64 v);
size_t fmt_u64(char *dst, u64 v);

u64 npow2(u64 v);   /* smallest power of two >= v */
u64 ppow2(u64 v);   /* largest power of two <= v */
int ncpu(void);
int ncores(void);   /* distinct physical cores, falls back to ncpu() */
u64 entropy64(void);

#endif /* KACHE_UTIL_H */
