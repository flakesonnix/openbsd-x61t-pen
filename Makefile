# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra
CPPFLAGS += -I/usr/X11R6/include
LDFLAGS += -L/usr/X11R6/lib
LDLIBS += -lXtst -lXrandr -lX11

PREFIX ?= ${HOME}/.local
BINDIR ?= ${PREFIX}/bin

all: x61-pen

x61-pen: x61-pen.c
	${CC} ${CPPFLAGS} ${CFLAGS} ${LDFLAGS} -o $@ x61-pen.c ${LDLIBS}

tests/integration: tests/integration.c x61-pen.c
	${CC} ${CPPFLAGS} ${CFLAGS} ${LDFLAGS} -o $@ tests/integration.c ${LDLIBS} -lutil

check: x61-pen tests/integration
	./x61-pen --self-test
	/bin/sh tests/check.sh

test: check

install: x61-pen
	install -d -m 755 "${DESTDIR}${BINDIR}"
	install -m 755 x61-pen "${DESTDIR}${BINDIR}/x61-pen"

clean:
	rm -f x61-pen tests/integration

.PHONY: all check test install clean
