/* kache - the per worker hot set.  See hot.h for what it is for. */
#include <stdlib.h>
#include <string.h>

#include "http/hot.h"
#include "util/clk.h"
#include "util/util.h"

/* The set is direct mapped, and the slot is taken from the middle bits
 * of the hash.  The low bits already choose the shard's bucket and the
 * top bits already choose the shard, so reusing either would line the
 * set up with a structure the keys are spread across on purpose. */
static inline u32
slot_of(u64 hash)
{
	return (u32)((hash >> 20) & (CFG_HOT_SLOTS - 1));
}

static inline u32
door_of(u64 hash)
{
	return (u32)((hash >> 32) & (CFG_HOT_DOOR - 1));
}

Hot *
hot_new(u64 now, u64 window)
{
	Hot *h = ecalloc(1, sizeof(Hot));

	h->window = window;
	h->decay = now + CFG_HOT_DECAY_MS;
	/* gen 0 is what a zeroed entry carries, so start past it and an
	 * empty slot can never be mistaken for a filled one. */
	h->gen = 1;
	return h;
}

void
hot_free(Hot *h)
{
	free(h);
}

const HotEnt *
hot_get(Hot *h, u64 hash, const void *k, u32 kl, u64 now)
{
	HotEnt *e = &h->ent[slot_of(hash)];

	/* One line decides almost every miss: hash, generation and
	 * deadline sit together at the head of the entry, ahead of the
	 * key and the response bytes. */
	if (e->hash != hash || e->gen != h->gen || now >= e->until)
		return NULL;
	if (e->klen != kl || memcmp(e->key, k, kl))
		return NULL;
	h->hits++;
	return e;
}

int
hot_admit(Hot *h, u64 hash, u64 now)
{
	u8 *c;

	/* Decay is a clear rather than a halving.  The set is asking one
	 * question - is this key hot right now - and a window that starts
	 * empty answers it without carrying a count that a key earned
	 * during some earlier burst. */
	if (now >= h->decay) {
		memset(h->door, 0, sizeof(h->door));
		h->decay = now + CFG_HOT_DECAY_MS;
	}
	c = &h->door[door_of(hash)];
	if (*c < 255)
		(*c)++;
	if (*c < CFG_HOT_ADMIT)
		return 0;
	/* Earned a slot.  Clear the counter so the next window starts the
	 * key over: if it stays hot it is in the set and never comes back
	 * here, and if it does come back it has to earn the slot again. */
	*c = 0;
	h->admits++;
	return 1;
}

void
hot_fill(Hot *h, u64 hash, const void *k, u32 kl, const void *resp,
         u32 rlen, u32 doff, u64 now)
{
	HotEnt *e = &h->ent[slot_of(hash)];

	if (kl > CFG_HOT_KEY || rlen > CFG_HOT_RESP)
		return;
	e->hash = hash;
	e->until = now + h->window;
	e->gen = h->gen;
	e->klen = kl;
	e->rlen = rlen;
	e->doff = doff;
	memcpy(e->key, k, kl);
	memcpy(e->resp, resp, rlen);
	h->fills++;
}

int
hot_date_off(const void *resp, u32 rlen)
{
	static const char pat[] = "\r\nDate: ";
	const char *p = resp, *q;
	u32 n = rlen;

	/* The offset is fixed for a given status and minimal setting, but
	 * deriving it from the response that was actually built means the
	 * header layout can change in http.c without silently patching
	 * the wrong bytes here.  It runs once per fill, not per hit. */
	if (rlen < sizeof(pat) - 1 + CLK_DATE_LEN)
		return -1;
	n = rlen - CLK_DATE_LEN;
	for (q = p; q + sizeof(pat) - 1 <= p + n; q++) {
		if (!memcmp(q, pat, sizeof(pat) - 1))
			return (int)((q - p) + sizeof(pat) - 1);
	}
	return -1;
}
