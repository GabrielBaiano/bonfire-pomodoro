CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -std=c99 -pedantic
LDFLAGS ?= -lm

TARGET = fireplace

all: $(TARGET)

$(TARGET): fireplace.c
	$(CC) $(CFLAGS) fireplace.c -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o *.ppm

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

install: $(TARGET)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	ln -sf $(TARGET) $(DESTDIR)$(BINDIR)/bonfire

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(BINDIR)/bonfire

.PHONY: all clean install uninstall

