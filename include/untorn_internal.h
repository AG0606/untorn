#ifndef UNTORN_INTERNAL_H
#define UNTORN_INTERNAL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "untorn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <linux/fiemap.h>
#include <time.h>

/* ANSI Colors for Logging */
#define UNTORN_COLOR_RESET  "\033[0m"
#define UNTORN_COLOR_RED    "\033[1;31m"
#define UNTORN_COLOR_GREEN  "\033[1;32m"
#define UNTORN_COLOR_YELLOW "\033[1;33m"
#define UNTORN_COLOR_CYAN   "\033[1;36m"
#define UNTORN_COLOR_BOLD   "\033[1m"

#define UNTORN_LOG_INFO(fmt, ...) \
    fprintf(stdout, UNTORN_COLOR_CYAN "[UNTORN INFO] " UNTORN_COLOR_RESET fmt "\n", ##__VA_ARGS__)

#define UNTORN_LOG_WARN(fmt, ...) \
    fprintf(stderr, UNTORN_COLOR_YELLOW "[UNTORN WARN] " UNTORN_COLOR_RESET fmt "\n", ##__VA_ARGS__)

#define UNTORN_LOG_ERR(fmt, ...) do { \
    if (errno != 0) { \
        fprintf(stderr, UNTORN_COLOR_RED "[UNTORN ERROR] " UNTORN_COLOR_RESET fmt " (errno=%d: %s)\n", \
                ##__VA_ARGS__, errno, strerror(errno)); \
        errno = 0; \
    } else { \
        fprintf(stderr, UNTORN_COLOR_RED "[UNTORN ERROR] " UNTORN_COLOR_RESET fmt "\n", ##__VA_ARGS__); \
    } \
} while (0)

#define UNTORN_LOG_SUCCESS(fmt, ...) \
    fprintf(stdout, UNTORN_COLOR_GREEN "[UNTORN SUCCESS] " UNTORN_COLOR_RESET fmt "\n", ##__VA_ARGS__)

/* Power-of-2 alignment check */
#define IS_POWER_OF_TWO(v) ((v) != 0 && (((v) & ((v) - 1)) == 0))
#define IS_ALIGNED(val, align) (((uintptr_t)(val) & ((align) - 1)) == 0)

/* High-resolution timer */
__attribute__((unused))
static inline double untorn_get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

/*
 * Definition of struct untorn_statx matching Linux 6.12+ ABI
 */
struct untorn_statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  __statx_pad1;
};

struct untorn_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __statx_pad1[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct untorn_statx_timestamp stx_atime;
    struct untorn_statx_timestamp stx_btime;
    struct untorn_statx_timestamp stx_ctime;
    struct untorn_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align;
    uint32_t stx_dio_offset_align;
    uint64_t stx_subvol;
    uint32_t stx_atomic_write_unit_min;
    uint32_t stx_atomic_write_unit_max;
    uint32_t stx_atomic_write_max_segments;
    uint32_t __statx_pad2[1];
    uint64_t __statx_pad3[12];
};

#endif /* UNTORN_INTERNAL_H */
