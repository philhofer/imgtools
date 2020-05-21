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

static int
verbose = 0;

static inline off_t
alignup(off_t off, unsigned bits)
{
    off_t mask = (1ULL << bits)-1;
    return (off + mask) & (~mask);
}

/* write the description of 'part' (for sfdisk) to 'fd'*/
static int
partln(int fd, struct partinfo *part)
{
    /* always suffix sizes with KiB so that they are invariant w.r.t. sector size */
    const char *boot = strcmp(part->kind, "U") ? "-" : "*";
    return dprintf(fd, "%lluKiB %lluKiB %s %s\n",
		   (unsigned long long)part->partoff >> 10,
		   (unsigned long long)part->partsz >> 10,
		   part->kind,
		   boot);
}

static void
sfdisk(char* disk, const char *uuid, struct partinfo *partlist)
{
    char *argv[] = {"sfdisk", disk, "--no-tell-kernel", "--no-reread", NULL};
    int pipefd[2];
    pid_t child;
    int status;
 
    please(pipe(pipefd));
    switch ((child = fork())) {
    case 0:
	please(close(pipefd[1]));
	please(dup2(pipefd[0], 0));
	if (pipefd[0])
	    please(close(pipefd[0]));
	please(execvp(argv[0], argv));
    default:
	break;
    }
    please(child);
    please(close(pipefd[0]));
    please(dprintf(pipefd[1], "label: %s\n", "gpt"));
    please(dprintf(pipefd[1], "label-id: %s\n", uuid));
    while (partlist) {
	please(partln(pipefd[1], partlist));
	if (verbose)
	    partln(1, partlist);
	partlist = partlist->next;
    }
    please(close(pipefd[1]));
    if (waitpid(child, &status, 0) != child)
	err(1, "waitpid(%d)", child);
    if (!WIFEXITED(status)) {
	dprintf(2, "sfdisk exited %d; exiting\n", WEXITSTATUS(status));
	_exit(WEXITSTATUS(status));
    }
}

static void
dosfmt(const char *disk, const char *label, struct partinfo *lst)
{
    unsigned char mbr[512];
    unsigned long sig;
    int fd, e;

    sig = strtoul(label, NULL, 0);
    if (errno)
	err(1, "strtoul(dos label)");
    if (sig > 0xffffffff)
	errx(1, "dos disk label id too large: %lu\n", sig);

    memset(mbr, 0, sizeof(mbr));
    put_le32(mbr + 440, sig);
    please(fd = open(disk, O_WRONLY|O_CLOEXEC));
    if ((e = mbr_write_parts(mbr, lst))) {
	errno = -e;
	err(1, "assembling dos parts");
    }
    /* TODO: mbr[...] = label; */
    if (pwrite(fd, mbr, sizeof(mbr), 0) != 512)
	err(1, "writing mbr");
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
    unsigned long long out;
    char *end;

    out = strtoull(text, &end, 0);
    if (errno)
	err(1, "strtoull(%s)", text);
    if (!*end)
	return out;
    if (*end && !*(end+1))
	return hsize(out, *end);
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
    char *diskname, *contents, *kind, *uuid;
    struct partinfo *head, *tail, *part;
    off_t srcsz, dstsize, off, width, align;
    int srcfd, dstfd, partnum;
    char optc;
    bool dos;

    /* the output ought to be deterministic, so pick a uuid: */
    dos = false;
    uuid = NULL;
    dstsize = 0;
    align = DEFAULT_ALIGN_BITS;
    off = (1ULL << align);
    /* -a = minimum partition alignment (in bits)
     * -s = force output size (in bytes or human-readable form)
     * -b = base address for first partition (in bytes or human-readable form) */
    while ((optc = getopt(argc, argv, "+a:s:b:u:vd")) != -1) {
	switch (optc) {
	case 'd':
	    dos = true;
	    break;
	case 'a':
	    align = atoi(optarg);
	    off = alignup(off, align);
	    dstsize = alignup(dstsize, align);
	    break;
	case 's':
	    dstsize = alignup(parse_size(optarg), align);
	    break;
	case 'b':
	    off = alignup(parse_size(optarg), align);	    
	    break;
	case 'u':
	    uuid = optarg;
	    break;
	case 'v':
	    verbose = 1;
	    break;
	default:
	    errx(1, "unrecognized option %c", optc);
	}
    }
    if (!uuid)
	uuid = dos ? "0x77777777" : "3782C3EE-1C16-F042-82A8-D6A40FB7CFAD";

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
	    if (!dstsize)
		errx(1, "cannot use wildcard part size without -s <size> flag");
	    if (off >= dstsize)
		errx(1, "not enough space for wildcard part");
	    width = srcsz = dstsize - off;
	} else if (contents[0] == '+') {
	    /* empty partition; fixed size */	   
	    srcfd = -1;
	    width = srcsz = alignup(parse_size(++contents), align);
	} else {
	    please(srcfd = open(contents, O_RDONLY|O_CLOEXEC));
	    srcsz = fgetsize(srcfd);
	    if (srcsz == 0)
		errx(1, "can't determine size of %s", contents);
	    width = alignup(srcsz, align);
	}

	part = calloc(1, sizeof(struct partinfo));
	part->kind = kind;
	part->srcfd = srcfd;
	part->srcsz = srcsz;
	part->partoff = off;
	part->partsz = width;
	part->num = partnum++;
	off += width;
	if (tail)
	    tail->next = part;
	else
	    head = part;
	tail = part;
    }
    if (!argc-- || strcmp(*argv++, ""))
	usage();

    /* now we know the full size of the image: */
    off = alignup(off, align);
    if (dstsize && off > dstsize)
	errx(1, "images do not fit in size %llu", (unsigned long long)dstsize);

    please(ftruncate(dstfd, dstsize ? dstsize : off));

    /* ... finally, do the actual work: */
    if (dos)
	dosfmt(diskname, uuid, head);
    else
	sfdisk(diskname, uuid, head);
    while (head) {
	if (head->srcfd >= 0)
	    setpart(dstfd, head->srcfd, head->partoff, head->srcsz);
	head = head->next;
    }
    free_parts(&head);
    please(fsync(dstfd));

    if (!argc)
	return 0;
    execvp(argv[0], argv);
    err(1, "execve");
    return 1;
}
