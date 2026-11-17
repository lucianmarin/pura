CC ?= cc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra

# Install system-wide when run as root (sudo make install), per-user otherwise.
PREFIX ?= $(if $(filter 0,$(shell id -u)),/usr/local,$(HOME)/.local)
BINDIR ?= $(DESTDIR)$(PREFIX)/bin

TARGET = pura
SRC = pura.c

.PHONY: all test bench install uninstall clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

test: $(TARGET)
	./$(TARGET) --test

bench: $(TARGET)
	./$(TARGET) --bench

install: $(TARGET)
	install -d "$(BINDIR)"
	install -m 755 $(TARGET) "$(BINDIR)/$(TARGET)"

uninstall:
	rm -f "$(BINDIR)/$(TARGET)"

clean:
	rm -f $(TARGET)
