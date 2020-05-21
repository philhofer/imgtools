#ifndef __FILESIZE_H_
#define __FILESIZE_H_
#include <unistd.h>
#include <err.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static inline off_t
fgetsize(int fd)
{
    struct stat st;
    off_t out = 0;

    if (fstat(fd, &st) < 0)
	err(1, "fstat %d", fd);
    out = st.st_size;
    if (!out && st.st_rdev) {
	out = ioctl(fd, BLKGETSIZE64, &out) == 0 ? out : 0;
    }
    return out;
}
  
static inline off_t
getsize(const char *path)
{
  off_t out;
  int fd = open(path, O_RDONLY);

  if (fd < 0)
    err(1, "open %s", path);
  out = fgetsize(fd);
  close(fd);
  return out;
}

#endif
