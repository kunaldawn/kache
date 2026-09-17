/* kache - the nested map.  See kkv.h for why it looks like this. */
#include <string.h>

#include "config.h"
#include "store/alloc.h"
#include "store/kkv.h"
#include "store/shard.h"
#include "util/hash.h"

static inline KkvTab *
tab_of(const Map *m, const Shard *s, const KkvHdr *h)
{
	return (KkvTab *)map_at(m, sub_off(s, h->tab));
}

static inline u32
tab_bytes(u32 nslots)
{
	return (u32)(sizeof(KkvTab) + (u64)nslots * sizeof(KkvSlot));
}

void
kkv_init(Rec *r)
{
	memset(cont_ctl(r), 0, sizeof(KkvHdr));
	r->vlen = (u32)sizeof(KkvHdr);
}

/* ---- the slot array -------------------------------------------------- */

static void
tab_link(KkvTab *t, u32 mask, u32 tag, Ref ref)
{
	u32 i = tag & mask;

	while (t->slot[i].ref)
		i = (i + 1) & mask;
	t->slot[i].tag = tag;
	t->slot[i].ref = ref;
}

/* Backward shift deletion, as in the shard index.  The tag is the low 32
 * bits of the hash, so a slot still knows where it belongs. */
static void
tab_unlink(KkvTab *t, u32 mask, u32 i)
{
	u32 j = i, k;

	t->slot[i].tag = 0;
	t->slot[i].ref = 0;
	for (;;) {
		j = (j + 1) & mask;
		if (!t->slot[j].ref)
			return;
		k = t->slot[j].tag & mask;
		if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
			continue;
		t->slot[i] = t->slot[j];
		t->slot[j].tag = 0;
		t->slot[j].ref = 0;
		i = j;
	}
}

/* Move to a table of want slots, or free it outright when want is 0.
 * Rehashing reads the old slots and nothing else: the tag carries the
 * whole of the new position, so no field record is touched. */
static int
tab_resize(Map *m, Shard *s, Rec *r, u32 want)
{
	KkvHdr *h = kkv_hdr(r);
	KkvTab *nt;
	u64 off;

	if (!want) {
		if (h->tab)
			alc_free(m, s, sub_off(s, h->tab), NULL, NULL);
		h->tab = 0;
		h->nslots = 0;
		return 0;
	}
	if (!(off = shd_alloc_sub(m, s, tab_bytes(want))))
		return -1;
	nt = (KkvTab *)map_at(m, off);
	nt->owner = sub_ref(s, map_off(m, r));
	nt->nslots = want;
	memset(nt->slot, 0, (size_t)want * sizeof(KkvSlot));
	if (h->tab) {
		KkvTab *ot = tab_of(m, s, h);
		u32 i;

		for (i = 0; i < h->nslots; i++)
			if (ot->slot[i].ref)
				tab_link(nt, want - 1, ot->slot[i].tag,
				         ot->slot[i].ref);
		alc_free(m, s, sub_off(s, h->tab), NULL, NULL);
	}
	h->tab = sub_ref(s, off);
	h->nslots = want;
	return 0;
}

/* room for one more field */
static int
tab_reserve(Map *m, Shard *s, Rec *r)
{
	KkvHdr *h = kkv_hdr(r);

	if (!h->tab)
		return tab_resize(m, s, r, CFG_KKV_SLOTS_MIN);
	if (((u64)h->count + 1) * 256 <= (u64)h->nslots * CFG_KKV_LOAD)
		return 0;
	if (h->nslots >= CFG_KKV_SLOTS_MAX)
		return -1;
	return tab_resize(m, s, r, h->nslots << 1);
}

/* Give the table back once the map has shrunk under it.  Deliberately
 * not part of kkv_erase: an enumeration erases expired fields as it goes
 * and must not have the array move underneath it. */
void
kkv_compact(Map *m, Shard *s, Rec *r)
{
	KkvHdr *h = kkv_hdr(r);
	u32 want;

	if (!h->tab)
		return;
	if (!h->count) {
		tab_resize(m, s, r, 0);
		return;
	}
	if (h->nslots <= CFG_KKV_SLOTS_MIN || (u64)h->count * 8 > h->nslots)
		return;
	want = (u32)npow2((u64)h->count * 4);
	if (want < CFG_KKV_SLOTS_MIN)
		want = CFG_KKV_SLOTS_MIN;
	if (want < h->nslots)
		tab_resize(m, s, r, want);   /* a refusal costs only memory */
}

