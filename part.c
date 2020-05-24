#define _FILE_OFFSET_BITS 64
#include <stdbool.h>
#include <errno.h>
#include "part.h"

#ifdef __linux__
#include <sys/ioctl.h>
#define BLKPG_ADD_PARTITION 1
int
kernel_add_part(int fd, int pnum, long long start, long long length)
{
    struct {
	int op;
	int flags;
	int datalen;
	void *data;
    } arg = {0};
    struct {
	long long start;
	long long length;
	int pno;
	char devname[64];
	char volname[64];
    } part = {0};

    part.pno = pnum;
    part.start = start;
    part.length = length;
    arg.op = BLKPG_ADD_PARTITION;
    arg.datalen = sizeof(part);
    arg.data = (void *)&part;
    return ioctl(fd, _IO(0x12, 105), &arg);
}
#else
int
kernel_add_part(int fd, int pnum, long long start, long long length)
{
    errno = EOPNOTSUPP;
    return -1;
}
#endif

static inline bool
overlap(const struct partinfo *low, struct partinfo *hi)
{
    return low->startlba >= hi->startlba ||
	low->startlba + low->nsectors > hi->startlba;
}

static inline bool
fits(const struct partinfo *part, int64_t nlbas)
{
    return part->startlba+part->nsectors <= nlbas;
}

static void
dump_parts(const struct partinfo *head)
{
    while (head) {
	if (head->hidden)
	    warnf("reserved: %ll + %ll %s\n", head->startlba, head->nsectors, head->kind);
	else
	    warnf("p%d: %ll + %ll %s\n", head->num, head->startlba, head->nsectors, head->kind);
	head = head->next;
    }
}

int
check_parts(const struct partinfo *head, int64_t nlbas)
{
    const struct partinfo *p, *n;
    int i, err = 0;

    i = 1;
    p = head;
    for (p = head; p; p = p->next) {	
	if (!fits(p, nlbas)) {
	    warnf("partition %d doesn't fit in %ll\n", p->num, p->num+1, (long long)nlbas);
	    err = -EINVAL;
	}
	if (p->next && overlap(p, p->next)) {
	    warnf("partition %d overlaps with next parition\n", p->num);
	    err = -EINVAL;
	}
	if (!p->hidden) {
	    for (n = p->next; n; n = n->next) {
		if (!n->hidden && p->num >= n->num) {
		    warnf("partition numbers (%d, %d) out-of-order\n", p->num, n->num);
		    err = -EINVAL;
		    break;
		}
	    }
	}
	i++;
    }

    if (err)
	dump_parts(head);
    return err;
}
