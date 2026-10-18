#ifndef UNTORN_H
#define UNTORN_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Kernel / Hardware Atomic Write constants introduced in Linux 6.11 / 6.12.
 * Defined conditionally so code compiles on older toolchains/headers as well.
 */
#ifndef STATX_WRITE_ATOMIC
#define STATX_WRITE_ATOMIC 0x00010000U
#endif

#ifndef RWF_ATOMIC
#define RWF_ATOMIC 0x00000040
#endif

#ifndef RWF_DSYNC
#define RWF_DSYNC 0x00000002
#endif

/*
 * Structure representing hardware and filesystem atomic write capabilities.
 * Discovered via statx(STATX_WRITE_ATOMIC) or fallback sysfs inspection.
 */
struct untorn_atomic_caps {
    uint32_t min_bytes;         /* Minimum atomic write size (e.g. 4096) */
    uint32_t max_bytes;         /* Maximum atomic write size (e.g. 16384 or 65536) */
    uint32_t max_segments;      /* Maximum scatter-gather segments (usually 1) */
    bool     supported;         /* True if hardware/filesystem supports atomic writes */
    bool     emulated;          /* True if operating under userspace software verification emulation */
    char     storage_model[64]; /* Discovered device model or emulation tag */
};

/*
 * Structure describing physical block extent layout returned by the
 * Extent Contiguity Validator (FIEMAP).
 */
struct untorn_extent_info {
    uint64_t logical_offset;    /* Logical file offset (bytes) */
    uint64_t physical_offset;   /* Physical block device byte offset */
    uint64_t extent_length;     /* Extent length in bytes */
    uint32_t extent_count;      /* Number of physical extents spanning this range */
    bool     is_contiguous;     /* True if the byte range lies within a single contiguous extent */
    bool     is_aligned;        /* True if physical offset is aligned to atomic_unit_min */
};

/*
 * Result codes for Untorn operations.
 */
enum untorn_status {
    UNTORN_OK                  =  0,
    UNTORN_ERR_PROBE_FAILED    = -1,
    UNTORN_ERR_NOT_SUPPORTED   = -2,
    UNTORN_ERR_UNALIGNED       = -3,
    UNTORN_ERR_DISCONTIGUOUS   = -4,
    UNTORN_ERR_SIZE_MISMATCH   = -5,
    UNTORN_ERR_IO_FAILED       = -6,
    UNTORN_ERR_FALLOC_FAILED   = -7
};

/*
 * ============================================================================
 * Phase 1: Hardware & Kernel Capability Discovery
 * ============================================================================
 */

/*
 * Query the atomic write capabilities of the filesystem/block device backing fd.
 */
int untorn_probe_fd(int fd, struct untorn_atomic_caps *caps);

/*
 * Query the atomic write capabilities of a given path.
 */
int untorn_probe_path(const char *path, struct untorn_atomic_caps *caps);

/*
 * Print human-readable summary of discovered atomic write capabilities.
 */
void untorn_print_caps(const struct untorn_atomic_caps *caps);

/*
 * ============================================================================
 * Phase 2: Extent Contiguity Validation & Physical Reservation
 * ============================================================================
 */

/*
 * Inspect physical block extent layout for [offset, offset + length) using
 * FS_IOC_FIEMAP ioctl on ext4/XFS filesystems.
 */
int untorn_validate_contiguity(int fd, off_t offset, size_t length,
                               struct untorn_extent_info *info);

/*
 * Ensure the file range [offset, offset + length) is backed by physically
 * contiguous extents. If unallocated or fragmented, pre-allocates contiguous
 * blocks via fallocate() to prevent blk-mq bio splitting.
 */
int untorn_ensure_contiguous(int fd, off_t offset, size_t length);

/*
 * ============================================================================
 * Phase 3: Direct Hardware-Atomic I/O Execution
 * ============================================================================
 */

/*
 * Allocate an aligned buffer suitable for Direct-I/O (O_DIRECT) atomic writes.
 */
void *untorn_alloc_aligned(size_t alignment, size_t size);

/*
 * Free an aligned buffer.
 */
void untorn_free_aligned(void *ptr);

/*
 * Execute a hardware atomic write directly to the storage controller using
 * pwritev2(..., RWF_ATOMIC | RWF_DSYNC) with Direct-I/O semantics.
 *
 * Verifies extent contiguity and alignment before submission to guarantee
 * blk-mq will never invoke bio_split().
 */
ssize_t untorn_pwrite_atomic(int fd, const void *buf, size_t count, off_t offset,
                             const struct untorn_atomic_caps *caps);

/*
 * ============================================================================
 * Phase 4: SQLite Custom VFS ("untorn-vfs")
 * ============================================================================
 */

/*
 * Register the Untorn Atomic VFS as the default (or named) SQLite VFS.
 * Reports SQLITE_IOCAP_ATOMIC device capabilities and enforces direct atomic
 * page commits. When paired with PRAGMA journal_mode = OFF and PRAGMA page_size = 16384,
 * this eliminates both rollback journal and WAL overhead safely.
 */
int untorn_sqlite_vfs_register(bool make_default);

/*
 * Unregister the Untorn Atomic VFS from SQLite.
 */
int untorn_sqlite_vfs_unregister(void);

#ifdef __cplusplus
}
#endif

#endif /* UNTORN_H */