/* ---- the operations -------------------------------------------------- */

KkvFld *
kkv_find(const Map *m, const Shard *s, const KkvHdr *h, u64 fh,
         const void *f, u32 fl, u32 *slot)
{
	KkvTab *t;
	u32 mask, tag = (u32)fh, i;

	if (!h->tab)
		return NULL;
	t = tab_of(m, s, h);
	mask = h->nslots - 1;
	i = tag & mask;
	for (;;) {
		if (!t->slot[i].ref)
			return NULL;
		if (t->slot[i].tag == tag) {
			KkvFld *fd = kkv_at(m, s, t->slot[i].ref);

			if (fd->klen == fl && memcmp(fd->data, f, fl) == 0) {
				*slot = i;
				return fd;
			}
		}
		i = (i + 1) & mask;
	}
}

KkvFld *
kkv_put(Map *m, Shard *s, Rec *r, u64 fh, const void *f, u32 fl, u32 vl,
        int *created)
{
	KkvHdr *h = kkv_hdr(r);
	u32 need = FLD_NEED(fl, vl), slot = 0;
	KkvFld *old, *nf;
	u64 off;

	*created = 0;
	old = kkv_find(m, s, h, fh, f, fl, &slot);
	if (old && alc_fits(alc_cap(m, map_off(m, old)), need)) {
		h->bytes = h->bytes - old->vlen + vl;
		old->vlen = vl;
		return old;
	}
	if (!old && tab_reserve(m, s, r) < 0)
		return NULL;
	/* The allocation may evict, but only whole keys, and this one is
	 * pinned by the caller - so neither the map nor any field of it can
	 * go away underneath us. */
	if (!(off = shd_alloc_sub(m, s, need)))
		return NULL;
	nf = (KkvFld *)map_at(m, off);
	nf->hash = fh;
	nf->owner = sub_ref(s, map_off(m, r));
	nf->klen = (u16)fl;
	nf->pad = 0;
	nf->vlen = vl;
	memcpy(nf->data, f, fl);

	if (old) {
		/* no table move happened, so the slot is still the one */
		h->bytes -= old->vlen;
		h->count--;
		tab_unlink(tab_of(m, s, h), h->nslots - 1, slot);
		alc_free(m, s, map_off(m, old), NULL, NULL);
	} else {
		*created = 1;
	}
	tab_link(tab_of(m, s, h), h->nslots - 1, (u32)fh, sub_ref(s, off));
	h->count++;
	h->bytes += vl;
	return nf;
}

void
kkv_erase(Map *m, Shard *s, Rec *r, u32 slot)
{
	KkvHdr *h = kkv_hdr(r);
	KkvTab *t = tab_of(m, s, h);
	Ref ref = t->slot[slot].ref;

	h->bytes -= kkv_at(m, s, ref)->vlen;
	h->count--;
	tab_unlink(t, h->nslots - 1, slot);
	alc_free(m, s, sub_off(s, ref), NULL, NULL);
}

KkvFld *
kkv_walk(Map *m, Shard *s, Rec *r, u64 now, u32 *cur, u32 *slot)
{
	KkvHdr *h = kkv_hdr(r);
	KkvTab *t;

	if (!h->tab)
		return NULL;
	t = tab_of(m, s, h);
	while (*cur < h->nslots) {
		u32 i = *cur;
		KkvFld *f;

		if (!t->slot[i].ref) {
			(*cur)++;
			continue;
		}
		f = kkv_at(m, s, t->slot[i].ref);
		if (fld_expired(f, now)) {
			s->expirations++;
			kkv_erase(m, s, r, i);
			/* a backward shift may have pulled a later entry
			 * into this slot, so look at it again */
			continue;
		}
		*slot = i;
		(*cur)++;
		return f;
	}
	return NULL;
}

