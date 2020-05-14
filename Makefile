CC ?= gcc
CFLAGS ?= -Wall -pipe -O2 -ffunction-sections -fdata-sections
LDFLAGS ?= -static-pie -Wl,--gc-sections

REPO := imgtools
TOOLS := gptimage alignsize
VERSION ?= 0.2

.PHONY: all clean release test
all: $(TOOLS)

$(TOOLS): $(wildcard *.h)

%: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

install: $(TOOLS)
	install -D -m 755 -t $(DESTDIR)/bin/ $(TOOLS)

tarball: $(REPO)-$(VERSION).tar.zst
$(REPO)-$(VERSION).tar.zst: $(wildcard *.c)
	git archive --format=tar --prefix=$(REPO)-$(VERSION)/ HEAD | zstd -c -o $@

test: $(TOOLS) $(wildcard test/*)
	@for x in test/??-test*; do echo $$x; ./$$x || echo "  >>> FAIL"; done

clean:
	$(RM) $(TOOLS) *.o
