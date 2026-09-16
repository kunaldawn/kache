# kache - build configuration

VERSION = 1.0

# paths
PREFIX    = /usr/local
MANPREFIX = $(PREFIX)/share/man

# compiler
CC = cc

# tune: add -march=native locally for the last few percent
CFLAGS_OPT = -O3 -fno-plt -fomit-frame-pointer
CFLAGS_WARN = -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align \
              -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter

CPPFLAGS = -D_GNU_SOURCE -DVERSION=\"$(VERSION)\" -Isrc -I.
CFLAGS   = -std=c11 -pthread $(CFLAGS_OPT) $(CFLAGS_WARN)
LDFLAGS  = -pthread
LIBS     =
