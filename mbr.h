#ifndef __MBR_H_
#define __MBR_H_
#include <stdint.h>
#include <sys/types.h>
#include "part.h"

static inline void
put_le32(unsigned char *dst, uint32_t u)
{
    dst[0] = (u >>  0) & 0xff;
    dst[1] = (u >>  8) & 0xff;
    dst[2] = (u >> 16) & 0xff;
    dst[3] = (u >> 24) & 0xff;
}

static inline uint32_t
get_le32(const unsigned char *src)
{
    uint32_t u;
    u = src[0];
    u |= ((uint32_t)src[1])<<8;
    u |= ((uint32_t)src[2])<<16;
    u |= ((uint32_t)src[3])<<24;
    return u;
}

/* mbr_add_lastpart() appends a partition to 'mbr'
 * that consumes all remaining diskspace
 *
 * the mbr must have at least one and no more than three
 * partitions already active 
 *
 * BUGS: currently ignores EBR parts */
int mbr_add_lastpart(unsigned char *mbr, int part, int64_t numlbas,
		     long long *start, long long *length);

/* mbr_write_parts() writes a list of partitions
 * to mbr as a DOS partition table; it leaves all but
 * the partition table and magic bits untouched
 *
 * BUGS: currently can only write primary partitions */
int mbr_write_parts(unsigned char *mbr, struct partinfo *parts);

#endif
