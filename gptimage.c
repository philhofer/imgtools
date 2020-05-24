#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <err.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include "filesize.h"
#include "gpt.h"
#include "mbr.h"
#include "part.h"

#ifndef SEEK_DATA
#define SEEK_DATA 3
#endif

#ifndef SEEK_HOLE
#define SEEK_HOLE 4
#endif

#define DEFAULT_ALIGN_BITS 20 /* 1MiB */
#define DEFAULT_SECTOR_BITS 9 /* 512B */

#define please(expr) do { if ((expr) < 0) err(1, #expr); } while(0)

/* C11 compilers ought to support static_assert() */
#ifdef static_assert
static_assert(sizeof(loff_t) == sizeof(off_t));
#endif

static int verbose = 0;

/* take a byte width 'w' and return
 * its width in lbas, taking care to
 * align the returned number of lbas to 'bits',
 * where bits is at least 9 (one sector) */
static int64_t
lba_align(off_t w, int bits)
{
    assert(bits >= 9);
    return alignup((w+511)>>9, bits-9);
}

static off_t
sectoff(int64_t lba)
{
    off_t b = lba << 9;
    if (b < lba)
	errx(1, "lba %lli overflows off_t", lba);
    return b;
}

static void
dosfmt(int fd, const char *label, struct partinfo *lst)
{
    unsigned char mbr[512];
    unsigned long sig;

    sig = strtoul(label, NULL, 0);
    if (errno)
	err(1, "strtoul(dos label)");
    if (sig > 0xffffffff)
	errx(1, "dos disk label id too large: %lu\n", sig);

    memset(mbr, 0, sizeof(mbr));
    put_le32(mbr + 440, sig);
    if (mbr_write_parts(mbr, lst) < 0)
	err(1, "assembling dos parts");

    if (pwrite(fd, mbr, sizeof(mbr), 0) != 512)
	err(1, "writing mbr");
}

static void
gptfmt(int fd, const char *uuid, struct partinfo *lst, int64_t sectors)
{
    if (gpt_write_parts(fd, lst, uuid, sectors) < 0)
	err(1, "creating GPT");
}

const char *usagestr = \
    "usage: gptimage [-a alignbits] [-b base] [-s size] [-u uuid] disk { contents kind ... } prog ...\n" \
    "    for example:\n" \
    "    $ gptimage -s 8G { efi.img U rootfs.img L } echo done\n";

static void
usage(void)
{
    dprintf(2, usagestr);
    _exit(1);
}

static off_t
hsize(off_t amt, char suff)
{
    switch (suff) {
    case 'B':
	return amt;
    case 'K':
	return amt << 10;
    case 'M':
	return amt << 20;
    case 'G':
	return amt << 30;
    case 'T':
	return amt << 40;
    default:
	errx(1, "bad suffix char %c", suff);
    }
}

/* parse a text string as a size,
 * taking care to observe suffix characters */
static off_t
parse_size(const char *text)
{
    long long out;
    char *end;
    off_t up;

    out = strtoll(text, &end, 0);
    if (errno)
	err(1, "strtoull(%s)", text);
    if (out < 0)
	errx(1, "negative size? %lli", out);
    if (!*end)
	return (off_t)out;
    if (*end && !*(end+1)) {
	up = hsize((off_t)out, *end);
	if (up < out)
	    errx(1, "expression %s overflows long long", text);
	return up;
    }
    errx(1, "couldn't parse %s", text);
}

/* copy srcfd into dstfd starting at offset 'dstoff'
 * for dstfd, not copying more than 'width' bytes */
static void
setpart(int dstfd, int srcfd, off_t dstoff, off_t width)
{
    loff_t srcoff, srcend, shift;

    /* find a section of data within srcfd
     * and copy just that section via copy_file_range(2) */
    srcoff = 0;
    shift = dstoff;
    while (srcoff < width) {	
	srcoff = lseek(srcfd, (off_t)srcoff, SEEK_DATA);
	if (srcoff < 0) {
	    if (errno == ENXIO)
		return; /* inside a hole at the end of the file */
	    err(1, "lseek(SEEK_DATA)");
	}
	if (srcoff == width)
	    break;
	assert(srcoff < width);
	please(srcend = lseek(srcfd, (off_t)srcoff, SEEK_HOLE));
	dstoff = srcoff + shift;
	please(copy_file_range(srcfd, &srcoff, dstfd, &dstoff, (size_t)(srcend - srcoff), 0));
    }
}

