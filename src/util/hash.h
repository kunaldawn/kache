/* kache - 64 bit string hash (wyhash, condensed)
 *
 * Fast enough that the hash never shows up in a profile, and well mixed
 * enough that the top bits pick a shard while the low bits pick a bucket. */
#ifndef KACHE_HASH_H
#define KACHE_HASH_H

#include <string.h>

#include "util/util.h"

static const u64 hash_secret[4] = {
	0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull,
	0x4b33a62ed433d4a3ull, 0x4d5a2da51de1aa47ull
};

static inline u64
hash_r8(const u8 *p)
{
	u64 v;
	memcpy(&v, p, 8);
	return v;
}

static inline u64
hash_r4(const u8 *p)
{
	u32 v;
	memcpy(&v, p, 4);
	return v;
}

static inline u64
hash_r3(const u8 *p, size_t k)
{
	return ((u64)p[0] << 16) | ((u64)p[k >> 1] << 8) | p[k - 1];
}

static inline u64
hash_mix(u64 a, u64 b)
{
#if defined(__SIZEOF_INT128__)
	__uint128_t r = (__uint128_t)a * b;
	return (u64)r ^ (u64)(r >> 64);
#else
	u64 ha = a >> 32, la = (u32)a, hb = b >> 32, lb = (u32)b;
	u64 rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
	u64 t = rl + (rm0 << 32), c = t < rl;
	u64 lo = t + (rm1 << 32);
	c += lo < t;
	return lo ^ (rh + (rm0 >> 32) + (rm1 >> 32) + c);
#endif
}

static inline u64
hash_bytes(const void *key, size_t len, u64 seed)
{
	const u8 *p = (const u8 *)key;
	u64 a, b;

	seed ^= hash_mix(seed ^ hash_secret[0], hash_secret[1]);
	if (LIKELY(len <= 16)) {
		if (LIKELY(len >= 4)) {
			a = (hash_r4(p) << 32) | hash_r4(p + ((len >> 3) << 2));
			b = (hash_r4(p + len - 4) << 32) |
			    hash_r4(p + len - 4 - ((len >> 3) << 2));
		} else if (len > 0) {
			a = hash_r3(p, len);
			b = 0;
		} else {
			a = b = 0;
		}
	} else {
		size_t i = len;
		if (UNLIKELY(i > 48)) {
			u64 s1 = seed, s2 = seed;
			do {
				seed = hash_mix(hash_r8(p) ^ hash_secret[1],
				                hash_r8(p + 8) ^ seed);
				s1 = hash_mix(hash_r8(p + 16) ^ hash_secret[2],
				              hash_r8(p + 24) ^ s1);
				s2 = hash_mix(hash_r8(p + 32) ^ hash_secret[3],
				              hash_r8(p + 40) ^ s2);
				p += 48;
				i -= 48;
			} while (i > 48);
			seed ^= s1 ^ s2;
		}
		while (UNLIKELY(i > 16)) {
			seed = hash_mix(hash_r8(p) ^ hash_secret[1],
			                hash_r8(p + 8) ^ seed);
			p += 16;
			i -= 16;
		}
		a = hash_r8(p + i - 16);
		b = hash_r8(p + i - 8);
	}
	a ^= hash_secret[1];
	b ^= seed;
#if defined(__SIZEOF_INT128__)
	{
		__uint128_t r = (__uint128_t)a * b;
		a = (u64)r;
		b = (u64)(r >> 64);
	}
#else
	{
		u64 m = hash_mix(a, b);
		a = m;
		b = m + len;
	}
#endif
	return hash_mix(a ^ hash_secret[0] ^ len, b ^ hash_secret[1]);
}

/* xorshift64*, used for eviction sampling.  State must be non zero. */
static inline u64
rng_next(u64 *s)
{
	u64 x = *s;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*s = x;
	return x * 0x2545f4914f6cdd1dull;
}

#endif /* KACHE_HASH_H */
