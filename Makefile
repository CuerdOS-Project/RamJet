# RamJet - Rice clone in C
# Modified 2026-09-03: bug fixes and conservative robustness improvements.

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -std=c11 -O2
CPPFLAGS ?= -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS  ?=

SRCDIR  = src
SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/ramjet.c \
          $(SRCDIR)/rule.c \
          $(SRCDIR)/cgroup.c \
          $(SRCDIR)/proc_type.c \
          $(SRCDIR)/parse.c \
          $(SRCDIR)/class.c
OBJECTS = $(SOURCES:.c=.o)
TARGET  = ramjet
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

.PHONY: all clean install uninstall debug test analyze

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(OBJECTS) $(TARGET) tests/test_core

debug: CFLAGS += -g -DDEBUG -O0
debug: clean all

test: $(TARGET) tests/test_core
	./tests/test_core

tests/test_core: tests/test_core.c $(OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dmain=ramjet_main -c -o /tmp/ramjet_main_test.o src/main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< src/parse.o src/class.o src/proc_type.o src/rule.o src/cgroup.o src/ramjet.o /tmp/ramjet_main_test.o $(LDLIBS)

analyze:
	clang --analyze $(CPPFLAGS) $(CFLAGS) $(SOURCES)
