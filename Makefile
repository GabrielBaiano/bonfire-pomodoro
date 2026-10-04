CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -std=c99 -pedantic
LDFLAGS ?= -lm

TARGET = bonfire
SYMLINK = fireplace

all: $(TARGET)

$(TARGET): fireplace.c
	$(CC) $(CFLAGS) fireplace.c -o $(TARGET) $(LDFLAGS)
	ln -sf $(TARGET) $(SYMLINK)

clean:
	rm -f $(TARGET) $(SYMLINK) *.o *.ppm

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
APPDIR ?= $(PREFIX)/share/applications

install: $(TARGET)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	ln -sf $(TARGET) $(DESTDIR)$(BINDIR)/$(SYMLINK)
	@if [ -d "$(DESTDIR)$(APPDIR)" ]; then \
		install -m 644 packaging/desktop/bonfire.desktop $(DESTDIR)$(APPDIR)/bonfire.desktop; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(SYMLINK)
	rm -f $(DESTDIR)$(APPDIR)/bonfire.desktop

.PHONY: all clean install uninstall

