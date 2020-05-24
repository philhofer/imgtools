#include <stdint.h>
#include <unistd.h>
#include <err.h>
#include <stdlib.h>
#include <stdio.h>

#include "filesize.h"

static void
usage(void)
{
    const char *usagestr = "usage: alignsize [-a alignbits] [-s sectorbits] [-e extrasize] <file> ...\n" \
	"alignsize prints the size of file(s) or disk(s) in bytes or sectors,\n" \
	"adding an optional addend an alignment\n"			\
	"    -a alignbits     align the result up to alignbits bits\n"	\
	"    -s sectorbits    print the output in sectors, with sectors of width 1<<sectorbits\n" \
	"    -e extrasize     add extrasize bytes to the file size before alignment\n"\
	"    <file> ...       files from which to sum sizes\n";
    dprintf(2, usagestr);
    _exit(1);
}

int
main(int argc, char **argv)
{
    off_t sectors = 0;
    off_t align = 0;
    off_t slack = 0;
    off_t sz;
    char opt;
    
    while ((opt = getopt(argc, argv, "a:s:e:h")) > 0) {
	switch (opt) {
	case 'h':
	    usage();
	    break;
	case 'a':
	    align = (off_t)atoi(optarg);
	    if (align < 0)
		errx(1, "negative alignment %lld", (long long)align);
	    break;
	case 's':
	    sectors = (off_t)atoi(optarg);
	    if (sectors < 0)
		errx(1, "negative sectors %lli", (long long)sectors);
	    break;
	case 'e':
	    slack = (off_t)atoi(optarg);
	    if (slack < 0)
		errx(1, "negative slack %lli", (long long)slack);
	    break;
	default:
	    errx(1, "unexpected option %c\n", opt);
	}
    }

    if (optind >= argc)
	usage();

    sz = 0;
    for (int i=optind; i<argc; i++)
	sz += getsize(argv[i]);

    if (align)
	sz = alignup(sz, align);
    sz += slack;
    if (align)
	sz = alignup(sz, align);
    if (sectors)
	sz >>= sectors;
    printf("%llu\n", (unsigned long long)sz);
}
