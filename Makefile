CC ?= gcc
CFLAGS ?= -Wall -pipe -O2 -ffunction-sections -fdata-sections
LDFLAGS ?= -static-pie -Wl,--gc-sections

TOOL := setparts
VERSION ?= 0.1

.PHONY: all clean release
all: $(TOOL)

setparts: setparts.c
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

install: $(TOOL)
	$(INSTALL) -D -m 755 -t $(DESTDIR)/bin/ $(TOOL)

tarball: $(TOOL)-$(VERSION).tar.zst
$(TOOL)-$(VERSION).tar.zst: $(wildcard *.c)
	git archive --format=tar --prefix=$(TOOL)-$(VERSION)/ HEAD | zstd -c -o $@

clean:
	$(RM) $(TOOL) *.o
