#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/falloc.h>
#include <linux/fiemap.h>
#include "untorn.h"
#include "untorn_internal.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define STRESS_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        g_tests_failed++; \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
    } \
} while(0)

/* Test hole punching and healing at a specific offset in a file of given total_size */
static void test_isolated_hole_case(const char *name, size_t total_size, off_t hole_offset, size_t hole_size) {
    printf("\n--- Sub-test: %s (file=%zu KB, hole=[%ld KB, %ld KB)) ---\n",
           name, total_size / 1024, (long)hole_offset / 1024, (long)(hole_offset + hole_size) / 1024);

    char path[128];
    snprintf(path, sizeof(path), "/var/tmp/untorn_hole_%ld_%zu.bin", (long)hole_offset, hole_size);
    unlink(path);

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    STRESS_ASSERT(fd >= 0, "Created test file");
    if (fd < 0) return;

    /* 1. Allocate initial contiguous file */
    int rc = untorn_ensure_contiguous(fd, 0, total_size);
    STRESS_ASSERT(rc == UNTORN_OK, "Initial pre-allocation contiguous");

    struct untorn_extent_info info;
    rc = untorn_validate_contiguity(fd, 0, total_size, &info);
    STRESS_ASSERT(rc == UNTORN_OK && info.is_contiguous, "Verified initial file is 100% contiguous");

    /* 2. Punch hole */
    int prc = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, hole_offset, hole_size);
    STRESS_ASSERT(prc == 0, "Punched hole via FALLOC_FL_PUNCH_HOLE");

    /* 3. Verify untorn_validate_contiguity detects discontiguity at the hole */
    rc = untorn_validate_contiguity(fd, hole_offset, hole_size, &info);
    STRESS_ASSERT(rc == UNTORN_ERR_DISCONTIGUOUS && info.extent_count == 0,
                  "Hole range detected as UNTORN_ERR_DISCONTIGUOUS with 0 extents");

    /* 4. Verify overall file is also detected as discontiguous */
    rc = untorn_validate_contiguity(fd, 0, total_size, &info);
    STRESS_ASSERT(rc == UNTORN_ERR_DISCONTIGUOUS || !info.is_contiguous,
                  "Full file detected as discontiguous");

    /* 5. Heal the hole using untorn_ensure_contiguous */
    rc = untorn_ensure_contiguous(fd, hole_offset, hole_size);
    STRESS_ASSERT(rc == UNTORN_OK, "untorn_ensure_contiguous healed the hole");

    /* 6. Verify contiguity is restored on the hole range */
    rc = untorn_validate_contiguity(fd, hole_offset, hole_size, &info);
    STRESS_ASSERT(rc == UNTORN_OK && info.is_contiguous && info.is_aligned,
                  "Hole extent restored to contiguous and sector-aligned");

    /* 7. Perform direct atomic write to the restored range */
    struct untorn_atomic_caps caps;
    untorn_probe_fd(fd, &caps);

    size_t write_sz = (hole_size >= 16384) ? 16384 : 4096;
    void *wbuf = untorn_alloc_aligned(4096, write_sz);
    void *rbuf = untorn_alloc_aligned(4096, write_sz);
    memset(wbuf, 0xAB, write_sz);

    ssize_t w = untorn_pwrite_atomic(fd, wbuf, write_sz, hole_offset, &caps);
    STRESS_ASSERT(w == (ssize_t)write_sz, "Atomic write to restored hole succeeded");

    ssize_t r = pread(fd, rbuf, write_sz, hole_offset);
    STRESS_ASSERT(r == (ssize_t)write_sz && memcmp(wbuf, rbuf, write_sz) == 0,
                  "Readback matches written atomic buffer byte-for-byte");

    untorn_free_aligned(wbuf);
    untorn_free_aligned(rbuf);
    close(fd);
    unlink(path);
}

int main(void) {
    printf("====================================================================\n");
    printf(" VARIED OFFSET HOLE-PUNCHING & CONTIGUITY RESTORATION TEST SUITE     \n");
    printf("====================================================================\n");

    /* Case 1: Hole at offset 0 (0..16KB in 64KB file) */
    test_isolated_hole_case("Hole at Start (Offset 0)", 64 * 1024, 0, 16384);

    /* Case 2: Hole at offset 16KB (16KB..32KB in 64KB file) */
    test_isolated_hole_case("Hole at 16KB in 64KB", 64 * 1024, 16384, 16384);

    /* Case 3: Hole at offset 32KB (32KB..48KB in 128KB file) */
    test_isolated_hole_case("Hole at 32KB in 128KB", 128 * 1024, 32768, 16384);

    /* Case 4: Hole at offset 48KB (48KB..64KB in 128KB file) */
    test_isolated_hole_case("Hole at 48KB in 128KB", 128 * 1024, 49152, 16384);

    /* Case 5: 32KB Hole at offset 64KB (64KB..96KB in 256KB file) */
    test_isolated_hole_case("32KB Hole at 64KB in 256KB", 256 * 1024, 65536, 32768);

    /* Case 6: Hole at End of File (240KB..256KB in 256KB file) */
    test_isolated_hole_case("Hole at End of File (240KB in 256KB)", 256 * 1024, 245760, 16384);

    /* Case 7: 4KB sub-block hole at offset 32KB in 64KB file */
    test_isolated_hole_case("4KB Sub-Block Hole at 32KB", 64 * 1024, 32768, 4096);

    printf("\n====================================================================\n");
    printf(" HOLE-PUNCHING SUITE RESULTS: %d / %d PASSED (%d FAILED)\n",
           g_tests_passed, g_tests_run, g_tests_failed);
    printf("====================================================================\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
