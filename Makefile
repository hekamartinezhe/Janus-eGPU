# egpu-session-daemon Makefile
# =============================
# make              - compile the binary
# make install      - install to /usr/local/bin (requires root)
# make uninstall    - remove the installed binary
# make clean        - clean compiled files

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wpedantic
LDFLAGS = -lsystemd

PREFIX     = /usr/local
BINDIR     = $(PREFIX)/bin
SERVICEDIR = /etc/systemd/system

TARGET  = egpu-session-daemon
SRC     = egpu-session-daemon.c

.PHONY: all install install-service uninstall uninstall-service clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

install-service: egpu-session-daemon@.service
	install -Dm644 egpu-session-daemon@.service $(DESTDIR)$(SERVICEDIR)/egpu-session-daemon@.service

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall-service:
	rm -f $(DESTDIR)$(SERVICEDIR)/egpu-session-daemon@.service

clean:
	rm -f $(TARGET)
