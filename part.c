#define _FILE_OFFSET_BITS 64
#include <stdbool.h>
#include <errno.h>
#include "part.h"

static inline bool
overlap(const struct partinfo *low, struct partinfo *hi)
{
    return low->partoff >= hi->partoff ||
	low->partoff + low->partsz > hi->partoff;
}

static inline bool
fits(const struct partinfo *part, off_t limit)
{
    return part->partoff+part->partsz <= limit;
}

static void
dump_parts(const struct partinfo *head)
{
    while (head) {
	if (head->hidden)
	    warnf("reserved: %llu + %llu %s\n", head->partoff, head->partsz, head->kind);
	else
	    warnf("p%d: %llu + %llu %s\n", head->num, head->partoff, head->partsz, head->kind);
	head = head->next;
    }
}

int
check_parts(const struct partinfo *head, off_t limit)
{
    const struct partinfo *p, *n;
    int i, err = 0;

    i = 1;
    p = head;
    for (p = head; p; p = p->next) {	
	if (!fits(p, limit)) {
	    warnf("partition %d doesn't fit in %llu\n", p->num, p->num+1, (unsigned long long)limit);
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
