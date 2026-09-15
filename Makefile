CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99 -O2
TARGET = touchpad-monitor
SRC = main.c
PREFIX = /usr/local
UNITDIR = /etc/systemd/system

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

debug: CFLAGS += -g -O0
debug: clean all

clean:
	rm -f $(TARGET) *.o

# Run as root.  After installing: systemctl enable --now touchpad-monitor
install: $(TARGET)
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	install -m 644 touchpad-monitor.service $(DESTDIR)$(UNITDIR)/
	-systemctl daemon-reload

uninstall:
	-systemctl disable --now touchpad-monitor
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET) $(DESTDIR)$(UNITDIR)/touchpad-monitor.service
	-systemctl daemon-reload

.PHONY: all clean debug install uninstall
