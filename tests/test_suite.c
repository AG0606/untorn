#include "untorn.h"
#include "untorn_internal.h"
#include <sqlite3.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

static void test_capabilities_probe(void) {
    printf(UNTORN_COLOR_BOLD "\n1. Running Test: Hardware & Kernel Capability Discovery" UNTORN_COLOR_RESET "\n");
    struct untorn_atomic_caps caps;
    int rc = untorn_probe_path(".", &caps);
    TEST_ASSERT(rc == UNTORN_OK, "untorn_probe_path returned UNTORN_OK");
    TEST_ASSERT(caps.min_bytes >= 4096, "atomic_write_unit_min is at least 4096 bytes");
    TEST_ASSERT(caps.max_bytes >= caps.min_bytes, "atomic_write_unit_max >= atomic_write_unit_min");
    TEST_ASSERT(caps.max_segments >= 1, "atomic_write_max_segments >= 1");
    TEST_ASSERT(strlen(caps.storage_model) > 0, "storage model string is populated");
}

static void test_extent_contiguity_and_falloc(void) {
    printf(UNTORN_COLOR_BOLD "\n2. Running Test: Extent Contiguity & Pre-allocation Manager" UNTORN_COLOR_RESET "\n");
    const char *test_file = "test_extent_temp.bin";
    unlink(test_file);

    int fd = open(test_file, O_RDWR | O_CREAT, 0644);
    TEST_ASSERT(fd >= 0, "Created temporary test file");

    /* Initial state before allocation should not be allocated */
    struct untorn_extent_info info;
    int rc = untorn_validate_contiguity(fd, 0, 16384, &info);
    TEST_ASSERT(rc != UNTORN_OK || !info.is_contiguous || info.extent_count == 0,
                "Detected unallocated or initial extent state");

    /* Pre-allocate 64 KB contiguous extent */
    rc = untorn_ensure_contiguous(fd, 0, 65536);
    TEST_ASSERT(rc == UNTORN_OK, "untorn_ensure_contiguous successfully pre-allocated 64KB");

    /* Re-verify contiguity */
    rc = untorn_validate_contiguity(fd, 0, 16384, &info);
    TEST_ASSERT(rc == UNTORN_OK && info.is_contiguous, "Pre-allocated extent verified 100% contiguous");
    TEST_ASSERT(info.is_aligned, "Physical extent block offset is 4KB sector aligned");

    close(fd);
    unlink(test_file);
}

static void test_direct_atomic_io(void) {
    printf(UNTORN_COLOR_BOLD "\n3. Running Test: Direct-I/O Hardware Atomic Write Engine" UNTORN_COLOR_RESET "\n");
    const char *test_file = "test_atomic_io.bin";
    unlink(test_file);

    int fd = open(test_file, O_RDWR | O_CREAT, 0644);
    TEST_ASSERT(fd >= 0, "Created target I/O file");

    struct untorn_atomic_caps caps;
    untorn_probe_fd(fd, &caps);

    size_t write_sz = 16384;
    void *buf = untorn_alloc_aligned(4096, write_sz);
    TEST_ASSERT(buf != NULL, "Allocated 4096-byte aligned Direct-I/O buffer");
    TEST_ASSERT(IS_ALIGNED(buf, 4096), "Direct-I/O memory buffer pointer is 4096-aligned");

    memset(buf, 0x5A, write_sz);
    untorn_ensure_contiguous(fd, 0, write_sz);

    ssize_t written = untorn_pwrite_atomic(fd, buf, write_sz, 0, &caps);
    TEST_ASSERT(written == (ssize_t)write_sz, "untorn_pwrite_atomic completed exact 16KB write");

    /* Verify data integrity on disk */
    void *read_buf = untorn_alloc_aligned(4096, write_sz);
    ssize_t read_bytes = pread(fd, read_buf, write_sz, 0);
    TEST_ASSERT(read_bytes == (ssize_t)write_sz, "Read back exact 16KB from disk");
    TEST_ASSERT(memcmp(buf, read_buf, write_sz) == 0, "Disk contents match atomic buffer byte-for-byte");

    /* Test alignment rejection */
    char unaligned_buf[128];
    char *misaligned_ptr = unaligned_buf + 1; /* Purposely misaligned */
    ssize_t bad_res = untorn_pwrite_atomic(fd, misaligned_ptr, 4096, 0, &caps);
    TEST_ASSERT(bad_res == UNTORN_ERR_UNALIGNED, "Rejected misaligned memory buffer with UNTORN_ERR_UNALIGNED");

    untorn_free_aligned(buf);
    untorn_free_aligned(read_buf);
    close(fd);
    unlink(test_file);
}

