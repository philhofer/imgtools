#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include "part.h"
#include "mbr.h"

#define MBR_PART1_OFFSET 446

static int
mbr_entry(unsigned char *desc, struct partinfo *info)
{
    off_t startsect, numsect;

    assert(!info->hidden);
    assert(info->num > 0 && info->num <= 4);
    if (!info->dc) {
	if (strcmp(info->kind, "L") == 0)
	    info->dc = 0x83;
	else if (strcmp(info->kind, "U") == 0)
	    info->dc = 0xef;
	else
	    info->dc = 0x83;
    }

    /* active: */
    desc[0] = info->dc == 0xef ? 0x80 : 0x00;

    /* start CHS address: invalid */
    desc[1] = 0xfe;
    desc[2] = 0xff;
    desc[3] = 0xff;
    desc[4] = info->dc;

    /* last CHS address: invalid */
    desc[5] = 0xfe;
    desc[6] = 0xff;
    desc[7] = 0xff;

    startsect = info->partoff>>9;
    if (startsect>>32)
	return -ERANGE;
    numsect = info->partsz>>9;
    if (numsect>>32)
	return -ERANGE;

    put_le32(desc + 8, (uint32_t)startsect);
    put_le32(desc + 12, (uint32_t)numsect);
    return 0;
}

int
mbr_write_parts(unsigned char *mbr, struct partinfo *parts)
{
    unsigned char *ptable;
    struct partinfo *head;
    int nrp, err;

    nrp = 0;
    for (head = parts; head; head = head->next) {
	if (head->hidden)
	    continue;
	if (head->num > 4 || head->num <= 0) {
	    warnf("partition %d not valid for DOS\n", head->num);
	    return -EINVAL;
        }
	if (head->next && head->num >= head->next->num) {
	    warnf("bailing: part %d comes before %d\n", head->num, head->next->num);
	    return -EINVAL;
        }
	nrp++;
    }

    if (nrp > 4) {
	warnf("can't write %d partitions to DOS\n", nrp);
	return -EINVAL;
    }
    err = 0;
    nrp = 0;
    ptable = mbr + MBR_PART1_OFFSET;
    for (head = parts; head; head = head->next)
	if (!head->hidden && (err = mbr_entry(ptable + (16 * (head->num - 1)), head)))
	    return err;

    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    return 0;
}

static struct partinfo *
read_mbr_partitions(unsigned char *mbr, int *nparts)
{
    struct partinfo *head, *tail, *part;
    unsigned char *desc;
    const char *kind;
    int i, np;

    if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
	warnf("missing boot record; got bytes %x %x\n", (int)mbr[510], (int)mbr[511]);
	errno = EINVAL;
	return NULL;
    }

    head = tail = NULL;
    *nparts = np = 0;
    for (i=0; i<4; i++) {
	desc = mbr + MBR_PART1_OFFSET + (i*16);
	if (desc[4] == 0)
	    continue;
	switch (desc[4]) {
	case 0x83:
	    kind = "L";
	    break;
	case 0xef:
	    kind = "U";
	    break;
	case 0xee:
	    warnf("warning: part %d looks like a protective MBR...\n", i+1);
	    /* fallthrough */
	default:
	    warnf("unrecognized partition label in p%d: %x\n", i+1, (int)desc[4]);
	    kind = "?";
	    break;
	}
	part = calloc(sizeof(struct partinfo), 1);
	part->partoff = ((off_t)get_le32(desc + 8))<<9;
	part->partsz = ((off_t)get_le32(desc + 12))<<9;
	part->kind = kind;
	part->dc = desc[4];
	part->num = i+1;
	if (part->partoff == 0 || part->partsz == 0)
	    warnf("warning: partition %d might only use CHS addressing\n", i+1);
	if (tail)
	    tail->next = part;
	else
	    head = part;
	tail = part;
	np++;
    }
    *nparts = np;
    return head;
}

int
mbr_add_lastpart(unsigned char *mbr, int num, off_t disksize)
{
    struct partinfo *parts, *part;
    off_t start, size;
    int err, nparts;

    parts = read_mbr_partitions(mbr, &nparts);
    if (parts == NULL || nparts == 0) {
	err = -EINVAL;
	goto done;
    }
    if (nparts == 4) {
	err = -ENOSPC;
	goto done;
    }
    if (num > 0 && nparts+1 != num) {
	warnf("cannot set part %d; %d partitions present\n", num, nparts);
	err = -EINVAL;
	goto done;
    }
    num = nparts+1;

    if ((err = check_parts(parts, disksize)))
	goto done;

    part = last_part(parts);
    start = part->partoff + part->partsz;
    if (start >= disksize) {
	warnf("last partition start %llu won't fit in disk\n", (unsigned long long)start);
	err = -ENOSPC;
	goto done;
    }
    size = disksize - start;
    if (size < (1L << 20)) {
	warnf("final partition size %llu is less than minimum alignment (1M)\n",
	      (unsigned long long)size);
	err = -ENOSPC;
	goto done;
    }
    part->next = calloc(sizeof(struct partinfo), 1);
    part = part->next;
    part->kind = "L";
    part->partoff = start;
    part->partsz = size;
    part->dc = 0x83;
    part->num = num;
    err = mbr_write_parts(mbr, parts);
done:
    free_parts(&parts);
    return err;
}
