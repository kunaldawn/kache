/* kache - diagnostics, allocation wrappers and the small parsers and
 * formatters used everywhere else.  Nothing here knows about the store. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util/util.h"

static int verbose = 1;

void
verbosity(int level)
{
	verbose = level;
}

static void
vmsg(const char *tag, const char *fmt, va_list ap)
{
	fprintf(stderr, "kache: %s", tag);
	vfprintf(stderr, fmt, ap);
	if (fmt[0] && fmt[strlen(fmt) - 1] == ':')
		fprintf(stderr, " %s", strerror(errno));
	fputc('\n', stderr);
}

void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg("", fmt, ap);
	va_end(ap);
	exit(1);
}

void
warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg("warning: ", fmt, ap);
	va_end(ap);
}

void
info(const char *fmt, ...)
{
	va_list ap;

	if (verbose < 1)
		return;
	va_start(ap, fmt);
	vmsg("", fmt, ap);
	va_end(ap);
}

void *
emalloc(size_t n)
{
	void *p = malloc(n);

	if (!p)
		die("out of memory");
	return p;
}

void *
ecalloc(size_t n, size_t sz)
{
	void *p = calloc(n, sz);

	if (!p)
		die("out of memory");
	return p;
}

void *
erealloc(void *p, size_t n)
{
	void *q = realloc(p, n);

	if (!q)
		die("out of memory");
	return q;
}

int
parse_size(const char *s, u64 *out)
{
	u64 v = 0;
	int digits = 0;

	if (!s)
		return -1;
	while (*s >= '0' && *s <= '9') {
		if (v > (UINT64_MAX - 9) / 10)
			return -1;
		v = v * 10 + (u64)(*s++ - '0');
		digits++;
	}
	if (!digits)
		return -1;
	switch (*s) {
	case 'k': case 'K': v <<= 10; s++; break;
	case 'm': case 'M': v <<= 20; s++; break;
	case 'g': case 'G': v <<= 30; s++; break;
	case 't': case 'T': v <<= 40; s++; break;
	}
	if (*s == 'b' || *s == 'B')
		s++;
	if (*s)
		return -1;
	*out = v;
	return 0;
}

int
parse_u64(const char *s, size_t n, u64 *out)
{
	u64 v = 0;
	size_t i;

	if (!n || n > 20)
		return -1;
	for (i = 0; i < n; i++) {
		u8 c = (u8)s[i] - '0';
		if (c > 9)
			return -1;
		if (v > (UINT64_MAX - c) / 10)
			return -1;
		v = v * 10 + c;
	}
	*out = v;
	return 0;
}

int
parse_i64(const char *s, size_t n, i64 *out)
{
	int neg = 0;
	u64 v;

	if (n && (*s == '-' || *s == '+')) {
		neg = (*s == '-');
		s++;
		n--;
	}
	if (parse_u64(s, n, &v) < 0)
		return -1;
	if (neg) {
		if (v > (u64)INT64_MAX + 1)
			return -1;
		*out = (v == (u64)INT64_MAX + 1) ? INT64_MIN : -(i64)v;
	} else {
		if (v > (u64)INT64_MAX)
			return -1;
		*out = (i64)v;
	}
	return 0;
}

/* About five numbers are formatted into every response, so the digits
 * come out two at a time: one division per pair instead of per digit,
 * and the pair itself is a table lookup. */
static const char digits2[201] =
	"00010203040506070809" "10111213141516171819"
	"20212223242526272829" "30313233343536373839"
	"40414243444546474849" "50515253545556575859"
	"60616263646566676869" "70717273747576777879"
	"80818283848586878889" "90919293949596979899";

size_t
fmt_u64(char *dst, u64 v)
{
	char tmp[20], *p = tmp + sizeof(tmp);
	size_t n;

	/* built from the back, so there is no reversal pass afterwards */
	while (v >= 100) {
		u64 q = v / 100;
		unsigned r = (unsigned)(v - q * 100) * 2;

		p -= 2;
		p[0] = digits2[r];
		p[1] = digits2[r + 1];
		v = q;
	}
	if (v >= 10) {
		unsigned r = (unsigned)v * 2;

		p -= 2;
		p[0] = digits2[r];
		p[1] = digits2[r + 1];
	} else {
		*--p = (char)('0' + (unsigned)v);
	}
	/* callers size their buffers on the return value, and several
	 * write into a fixed slot, so never touch dst beyond n */
	n = (size_t)(tmp + sizeof(tmp) - p);
	memcpy(dst, p, n);
	return n;
}

size_t
fmt_i64(char *dst, i64 v)
{
	if (v < 0) {
		*dst = '-';
		return 1 + fmt_u64(dst + 1, -(u64)v);
	}
	return fmt_u64(dst, (u64)v);
}

u64
npow2(u64 v)
{
	if (v <= 1)
		return 1;
	return 1ull << (64 - (unsigned)__builtin_clzll(v - 1));
}

u64
ppow2(u64 v)
{
	if (!v)
		return 0;
	return 1ull << (63 - (unsigned)__builtin_clzll(v));
}

int
ncpu(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);

	return n > 0 ? (int)n : 1;
}

/* Physical cores, for the callers that want one worker per core rather
 * than per hardware thread.  A cpu is counted only when it is the first
 * entry of its own sibling list, so a sibling group contributes exactly
 * one core and no set is needed.  Anything we cannot account for - no
 * sysfs, a kernel without topology, online cpus whose files we never
 * found - falls back to ncpu(), which is never wrong, only pessimistic. */
int
ncores(void)
{
	char path[80], buf[32];
	int n = ncpu(), seen = 0, cores = 0, i;

	/* ids are normally dense, so this is n opens; the cap only keeps
	 * a sparse or half offline machine from scanning forever */
	for (i = 0; seen < n && i < n * 4 + 64; i++) {
		unsigned first = 0;
		const char *p;
		ssize_t r;
		int fd;

		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d"
		    "/topology/thread_siblings_list", i);
		if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
			continue;   /* offline, or not a cpu at all */
		r = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (r <= 0)
			return n;
		buf[r] = '\0';
		seen++;
		for (p = buf; *p >= '0' && *p <= '9'; p++)
			first = first * 10 + (unsigned)(*p - '0');
		/* the list is sorted, so a first entry above our own id
		 * means we are not reading what we think we are */
		if (p == buf || first > (unsigned)i)
			return n;
		if (first == (unsigned)i)
			cores++;
	}
	return (seen == n && cores > 0) ? cores : n;
}

u64
entropy64(void)
{
	struct timespec ts;
	u64 v = 0;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

	if (fd >= 0) {
		ssize_t r = read(fd, &v, sizeof(v));
		close(fd);
		if (r == (ssize_t)sizeof(v) && v)
			return v;
	}
	clock_gettime(CLOCK_REALTIME, &ts);
	v = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
	v ^= (u64)getpid() << 32;
	return v ? v : 0x9e3779b97f4a7c15ull;
}
