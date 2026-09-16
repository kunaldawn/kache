# kache - a multi threaded, file backed key/value cache
# See LICENSE for copyright and license details.
.POSIX:

include config.mk

# util/  primitives with no knowledge of the store
# store/ the mapped file, the allocator, the index, the operations
# http/  the front end that exposes the store over a socket
UTIL = src/util/util.c src/util/clk.c src/util/lock.c
STORE = src/store/map.c src/store/alloc.c src/store/shard.c src/store/db.c
HTTP = src/http/buf.c src/http/stats.c src/http/http.c src/http/route.c \
       src/http/conn.c src/http/server.c

# the engine without the front end, so the microbenchmark can link it
CORE = $(UTIL) $(STORE)
COREOBJ = $(CORE:.c=.o)

SRC = $(UTIL) $(STORE) $(HTTP) src/main.c
OBJ = $(SRC:.c=.o)

TOOLS = kache-bench kache-micro kache-cmp

HDR = src/util/util.h src/util/hash.h src/util/clk.h src/util/lock.h \
      src/store/store.h src/store/map.h src/store/alloc.h src/store/shard.h \
      src/store/db.h src/http/buf.h src/http/stats.h src/http/http.h \
      src/http/route.h src/http/conn.h src/http/server.h config.h

all: kache

options:
	@echo kache build options:
	@echo "CFLAGS   = $(CFLAGS)"
	@echo "CPPFLAGS = $(CPPFLAGS)"
	@echo "LDFLAGS  = $(LDFLAGS)"
	@echo "CC       = $(CC)"

# suckless style: config.def.h holds the defaults, config.h is yours
config.h:
	cp config.def.h $@

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJ): $(HDR)

kache: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)

kache-bench: test/bench.c test/metric.h config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test/bench.c $(LDFLAGS)

kache-micro: test/micro.c test/metric.h $(COREOBJ) config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test/micro.c $(COREOBJ) $(LDFLAGS)

kache-cmp: test/cmp.c config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test/cmp.c

bench: kache $(TOOLS)

# run the suite and, if bench/baseline.txt exists, diff against it
benchmark: bench
	sh test/bench.sh

# record the current tree as the thing future runs are measured against
baseline: bench
	sh test/bench.sh -t baseline -f

check: kache
	sh test/test.sh

# a build with the sanitizers on, for running check and bench against
debug:
	$(MAKE) clean
	$(MAKE) CFLAGS_OPT="-O1 -g -fno-omit-frame-pointer \
	    -fsanitize=address,undefined" \
	    LDFLAGS="-pthread -fsanitize=address,undefined"

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f kache $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/kache
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	cp -f doc/kache.1 $(DESTDIR)$(MANPREFIX)/man1/kache.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/kache.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/kache
	rm -f $(DESTDIR)$(MANPREFIX)/man1/kache.1

clean:
	rm -f kache $(TOOLS) $(OBJ)

distclean: clean
	rm -f config.h

.PHONY: all options bench benchmark baseline check debug install uninstall clean distclean
