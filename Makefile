CC ?= cc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra

PREFIX ?= $(shell if [ -d "$(HOME)/.local" ]; then echo "$(HOME)/.local"; else echo /usr/local; fi)
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