int
kkv_reap(Map *m, Shard *s, Rec *r, u32 *budget)
{
	KkvHdr *h = kkv_hdr(r);
	KkvTab *t;

	if (!h->tab)
		return 1;
	t = tab_of(m, s, h);
	while (h->gpos < h->nslots) {
		Ref ref;

		if (!*budget)
			return 0;
		(*budget)--;
		if ((ref = t->slot[h->gpos].ref) != 0)
			alc_free(m, s, sub_off(s, ref), NULL, NULL);
		h->gpos++;
	}
	alc_free(m, s, sub_off(s, h->tab), NULL, NULL);
	h->tab = 0;
	h->nslots = 0;
	h->count = 0;
	return 1;
}

/* ---- recovery --------------------------------------------------------
 *
 * Claiming is part of validating: a block already marked belongs to
 * somebody else, and a stale reference left by a crash must not let two
 * maps share a field.  So the marks go down as the walk goes, and come
 * back off if the walk ends badly. */

static void
unclaim(Map *m, Shard *s, const KkvTab *t, u32 upto)
{
	u32 i;

	for (i = 0; i < upto; i++) {
		u64 off;

		if (!t->slot[i].ref)
			continue;
		off = sub_off(s, t->slot[i].ref);
		if (off > s->arena_off && off < s->arena_off + s->arena_size)
			alc_unmark(m, off, BLK_MARK);
	}
}

/* Every occupied slot must be reachable from its home by probing, or a
 * lookup would walk past it into an empty slot and report a miss.  One
 * pass: start from an empty slot, and inside each run of occupied slots
 * every home must lie between the run's start and the slot itself. */
static int
probes_sound(const KkvTab *t, u32 nslots)
{
	u32 mask = nslots - 1, e, run, i;

	for (e = 0; e < nslots; e++)
		if (!t->slot[e].ref)
			break;
	if (e == nslots)
		return 0;                    /* full: cannot be at 0.75 */
	run = (e + 1) & mask;
	for (i = 1; i <= nslots; i++) {
		u32 j = (e + i) & mask, home;

		if (!t->slot[j].ref) {
			run = (j + 1) & mask;
			continue;
		}
		home = t->slot[j].tag & mask;
		if (run <= j ? (home < run || home > j)
		             : (home < run && home > j))
			return 0;
	}
	return 1;
}

int
kkv_check(Map *m, Shard *s, Rec *r, u64 now)
{
	KkvHdr *h;
	KkvTab *t;
	Ref self = sub_ref(s, map_off(m, r));
	u64 toff, bytes = 0;
	u32 i, count = 0;

	(void)now;
	if (r->vlen != sizeof(KkvHdr))
		return -1;
	h = kkv_hdr(r);
	h->gnext = 0;
	h->gpos = 0;
	if (!h->tab) {
		h->nslots = 0;
		h->count = 0;
		h->bytes = 0;
		return 0;
	}
	if (h->nslots < CFG_KKV_SLOTS_MIN || h->nslots > CFG_KKV_SLOTS_MAX ||
	    (h->nslots & (h->nslots - 1)))
		return -1;
	if (!(toff = shd_subok(m, s, h->tab, tab_bytes(h->nslots))))
		return -1;
	t = (KkvTab *)map_at(m, toff);
	if (t->owner != self || t->nslots != h->nslots)
		return -1;
	alc_mark(m, toff, BLK_MARK);

	for (i = 0; i < h->nslots; i++) {
		KkvFld *f;
		u64 foff;

		if (!t->slot[i].ref)
			continue;
		if (!(foff = shd_subok(m, s, t->slot[i].ref, FLD_HDR)))
			goto bad;
		f = (KkvFld *)map_at(m, foff);
		if (f->owner != self || !f->klen || f->klen > m->maxkey ||
		    f->vlen > m->maxval)
			goto bad;
		if (FLD_NEED(f->klen, f->vlen) + ST_BLKHDR >
		    BLK_SIZE(alc_of(m, foff)))
			goto bad;
		if (f->hash != hash_bytes(f->data, f->klen, m->seed) ||
		    t->slot[i].tag != (u32)f->hash)
			goto bad;
		alc_mark(m, foff, BLK_MARK);
		count++;
		bytes += f->vlen;
	}
	if (!probes_sound(t, h->nslots)) {
		i = h->nslots;
		goto bad;
	}
	h->count = count;
	h->bytes = bytes;
	return 0;
bad:
	unclaim(m, s, t, i);
	alc_unmark(m, toff, BLK_MARK);
	return -1;
}
