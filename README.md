# Disk Image Tools

An assortment of tools for producing disk images.

These tools are designed to work on both real
disk device nodes and on sparse files.

## gptimage

The `gptimage` tool produces GPT- or DOS-formatted
disk images. It is designed to be used with the
[execline](https://skarnet.org/software/execline) scripting language.

Usage:

```
gptimage [-d] [-a align] [-s size] [-u label] [-b base] { partitions ... } prog ...
```

Command line arguments:

 * `-d`: use DOS instead of the default partitioning scheme (GPT)
 * `-a bits`: use `bits` as the alignment for partitions (default 20, or 1M)
 * `-s size`: use `size` as the size of the disk; the default is to make
   the image as small as possible while still preserving alignment
 * `-u label`: use `label` as the disk lable; either a 4-byte hex number
   for DOS or a GPT UUID for GPT
 * `-b base`: use `base` as the lowest available offset for partitions

The `{ partitions ... }` spec are pairs of content-plus-type indicators.
The "content" can be specified by a file name, or it can be the character
'+' plus a size to indicate an empty partition of a particular size.
As a special case, "content" can be `*` when specifying the last partition,
which indicates that the partition should consume the rest of the image.
The "type" can be the literal characters `L` or `U`, which
mean Linux filesystem data and EFI System Partition, respectively, or
it can be a literal GPT partition type UUID.

For example:

```
#!/bin/execlineb -P

# create a DOS (-d) disk0.img of size 16G (-s 16G)
# with a disk label of 0xdeadbeef
gptimage -d -s 16G -u 0xdeadbeef disk0.img {
	 +32M       L # p1: empty, 32M in size, Linux filesystem partition type
	 rootfs.img L # p2: contents should come from rootfs.img
	 *          L # p3: rest of the disk, no contents
}

# create disk1.img, sized automatically, using GPT, with
# a UEFI ESP and an ordinary Linux filesystem partition
gptimage disk1.img {
	 efi.img  U # p1: UEFI EFI system partition, using efi.img
	 root.img L # p2: Linux filesystem data, from root.img
}
```

Note that GPT disks will be formatted with a protective MBR,
so tools that only recognize DOS partitions will see the disk
as containing a single partition of type `0xee`.

## `dosextend` and `gptextend`

The `dosextend` and `gptextend` tools append partitions to their
respective partitioning schemes.

These help simplify deployment of variable-sized disk images
(often in virtualized environments) where the size of the target disk
isn't known in advance.

For example, if you use `gptimage` to produce a 128M image with two
partitions, and then run `truncate -s 1G my.img && ./gptextend my.img`,
the disk will now have three partitions with the third partition consuming
the remaining disk space. (The [distill](https://git.sr.ht/~pmh/distill)
project uses this trick for creating the `/var` partition in virtualized
environments.)

Usage:

```
gptextend [-n num] [-k] <file-or-disk>
dosextend [-n num] [-k] <file-or-disk>
```

Command line options (for both tools):

 * `-n num`: Expect to create partition `num`, and fail otherwise.
 * `-k`: Inform the kernel of the new partition. This is only useful
   if the target is actually a disk device node. You likely don't
   need this option unless one of the paritions on this disk is
   already mounted.

## `alignsize`

The `alignsize` tool prints file sizes in various units
and alignments.

Command line options:

 * `-a align`: align the result to `2^align` bits
 * `-s sector`: right-shift the result by `sector`
 * `-e addend`: add `addend` to the result before alignment

Usage:

```
alignsize [-a align] [-s sector] [-e addend] files...
```

For example:
```
# print the size of 'foo.img' rounded up to mebibytes in 512-byte sectors:
$ alignsize -a20 -s9 foo.img
# print the size of 'foo.img' in mebibytes, rounded up to the nearest mebibyte:
$ alignsize -a20 -s20 foo.img
# round up the sum of the sizes of a bunch of files to mebibytes:
$ find . | xargs alignsize -a20
```
