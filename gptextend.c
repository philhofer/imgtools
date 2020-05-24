#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include "filesize.h"
#include "part.h"
#include "gpt.h"

#ifdef static_assert
static_assert(sizeof(loff_t)==sizeof(off_t));
#endif

static void
usage(void)
{
    dprintf(2, "usage: gptextend [-n part] disk\n");
    exit(1);
}

int
main(int argc, char **argv)
{
    const char *disk;
    off_t disksize;
    int fd, part, rc;
    char c;

    part = -1;
    while ((c = getopt(argc, argv, "+n:")) != -1) {
	switch (c) {
	case 'n':
	    part = atoi(optarg);
	    break;
	default:
	    usage();
	}
    }
    argc -= optind;
    argv += optind;
    if (argc <= 0)
	usage();
    disk = argv[0];
    fd = open(disk, O_RDWR|O_CLOEXEC);
    if (fd < 0)
	err(1, "open %s", disk);
    disksize = fgetsize(fd);

    if ((rc = gpt_add_lastpart(fd, part, disksize)) < 0) {
	errno = -rc;
	err(1, "adding partition");
    }
    return 0;
}
