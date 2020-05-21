CC ?= gcc
CFLAGS ?= -Wall -Werror -pipe -O1 -g -ffunction-sections -fdata-sections
LDFLAGS ?= -static-pie -g -Wl,--gc-sections

REPO := imgtools
TOOLS := gptimage alignsize dosextend
VERSION ?= 0.2.3

.PHONY: all clean release test
all: $(TOOLS)

gptimage: gptimage.o mbr.o part.o
alignsize: alignsize.o
dosextend: dosextend.o mbr.o part.o

%.o: %.c $(wildcard *.h)
	$(CC) -c $(CFLAGS) $< -o $@

%: %.o
	$(CC) $(LDFLAGS) $^ -o $@

install: $(TOOLS)
	install -D -m 755 -t $(DESTDIR)/bin/ $(TOOLS)

tarball: $(REPO)-$(VERSION).tar.zst
$(REPO)-$(VERSION).tar.zst: $(wildcard *.c)
	git archive --format=tar --prefix=$(REPO)-$(VERSION)/ HEAD | zstd -c -o $@

test: $(TOOLS) $(wildcard test/*)
	@for x in test/??-test*; do echo $$x; ./$$x >/dev/null || exit 1; done

clean:
	$(RM) $(TOOLS) *.o
