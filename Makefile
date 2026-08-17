CC ?= cc
VERSION := $(strip $(shell cat VERSION))
CPPFLAGS ?=
CPPFLAGS += -DPANFAN_VERSION=\"$(VERSION)\"
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
FORMAT ?= clang-format
SANITIZER_FLAGS = -O1 -g3 -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer

PREFIX ?= /usr
SBINDIR ?= $(PREFIX)/sbin
UNITDIR ?= $(PREFIX)/lib/systemd/system
SLEEPDIR ?= $(PREFIX)/lib/systemd/system-sleep
MODULESLOADDIR ?= $(PREFIX)/lib/modules-load.d
DOCDIR ?= $(PREFIX)/share/doc/panfan
MANDIR ?= $(PREFIX)/share/man
SLEEPLINK ?= ../../../sbin/panfan
DESTDIR ?=

.PHONY: all clean format format-check install sanitize

all: panfan

panfan: panfan.c VERSION
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

panfan-sanitize: panfan.c VERSION
	$(CC) $(CPPFLAGS) $(SANITIZER_FLAGS) -o $@ $<

sanitize: panfan-sanitize

format:
	$(FORMAT) -i panfan.c

format-check:
	$(FORMAT) --dry-run --Werror panfan.c

clean:
	$(RM) panfan panfan-sanitize

install: panfan
	install -D -m 0755 panfan $(DESTDIR)$(SBINDIR)/panfan
	install -D -m 0644 panfan.service $(DESTDIR)$(UNITDIR)/panfan.service
	install -D -m 0644 panfan.modules-load.conf $(DESTDIR)$(MODULESLOADDIR)/panfan.conf
	install -D -m 0644 panfan.conf.example $(DESTDIR)$(DOCDIR)/panfan.conf.example
	install -D -m 0644 README.md $(DESTDIR)$(DOCDIR)/README.md
	install -D -m 0644 LICENSE $(DESTDIR)$(DOCDIR)/LICENSE
	install -D -m 0644 panfan.8 $(DESTDIR)$(MANDIR)/man8/panfan.8
	install -d $(DESTDIR)$(SLEEPDIR)
	ln -sfn $(SLEEPLINK) $(DESTDIR)$(SLEEPDIR)/panfan
