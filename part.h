#ifndef __PART_H_
#define __PART_H_
#include <stdio.h>     /* dprintf */
#include <stdlib.h>    /* free */
#include <stdint.h>    /* uint8_t */
#include <sys/types.h> /* off_t */
#include <stdbool.h>

#define warnf(e, ...) dprintf(2, e, __VA_ARGS__)

struct partinfo;

struct partinfo {
    struct partinfo *next; /* next partition */    
    const char *kind;      /* type string (usually "L" or "U"); corresponds to EFI type */
    off_t   srcsz;         /* size of partition image (always <= partsz) */
    int64_t startlba;      /* starting LBA */
    int64_t nsectors;      /* size in sectors */
    int   srcfd;           /* source image */
    int   num;             /* partition number (only valid if !hidden) */
    uint8_t dc;            /* dos partition type; only used for DOS partition tables */
    bool  hidden;          /* area is reserved but not an actual partition */
};

static inline struct partinfo *
last_part(struct partinfo *head)
{
	while (head->next)
		head = head->next;
	return head;
}

static inline void
free_parts(struct partinfo **head)
{
    if (!*head)
	return;
    if ((*head)->next)
	free_parts(&(*head)->next);
    free(*head);
}

/* check_parts() checks a list of partitions
 * for sanity and returns a (negative) error
 * if something looks wrong */
int check_parts(const struct partinfo *head, int64_t nlbas);

#endif