int
main(int argc, char * const* argv)
{
    int64_t lba, nsectors, disksectors, trailersectors;
    char *diskname, *contents, *kind, *uuid;
    struct partinfo *head, *tail, *part;
    int srcfd, dstfd, partnum;
    off_t srcsz, align;
    char optc;
    bool dos;

    /* the output ought to be deterministic, so pick a uuid: */
    dos = false;
    uuid = NULL;
    disksectors = 0;
    align = DEFAULT_ALIGN_BITS;
    lba = lba_align(1, DEFAULT_ALIGN_BITS);
    /* -a = minimum partition alignment (in bits)
     * -s = force output size (in bytes or human-readable form)
     * -b = base address for first partition (in bytes or human-readable form) */
    while ((optc = getopt(argc, argv, "+a:s:b:u:vdh")) != -1) {
	switch (optc) {
	case 'd':
	    dos = true;
	    break;
	case 'a':
	    align = atoi(optarg);
	    if (align < 9)
		errx(1, "alignment %d below minimum alignment %d\n", align, 9);
	    if (align < 20)
		warnf("warning: alignment %d below recommended of %d\n", align, 20);
	    lba = alignup(lba, align-9);
	    disksectors = alignup(disksectors, align-9);
	    break;
	case 's':
	    disksectors = lba_align(parse_size(optarg), align);
	    break;
	case 'b':
	    lba = lba_align(parse_size(optarg), align);
	    break;
	case 'u':
	    uuid = optarg;
	    break;
	case 'v':
	    verbose = 1;
	    break;
	case 'h':
	    usage();
	    break;
	default:
	    errx(1, "unrecognized option %c", optc);
	}
    }
    if (!uuid)
	uuid = dos ? "0x77777777" : "3782C3EE-1C16-F042-82A8-D6A40FB7CFAD";

    /* we may need to reserve space at the end of the disk */
    trailersectors = dos ? 0 : GPT_RESERVE_LBAS;

    argc -= optind;
    argv += optind;
    if (argc < 2) usage();
    diskname = argv[0];
    argc--; argv++;

    please(dstfd = open(diskname, O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC, 0644));

    head = tail = NULL;
    partnum = 1;
    while (argc && strcmp(argv[0], "")) {
	if (argc < 2)
	    usage();
	contents = *argv++;
	kind = *argv++;
	argc -= 2;
	if (*contents++ != ' ' || *kind++ != ' ')
	    usage();

	if (strcmp(contents, "*") == 0) {
	    /* empty partiton; wildcard size */
	    srcfd = -1;
	    if (!disksectors)
		errx(1, "cannot use wildcard part size without -s <size> flag");
	    if (lba >= disksectors-trailersectors)
		errx(1, "no space remaining for wildcard partition");
	    nsectors = disksectors - trailersectors - lba;
	    srcsz = sectoff(disksectors - trailersectors - lba);
	} else if (contents[0] == '+') {
	    /* empty partition; fixed size */
	    srcfd = -1;
	    srcsz = parse_size(++contents);
	    nsectors = lba_align(srcsz, align);
	} else {
	    please(srcfd = open(contents, O_RDONLY|O_CLOEXEC));
	    srcsz = fgetsize(srcfd);
	    nsectors = lba_align(srcsz, align);
	}

	part = calloc(1, sizeof(struct partinfo));
	part->kind = kind;
	part->srcfd = srcfd;
	part->srcsz = srcsz;
	part->startlba = lba;
	part->nsectors = nsectors;
	part->num = partnum++;
	lba += nsectors;
	warnf("p%d %lli %lli\n", part->num, part->startlba, part->nsectors);
	if (tail)
	    tail->next = part;
	else
	    head = part;
	tail = part;
    }
    if (!argc-- || strcmp(*argv++, ""))
	usage();

    /* now we know the full size of the image: */
    lba += trailersectors;
    lba = alignup(lba, align-9);

    if (lba < 0)
	errx(1, "lba %lli (overflow somewhere?)", lba);
    if (!disksectors)
	disksectors = lba;
    else if (lba > disksectors)
	errx(1, "images (%lli sectors) do not fit in %lli sectors",
	     (long long)lba, (long long)disksectors);

    please(ftruncate(dstfd, sectoff(disksectors)));

    /* ... finally, do the actual work: */
    if (dos)
	dosfmt(dstfd, uuid, head);
    else
	gptfmt(dstfd, uuid, head, disksectors);
    while (head) {
	if (head->srcfd >= 0)
	    setpart(dstfd, head->srcfd, sectoff(head->startlba), head->srcsz);
	head = head->next;
    }
    free_parts(&head);
    close(dstfd);

    if (!argc)
	return 0;
    execvp(argv[0], argv);
    err(1, "execve");
    return 1;
}
