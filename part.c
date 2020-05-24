#define _FILE_OFFSET_BITS 64
#include <stdbool.h>
#include <errno.h>
#include "part.h"

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
