CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -std=c99 -pedantic
LDFLAGS ?= -lm

TARGET = fireplace

all: $(TARGET)

$(TARGET): fireplace.c
	$(CC) $(CFLAGS) fireplace.c -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o *.ppm

.PHONY: all clean