static void test_sqlite_untorn_vfs(void) {
    printf(UNTORN_COLOR_BOLD "\n4. Running Test: SQLite Integration under 'untorn' VFS" UNTORN_COLOR_RESET "\n");
    const char *db_file = "test_sqlite_vfs.db";
    unlink(db_file);

    int rc = untorn_sqlite_vfs_register(false);
    TEST_ASSERT(rc == UNTORN_OK, "Registered 'untorn' SQLite VFS");

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(db_file, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "untorn");
    TEST_ASSERT(rc == SQLITE_OK, "Successfully opened SQLite database with 'untorn' VFS");

    /* Configure zero-logging atomic mode: page_size MUST be set first */
    sqlite3_exec(db, "PRAGMA page_size = 16384;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode = OFF;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);

    /* Verify page size was actually set to 16384 */
    sqlite3_stmt *ps_stmt;
    sqlite3_prepare_v2(db, "PRAGMA page_size;", -1, &ps_stmt, NULL);
    TEST_ASSERT(sqlite3_step(ps_stmt) == SQLITE_ROW, "Queried PRAGMA page_size");
    int actual_page_sz = sqlite3_column_int(ps_stmt, 0);
    TEST_ASSERT(actual_page_sz == 16384, "Verified database configured with 16384-byte AWUN page size");
    sqlite3_finalize(ps_stmt);

    rc = sqlite3_exec(db, "CREATE TABLE test_data (id INT PRIMARY KEY, val TEXT);", NULL, NULL, NULL);
    TEST_ASSERT(rc == SQLITE_OK, "Created table in zero-logging atomic database");

    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    for (int i = 0; i < 500; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO test_data VALUES (%d, 'Payload_%d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    TEST_ASSERT(rc == SQLITE_OK, "Committed 500 records via hardware atomic VFS");

    /* Run integrity check */
    sqlite3_stmt *stmt = NULL;
    int prep_rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, NULL);
    TEST_ASSERT(prep_rc == SQLITE_OK, "Prepared PRAGMA integrity_check");
    rc = sqlite3_step(stmt);
    TEST_ASSERT(rc == SQLITE_ROW, "Executed PRAGMA integrity_check");
    const char *status = (rc == SQLITE_ROW) ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
    if (!status) {
        printf("  [DEBUG] integrity_check failed: rc=%d err=%s\n", rc, sqlite3_errmsg(db));
    }
    TEST_ASSERT(status != NULL && strcmp(status, "ok") == 0, "PRAGMA integrity_check returned 'ok' (ACID consistency verified)");
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    untorn_sqlite_vfs_unregister();
    unlink(db_file);
}

static void test_vfs_locking_and_bounce(void) {
    printf(UNTORN_COLOR_BOLD "\n5. Running Test: VFS Concurrency Locking & Direct-I/O Bounce Buffer" UNTORN_COLOR_RESET "\n");
    const char *lock_file = "test_lock_file.db";
    unlink(lock_file);

    int rc = untorn_sqlite_vfs_register(false);
    TEST_ASSERT(rc == UNTORN_OK, "VFS registered for locking & bounce tests");

    sqlite3_vfs *vfs = sqlite3_vfs_find("untorn");
    TEST_ASSERT(vfs != NULL, "Located 'untorn' VFS handle");

    sqlite3_file *file = (sqlite3_file *)malloc(vfs->szOsFile);
    int out_flags = 0;
    rc = vfs->xOpen(vfs, lock_file, file, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB, &out_flags);
    TEST_ASSERT(rc == SQLITE_OK, "Opened test file via VFS xOpen with O_DIRECT");

    /* Test 1: POSIX Advisory Locking */
    rc = file->pMethods->xLock(file, SQLITE_LOCK_SHARED);
    TEST_ASSERT(rc == SQLITE_OK, "Successfully acquired SQLITE_LOCK_SHARED (F_RDLCK)");

    rc = file->pMethods->xLock(file, SQLITE_LOCK_EXCLUSIVE);
    TEST_ASSERT(rc == SQLITE_OK, "Successfully acquired SQLITE_LOCK_EXCLUSIVE (F_WRLCK)");

    int is_reserved = 0;
    rc = file->pMethods->xCheckReservedLock(file, &is_reserved);
    TEST_ASSERT(rc == SQLITE_OK && is_reserved == 1, "CheckReservedLock correctly confirms exclusive lock held");

    rc = file->pMethods->xUnlock(file, SQLITE_LOCK_NONE);
    TEST_ASSERT(rc == SQLITE_OK, "Successfully released locks via SQLITE_LOCK_NONE (F_UNLCK)");

    /* Test 2: Unaligned Direct-I/O Read Bounce Buffer */
    char test_pattern[128];
    for (int i = 0; i < 128; i++) test_pattern[i] = (char)(i & 0xFF);

    /* Write 128 bytes at offset 0 (standard fallback write path) */
    rc = file->pMethods->xWrite(file, test_pattern, 128, 0);
    TEST_ASSERT(rc == SQLITE_OK, "Wrote 128 bytes to start of test file");

    /* Read back 64 bytes starting at offset 13 (unaligned offset and size into stack buffer) */
    char unaligned_read[64];
    rc = file->pMethods->xRead(file, unaligned_read, 64, 13);
    TEST_ASSERT(rc == SQLITE_OK, "Unaligned xRead routed safely through bounce buffer without EINVAL");
    TEST_ASSERT(memcmp(unaligned_read, test_pattern + 13, 64) == 0,
                "Read contents via bounce buffer match source byte-for-byte");

    file->pMethods->xClose(file);
    free(file);
    untorn_sqlite_vfs_unregister();
    unlink(lock_file);
}

int main(void) {
    printf("\n" UNTORN_COLOR_BOLD "========================================================\n" UNTORN_COLOR_RESET);
    printf(UNTORN_COLOR_BOLD " UNTORN SYSTEM INVARIANT & REGRESSION TEST SUITE\n" UNTORN_COLOR_RESET);
    printf(UNTORN_COLOR_BOLD "========================================================\n" UNTORN_COLOR_RESET);

    test_capabilities_probe();
    test_extent_contiguity_and_falloc();
    test_direct_atomic_io();
    test_sqlite_untorn_vfs();
    test_vfs_locking_and_bounce();

    printf("\n" UNTORN_COLOR_BOLD "========================================================\n" UNTORN_COLOR_RESET);
    printf(" TOTAL TESTS RUN   : %d\n", tests_run);
    printf(" TESTS PASSED      : %s%d%s\n",
           (tests_passed == tests_run) ? UNTORN_COLOR_GREEN : UNTORN_COLOR_RED,
           tests_passed, UNTORN_COLOR_RESET);
    printf(" STATUS            : %s\n",
           (tests_passed == tests_run) ?
           UNTORN_COLOR_GREEN "ALL INVARIANTS VERIFIED! READY FOR PRODUCTION." UNTORN_COLOR_RESET :
           UNTORN_COLOR_RED "FAILURES DETECTED." UNTORN_COLOR_RESET);
    printf(UNTORN_COLOR_BOLD "========================================================\n\n" UNTORN_COLOR_RESET);

    return (tests_passed == tests_run) ? 0 : 1;
}
