#include <stdint.h>
#include <sys/types.h>
#include "part.h"

/* we need to map at least this much
 * of the beginning of a disk: */
#define GPT_RESERVE_LBAS (1L + 32L)
#define GPT_RESERVE      (GPT_RESERVE_LBAS << 9)

static inline void
put_le64(unsigned char *dst, int64_t s)
{
    int i;

    for (i=0; i<8; i++) {
	dst[i] = (s&0xff);
	s >>= 8;
    }
}

static inline int64_t
get_le64(const unsigned char *src)
{
    int64_t s = 0;
    int i;

    for (i=7; i>=0; i--) {
	s <<= 8;
	s |= (int64_t)src[i];
    }
    return s;
}

int gpt_add_lastpart(int fd, int num, int64_t numlbas);

int gpt_write_parts(int fd, struct partinfo *parts, const char *diskuuid, int64_t numlbas);
