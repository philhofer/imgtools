#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include "part.h"
#include "filesize.h"
#include "mbr.h"
#include "gpt.h"

/* we expect headers to be 92 bytes */
#define GPT_HEADER_SIZE 92
/* we expect partition entries to be 128 bytes */
#define GPT_PART_SIZE   128
/* we create partition tables with 128 entries */
#define GPT_NUM_PARTS   128

#define getf32(mem, name) \
    get_le32((mem)+ __ ## name ## _offset32)

#define setf32(mem, name, val) \
    put_le32((mem)+ __ ## name ## _offset32, val)

#define getf64(mem, name) \
    get_le64((mem) + __ ## name ## _offset64)

#define setf64(mem, name, val) \
    put_le64((mem) + __ ## name ## _offset64, val)

/* commonly-accessed GPT header fields: */
#define __hdrsize_offset32   12    /* header size (should be >= 92) */
#define __hdrcrc_offset32    16    /* crc of header */
#define __thislba_offset64   24    /* lba of this header */
#define __otherlba_offset64  32    /* lba of other header */
#define __firstlba_offset64  40    /* first usable lba */
#define __lastlba_offset64   48    /* last usable lba (inclusive) */
#define __partstart_offset64 72    /* lba of partition entries */
#define __nparts_offset32    80    /* number of partition entries */
#define __psize_offset32     84    /* partition entry size (should be 128) */
#define __partcrc_offset32   88    /* crc of partition entries */

/* commonly-accessed GPT partition entry fields: */
#define __partfirst_offset64 32  /* first lba of partition */
#define __partlast_offset64  40  /* last lba of partition (inclusive) */

static int
xb(uint8_t *o, const char *str)
{
    uint8_t ov;
    int i;

    ov = 0;
    for (i=0; i<2; i++) {
	ov <<= 4;
	if (str[i] >= '0' && str[i] <= '9')
	    ov |= str[i]-'0';
	else if (str[i] >= 'A' && str[i] <= 'F')
	    ov |= str[i]-'A'+10;
	else
	    return (int)str[i];
    }
    *o = ov;
    return 0;
}

/* The first three dash-delimited bits of the GUID
 * are little-endian, and the rest are not,
 * which is why this ends up being so hairy:
 *
 * format:
 *  AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEEEEEE
 *
 * where A, B, and C are little-endian and D and E are big-endian
 * (all hex-encoded)
 */
static int
encode_guid(unsigned char *dst, const char *src)
{
    int j = 0, i = 0;

    if (strnlen(src, 37) != 36)
	return -EINVAL;
    /* first 4-byte word: little-endian */
    j = 3;
    while (i < 8) {
	if (xb(&dst[j--], &src[i]))
	    return -EINVAL;
	i += 2;
    }
    if (src[i++] != '-')
	return -EINVAL;
    /* second 2-byte word: little-endian */
    j = 5;
    while (i < 13) {
	if (xb(&dst[j--], &src[i]))
	    return -EINVAL;
	i += 2;
    }
    if (src[i++] != '-')
	return -EINVAL;
    /* third 2-byte word: little-endian */
    j = 7;
    while (i < 18) {
	if (xb(&dst[j--], &src[i]))
	    return -EINVAL;
	i += 2;
    }
    if (src[i++] != '-')
	return -EINVAL;
    /* fourth 2-byte word: big-endian */
    j = 8;
    while (i < 23) {
	if (xb(&dst[j++], &src[i]))
	    return -EINVAL;
	i += 2;
    }
    if (src[i++] != '-')
	return -EINVAL;
    /* fifth 8-byte word: big-endian */
    while (i < 36) {
	if (xb(&dst[j++], &src[i]))
	    return -EINVAL;
	i += 2;
    }
    return 0;
}

static int
write_part(unsigned char *pstart, struct partinfo *part, const unsigned char *diskguid)
{
    const char *typeguid;
    unsigned char *base;
    uint64_t seed;
    int idx;

    idx = part->num-1;
    assert(idx >= 0);
    base = pstart + GPT_PART_SIZE*idx;

    if (strcmp(part->kind, "L") == 0)
	typeguid = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";
    else if (strcmp(part->kind, "U") == 0)
	typeguid = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
    else
	typeguid = part->kind;

    if (encode_guid(base, typeguid))
	warnf("gpt: warning: couldn't handle weird UUID %s\n", typeguid);

    /* compute this partition UUID deterministically using
     * the partition metadata; the tool output
     * needs to be reproducible */
    seed = 0x49799a933a97c4c2ULL;
    seed = (seed << (part->num&63)) | (seed >> (64-(part->num&63)));
    seed ^= (part->partoff>>20) + part->num;
    put_le64(base + 16, ((uint64_t)get_le64(diskguid))^((uint64_t)get_le64(base))^seed);
    put_le64(base + 24, ((uint64_t)get_le64(diskguid+8))^((uint64_t)get_le64(base+8))^seed);

    setf64(base, partfirst, part->partoff>>9);
    setf64(base, partlast, ((part->partoff + part->partsz)>>9)-1);

    /* for now, no attribute flags or partition name */
    memset(base + 48, 0, GPT_PART_SIZE - 48);
    return 0;
}

static const uint32_t crc32_lut[256] = {
    0,          0x77073096, 0xEE0E612C, 0x990951BA,
    0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
    0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
    0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
    0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
    0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
    0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
    0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
    0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
    0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
    0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
    0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
    0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
    0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
    0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
    0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04,
    0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
    0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E,
    0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
    0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
    0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
    0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
};

static uint32_t
crc32(const unsigned char *mem, size_t sz)
{
    uint32_t o = 0xffffffff;
    while (sz--)
	o = crc32_lut[(o&0xff)^*mem++]^(o>>8);
    return ~o;
}

static void
protect_mbr(unsigned char *mem, off_t disksize)
{
    struct partinfo part = {0};
    int64_t maxsize;

    maxsize = 512ULL * ((1ULL << 32) - 1);
    if (disksize > maxsize)
	disksize = maxsize;

    part.num = 1;
    part.kind = "?";
    part.dc = 0xee;
    part.partoff = 512;
    part.partsz = disksize - 512; /* first sector is used, obviously */
    if (mbr_write_parts(mem, &part))
	warnf("couldn't write protective mbr (size = %llu)\n", (unsigned long long)disksize);
}

/* orig should point to lba 1 (an existing GPT)
 * bup should point to the last GPT_RESERVE bytes of the disk;
 * lastlba should be the lba at which the backup GPT header will land */
static void
backup_gpt(const unsigned char *orig, unsigned char *bup, int64_t lastlba)
{
    unsigned char *base, *partstart;

    base = bup + (GPT_RESERVE - 512);

    /* copy the original gpt header verbatim */
    assert(getf64(orig, partstart) == 2);
    memset(base, 0, 512);
    memcpy(base, orig, GPT_HEADER_SIZE);

    /* update lba pointers and re-crc */
    setf32(base, hdrcrc, 0);
    setf64(base, thislba, lastlba);
    setf64(base, otherlba, 1);
    setf64(base, partstart, lastlba - 32);
    setf32(base, hdrcrc, crc32(base, GPT_HEADER_SIZE));

    if (getf64(orig, otherlba) != lastlba)
	warnf("gpt: warning: GPT doesn't have backup at LBA %llu\n", (unsigned long long)lastlba);

    /* duplicate partitions */
    assert(getf32(orig, nparts) <= GPT_NUM_PARTS);
    partstart = bup + (GPT_RESERVE - 33 * 512);
    memcpy(partstart, orig + 512, GPT_PART_SIZE * getf32(orig, nparts));
}

int
gpt_write_parts(int fd, struct partinfo *parts, const char *diskguid, off_t disksize)
{
    unsigned char header[GPT_RESERVE + 512];
    unsigned char trailer[GPT_RESERVE];
    struct partinfo *head;
    unsigned char *base;
    int64_t lastlba;
    int i;

    /* disks must be sector-aligned */
    disksize = aligndown(disksize, 9);
    lastlba = (disksize>>9)-1;

    /* 2048 is the first 1M-aligned lba, and we need a 34-lba trailer */
    if (lastlba <= 2048+GPT_RESERVE_LBAS) {
	warnf("gpt: disk too small (%llu) to retain sane partition alignment\n", disksize);
	return -ENOSPC;
    }

    memset(header, 0, sizeof(header));
    memset(trailer, 0, sizeof(trailer));

    /* base is lba 1 */
    base = header + 512;
    memcpy(base, "EFI PART", 8);
    base[8 + 2] = 1; /* version = 1 */ 

    setf32(base, hdrsize, GPT_HEADER_SIZE);
    setf64(base, thislba, 1);
    setf64(base, otherlba, lastlba);
    setf64(base, firstlba, 2048); /* always reserve 1M to retain filesystem alignment */
    setf64(base, lastlba, lastlba - GPT_RESERVE_LBAS);

    if (encode_guid(base + 56, diskguid)) {
	warnf("gpt: bad disk guid %s\n", diskguid);
	return -EINVAL;
    }

    setf64(base, partstart, 2); /* partitions are in lba 2-33*/
    setf32(base, nparts, GPT_NUM_PARTS);
    setf32(base, psize, GPT_PART_SIZE);

    /* write partition entries */
    i = 0;
    for (head = parts; head; head = head->next) {
	if (head->hidden)
	    continue;
	if (head->num != ++i) {
	    warnf("gpt: expected part %d but got part %d\n", i, head->num);
	    return -EINVAL;
	}
	if (head->num > GPT_NUM_PARTS) {
	    warnf("gpt: partition number %d above highest supported number %d\n", head->num, GPT_NUM_PARTS);
	    return -EOPNOTSUPP;
	}
	if (head->partoff < (2048 << 9)) {
	    warnf("gpt part %d starts at %llu (below first usable offset %llu)\n",
		  head->num, (unsigned long long)head->partoff, (unsigned long long)(2048 << 9));
	    return -EINVAL;
	}
	/* there are filesystem compatibility problems with
	 * unaligned filesystems; try to encourage 1MB+ alignment: */
	if (head->partoff & ((1UL<<20)-1))
	    warnf("warning: gpt part %d not aligned to 1MB boundary\n", head->num);

	if (write_part(base + 512, head, base + 56) < 0) {
	    warnf("bad partition spec %d\n", head->num);
	    return -EINVAL;
	}
    }

    /* compute crc of partition entries, then crc of header */    
    setf32(base, partcrc, crc32(base + 512, GPT_PART_SIZE * GPT_NUM_PARTS));
    setf32(base, hdrcrc, crc32(base, 92));

    protect_mbr(header, disksize);
    backup_gpt(base, trailer, lastlba);

    if (pwrite(fd, header, sizeof(header), 0) != sizeof(header))
	err(1, "pwrite");
    if (pwrite(fd, trailer, sizeof(trailer), disksize - GPT_RESERVE) != sizeof(trailer))
	err(1, "pwrite");
    return 0;
}

static const unsigned char zeroguid[16];

int
gpt_add_lastpart(int fd, int num, off_t disksize)
{
    unsigned char header[GPT_RESERVE + 512];
    unsigned char trailer[GPT_RESERVE];
    unsigned char *partbase, *gpt, *p;
    struct partinfo spec = {0};
    uint32_t c, c2, np;
    off_t start, width;
    int64_t lba, off, blba;
    int i, pfree;

    disksize = aligndown(disksize, 9);
    if (pread(fd, header, sizeof(header), 0) != sizeof(header))
	err(1, "pread");

    memset(trailer, 0, sizeof(trailer));
    blba = (disksize >> 9)-1;

    gpt = header + 512;
    if (memcmp(gpt, "EFI PART", 8))
	return -EINVAL;
    if (getf32(gpt, hdrsize) != 92) {
	warnf("gpt: header size %lu?\n", (unsigned long)getf32(gpt, hdrsize));
	return -EINVAL;
    }
    c = getf32(gpt, hdrcrc);
    setf32(gpt, hdrcrc, 0);
    c2 = crc32(gpt, 92);
    setf32(gpt, hdrcrc, c);
    if (c != c2) {
	warnf("gpt: primary header has invalid crc %lxu\n", c);
	/* TODO: handle backup */
	return -EINVAL;
    }
    /* TODO: the spec says sizes could be 256, 512, ... but no one appears to do that */
    if (getf32(gpt, psize) != GPT_PART_SIZE) {
	warnf("gpt: don't know how to parse partition entries of size %lu\n", (unsigned long)getf32(gpt, psize));
	return -EINVAL;
    }
    np = getf32(gpt, nparts);
    if (np == 0) {
	warnf("gpt: partition entry array is size %u\n", np);
	return -EINVAL;
    }
    if (np > 128) {
	warnf("gpt: not prepared for %d partition entries", np);
	return -EOPNOTSUPP;
    }
    lba = getf64(gpt, partstart); 
    if (lba != 2) {
	warnf("gpt: primary GPT header has partitions starting at %ll?\n", lba);
	return -EINVAL;
    }
    lba = getf64(gpt, firstlba);
    if (lba < 2 + (alignup(np*GPT_PART_SIZE, 9)>>9)) {
	warnf("gpt: partition table entries (%lu) bleed into first usable lba\n", np);
	return -EINVAL;
    }

    partbase = gpt + 512;
    if (crc32(partbase, np*GPT_PART_SIZE) != getf32(gpt, partcrc)) {
	warnf("gpt: crc error (%xu) for partitions\n", getf32(gpt, partcrc));
	return -EINVAL;
    }

    /* now figure out where we can insert the last (first?) partition */
    off = getf64(gpt, firstlba) << 9;
    /* index of first unused slot after all valid partitions */
    pfree = 0;
    for (i=0; i<np; i++) {
	p = partbase + GPT_PART_SIZE*i;
	if (memcmp(p, zeroguid, sizeof(zeroguid)) == 0)
	    continue;
        start = getf64(p, partfirst) << 9;
	width = ((getf64(p, partlast)+1) << 9) - start;
	if (width < 0 || start < 0) {
	    warnf("gpt: part %d: strange bounds [%ll, %ll]\n", getf64(p, firstlba), getf64(p, lastlba));
	    return -EINVAL;
	}
	if (start < off || start+width < off) {
	    warnf("gpt: part %d: overlapping / not in disk order? (%llu, %llu)\n",
		  i+1, (unsigned long long)start, (unsigned long long)width);
	    return -EINVAL;
	}
	pfree = i+1;
	off = start+width;
    }
    if (num > 0 && pfree != num-1) {
	warnf("gpt: found available part %d; not equal to expected part %d\n", pfree+1, num);
	return -ENXIO;
    }
    if (pfree >= np) {
	warnf("gpt: all partition entries (%d) already used\n", np);
	return -ENXIO;
    }
    off = alignup(off, 20);
    if (off >= disksize || disksize-off-GPT_RESERVE < (1UL<<20))
	return -ENOSPC;
    spec.partoff = off;
    spec.partsz = disksize - off - GPT_RESERVE;
    spec.kind = "L";
    spec.num = pfree+1;

    /* now actually do the modification */
    if (getf64(gpt, otherlba) != blba)
	warnf("gpt: note: moving backup GPT LBA %ll -> %ll\n", getf64(gpt, otherlba), blba);
    else
	warnf("gpt: note: backup GPT at LBA %ll will be overwritten\n", blba);

    setf64(gpt, otherlba, blba);
    write_part(partbase, &spec, gpt + 56);
    setf32(gpt, hdrcrc, 0);
    setf32(gpt, partcrc, crc32(partbase, GPT_PART_SIZE*np));
    setf32(gpt, hdrcrc, crc32(gpt, 92));
    backup_gpt(gpt, trailer, blba);

    /* only update PMBR if one is actually present */
    if (header[510] == 0x55 && header[511] == 0xaa)
	protect_mbr(header, disksize);

    if (pwrite(fd, header, sizeof(header), 0) != sizeof(header))
	err(1, "pwrite");
    if (pwrite(fd, trailer, sizeof(trailer), disksize - GPT_RESERVE) != sizeof(trailer))
	err(1, "pwrite");
    return 0;
}