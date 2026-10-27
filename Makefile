CC ?= cc
PYTHON ?= /usr/bin/python3
NODE ?= node
PKGS = gio-2.0 gio-unix-2.0 json-glib-1.0
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

SOURCES = omapower.c config.c process-watch.c service-watch.c
HEADERS = config.h process-watch.h service-watch.h

omapower: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

tests/test_config: tests/test_config.c config.c config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< config.c $(LDLIBS)

tests/test_process: tests/test_process.c process-watch.c process-watch.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

.PHONY: test clean
test: omapower tests/test_config tests/test_process
	./tests/test_config
	./tests/test_process
	sh tests/test_edit.sh
	$(NODE) tests/test_whitelist.js
	dbus-run-session -- $(PYTHON) tests/test_daemon.py ./omapower

clean:
	rm -f omapower tests/test_config tests/test_process
