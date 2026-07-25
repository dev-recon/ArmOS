# Storage Performance

ArmOS uses the same VFS, ext2 and FAT32 implementations on every architecture.
Only the block transport changes: VirtIO block on QEMU `virt`, and the
BCM2835/BCM2837 SD/eMMC controller on Raspberry Pi platforms.

## Measurement Tool

`iobench` exercises storage through the normal POSIX and VFS path:

```sh
iobench -f /tmp/iobench-ext2.dat -m 4 -b 64 -k
/sbin/mount-fat32 /dev/virtio0p2 /mnt
iobench -f /mnt/iobench-fat.dat -m 2 -b 64 -k
```

The normal run writes the file, calls `fsync()`, then reads it twice. The first
read is not necessarily cold because filesystem caches may retain data written
by the same run. A real cold-read test keeps the file, shuts down cleanly,
reboots, and reads without rewriting:

```sh
iobench -f /tmp/iobench-ext2.dat -m 8 -b 64 -k
/sbin/shutdown
# Reboot ArmOS.
iobench -f /tmp/iobench-ext2.dat -r -b 64
```

## QEMU Reference Results

These numbers compare the storage stack before and after the July 2026
writeback work. They are regression references, not hardware specifications.
Host load, QEMU version and cache state affect the result. Measurements below
used QEMU 10.0.2, ARM64, four virtual CPUs, and 64 KiB user I/O blocks.

### Filesystems On QEMU Virt

| Filesystem | Test size | Metric | Before | Current | Change |
|---|---:|---|---:|---:|---:|
| ext2 | 4 MiB | sequential write | 1.74 MiB/s | 4.13 MiB/s | +137% |
| ext2 | 4 MiB | first read | about 19-20 MiB/s | 28.98 MiB/s | about +49% |
| ext2 | 4 MiB | second read | about 19-20 MiB/s | 26.14 MiB/s | about +34% |
| FAT32 | 2 MiB | sequential write | 2.50 MiB/s | 14.59 MiB/s | +484% |
| FAT32 | 2 MiB | first read | about 1.9 MiB/s | 19.80 MiB/s | about 10.4x |
| FAT32 | 2 MiB | second read | about 1.9 MiB/s | 18.86 MiB/s | about 9.9x |

The ext2 write duration fell from 2.287 seconds to 0.968 seconds. The FAT32
write duration fell from 0.800 seconds to 0.137 seconds.

### SD/eMMC Transport On QEMU Raspi3b

This comparison uses the same optimized ext2 code and changes only the emulated
SD transport policy. It therefore isolates the driver direction better than a
comparison with the older filesystem implementation.

| SD mode | Workload | Write | Maximum sectors per block request |
|---|---|---:|---:|
| Legacy reference: 1-bit, CMD17/CMD24 | ext2, 4 MiB | 0.47 MiB/s | 1 |
| Current: 4-bit, CMD18/CMD25 | ext2, 4 MiB | 0.57 MiB/s | 128 |

QEMU's PIO SD model still accounts for every transferred word, so the measured
throughput gain is only about 21%. The reduction in command count should matter
more on hardware, but that must be measured on the Raspberry Pi 3 rather than
inferred from emulation.

### Preliminary Raspberry Pi 3 Results

An 8 MiB `iobench` run on the current ARM64 Raspberry Pi 3 path produced the
following preliminary ext2 figures. The SD-card model was not recorded, so
these are diagnostic observations rather than a release performance promise.

| Pass | Write | First read | Second read |
|---|---:|---:|---:|
| First run after boot | 0.73 MiB/s | 16.84 MiB/s | 18.05 MiB/s |
| Repeated steady runs | 0.50-0.55 MiB/s | 7.87-7.88 MiB/s | 8.15 MiB/s |

The faster first run is consistent with a different initial cache and card
state. Later runs stabilized closely, which is more useful as the current
hardware regression baseline. Writes remain the dominant bottleneck; `fsync`
returned in about 5 ms because dirty ext2 data had already been written through
the synchronous benchmark path before the explicit flush.

## Implemented Optimizations

The generic block layer records requests, sectors, errors, flushes and maximum
request sizes in `/proc/diskstats`.

FAT32 now:

- reads and writes contiguous cluster runs in grouped sector requests;
- avoids whole-file temporary buffers on sequential reads;
- skips read-modify-write for complete cluster overwrites;
- writes only the dirty FAT range and groups updates to both FAT copies;
- exposes allocation and FAT synchronization counters in
  `/proc/fs/fat32/stats`.

Ext2 now:

- keeps a pinned superblock copy instead of reading and writing it for every
  allocation counter update;
- caches dirty data and allocation metadata;
- groups up to 16 contiguous 4 KiB blocks into one 64 KiB writeback request;
- avoids reads before complete block overwrites;
- reuses indirect block mapping state during sequential reads;
- exposes cache, writeback and consistency counters in
  `/proc/fs/ext2/stats`.

The syscall layer uses a bounded 64 KiB bounce buffer and automatically falls
back to smaller buffers under kernel-memory pressure.

The Raspberry Pi SD transports now:

- negotiates the four-bit SD bus with ACMD6;
- use CMD18 and CMD25 for multi-sector transfers;
- terminate Arasan transfers with automatic CMD12 and BCM2835 SDHOST
  transfers with an explicit CMD12;
- serializes complete requests;
- disables multi-block mode permanently after the first transfer failure and
  retries through the validated CMD17/CMD24 path.

On Raspberry Pi 3, enabling Wi-Fi moves the system SD card from the Arasan
controller to BCM2835 SDHOST so the Arasan controller can drive the CYW43455
SDIO radio. The original SDHOST bring-up split every block-layer request into
single-sector CMD17/CMD24 commands. This made `max_write_sectors` report the
64 KiB ext2 request while the controller still issued 128 physical commands.
The SDHOST path now preserves that request as one multi-block command.

`/proc/diskstats` reports both views:

- `read_requests`, `write_requests`, and `max_*_sectors` describe requests
  submitted by the filesystems;
- `transport`, `transport_*_commands`, and `transport_max_*_sectors` describe
  the selected hardware backend and its physical data commands;
- `transport_multiblock_fallbacks` records boots where a multi-block failure
  forced the conservative single-block path.

## Raspberry Pi 3 Hardware Validation

Run this sequence after writing the final image to a real SD card:

```sh
rm -f /tmp/iobench-ext2.dat
iobench -f /tmp/iobench-ext2.dat -m 8 -b 64 -k
cat /proc/diskstats
cat /proc/fs/ext2/stats

/sbin/mount-fat32 /dev/sd0p1 /mnt
rm -f /mnt/iobench-fat.dat
iobench -f /mnt/iobench-fat.dat -m 4 -b 64 -k
cat /proc/diskstats
cat /proc/fs/fat32/stats

vfstest --stress --cross-fs
sync
/sbin/shutdown
```

After reboot, run read-only passes on both retained files. The final hardware
table must record the SD card model and capacity because card controllers vary
substantially.

For a healthy Pi 3 Wi-Fi build, the post-test diagnostics should contain
`transport bcm2835-sdhost`, a non-zero
`transport_max_write_sectors` greater than 1, and
`transport_multiblock_fallbacks 0`.
