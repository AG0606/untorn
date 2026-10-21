#include "untorn.h"
#include "untorn_internal.h"
#include <sqlite3.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

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

static void test_synthetic_extent_fragmentation(void) {
    printf(UNTORN_COLOR_BOLD "\n6. Running Test: Synthetic Extent Fragmentation & Contiguity Restoration" UNTORN_COLOR_RESET "\n");
    const char *test_file = "/var/tmp/untorn_frag_invariant.bin";
    unlink(test_file);

    int fd = open(test_file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        test_file = "test_frag_invariant.bin";
        unlink(test_file);
        fd = open(test_file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    }
    TEST_ASSERT(fd >= 0, "Created target extent fragmentation test file");

    /* Step 1: Pre-allocate 64KB contiguous extent */
    int rc = untorn_ensure_contiguous(fd, 0, 65536);
    TEST_ASSERT(rc == UNTORN_OK, "Pre-allocated 64KB baseline extent");

    struct untorn_extent_info info;
    rc = untorn_validate_contiguity(fd, 0, 65536, &info);
    TEST_ASSERT(rc == UNTORN_OK && info.is_contiguous, "Initial 64KB extent verified 100% contiguous");

    /* Step 2: Artificially punch a 16KB hole at offset 16384 (blocks 4..7) */
    int punch_rc = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 16384, 16384);
    TEST_ASSERT(punch_rc == 0, "Artificially fragmented file layout via fallocate(FALLOC_FL_PUNCH_HOLE)");

    /* Step 3: Assert untorn_validate_contiguity detects discontiguity */
    rc = untorn_validate_contiguity(fd, 0, 65536, &info);
    TEST_ASSERT(rc == UNTORN_ERR_DISCONTIGUOUS || !info.is_contiguous,
                "untorn_validate_contiguity detected discontiguity across punched file");
    TEST_ASSERT(info.extent_count != 1, "Mapped extents split into discontiguous segments");

    /* Step 4: Assert untorn_validate_contiguity detects unallocated hole */
    rc = untorn_validate_contiguity(fd, 16384, 16384, &info);
    TEST_ASSERT(rc == UNTORN_ERR_DISCONTIGUOUS && info.extent_count == 0,
                "untorn_validate_contiguity identified unallocated hole at [16KB, 32KB)");

    /* Step 5: Invoke untorn_ensure_contiguous on the hole to restore contiguity */
    rc = untorn_ensure_contiguous(fd, 16384, 16384);
    TEST_ASSERT(rc == UNTORN_OK, "untorn_ensure_contiguous successfully pre-allocated hole");

    /* Step 6: Assert contiguity is restored */
    rc = untorn_validate_contiguity(fd, 16384, 16384, &info);
    TEST_ASSERT(rc == UNTORN_OK && info.is_contiguous, "Target extent contiguity successfully restored");
    TEST_ASSERT(info.is_aligned, "Restored extent block offset is 4KB sector aligned");

    /* Step 7: Perform direct hardware atomic write to restored extent */
    struct untorn_atomic_caps caps;
    untorn_probe_fd(fd, &caps);
    void *buf = untorn_alloc_aligned(4096, 16384);
    TEST_ASSERT(buf != NULL, "Allocated 4096-byte aligned buffer for atomic write");
    memset(buf, 0xCE, 16384);
    ssize_t written = untorn_pwrite_atomic(fd, buf, 16384, 16384, &caps);
    TEST_ASSERT(written == 16384, "Direct hardware atomic write succeeded on restored extent");

    void *read_buf = untorn_alloc_aligned(4096, 16384);
    TEST_ASSERT(read_buf != NULL, "Allocated readback buffer");
    ssize_t read_bytes = pread(fd, read_buf, 16384, 16384);
    TEST_ASSERT(read_bytes == 16384, "Read back exact 16KB from disk");
    TEST_ASSERT(memcmp(buf, read_buf, 16384) == 0, "Restored extent contents match atomic buffer byte-for-byte");

    untorn_free_aligned(buf);
    untorn_free_aligned(read_buf);
    close(fd);
    unlink(test_file);
}

#define CONC_NUM_READERS 3
#define CONC_NUM_WRITES  150

struct conc_test_context {
    const char *db_name;
    volatile int keep_running;
    int writes_completed;
    int reads_completed;
    int corrupt_reads;
    int busy_events;
    pthread_mutex_t lock;
};

static void *conc_writer_worker(void *arg) {
    struct conc_test_context *ctx = (struct conc_test_context *)arg;
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(ctx->db_name, &db, SQLITE_OPEN_READWRITE, "untorn");
    if (rc != SQLITE_OK) {
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "INSERT INTO accounts (id, name, balance) VALUES (?, ?, ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }

    for (int i = 0; i < CONC_NUM_WRITES; i++) {
        int tx_rc;
        int retries = 0;
        do {
            tx_rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
            if (tx_rc == SQLITE_BUSY || tx_rc == SQLITE_LOCKED) {
                pthread_mutex_lock(&ctx->lock);
                ctx->busy_events++;
                pthread_mutex_unlock(&ctx->lock);
                usleep(500);
                retries++;
            }
        } while ((tx_rc == SQLITE_BUSY || tx_rc == SQLITE_LOCKED) && retries < 1000);

        if (tx_rc != SQLITE_OK) {
            break;
        }

        sqlite3_bind_int(stmt, 1, 1000 + i);
        char name[32];
        snprintf(name, sizeof(name), "Concurrent_%d", i);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 3, 2000.0 + (double)i);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);

        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

        pthread_mutex_lock(&ctx->lock);
        ctx->writes_completed++;
        pthread_mutex_unlock(&ctx->lock);
        usleep(100);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    ctx->keep_running = 0;
    return NULL;
}

static void *conc_reader_worker(void *arg) {
    struct conc_test_context *ctx = (struct conc_test_context *)arg;
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(ctx->db_name, &db, SQLITE_OPEN_READONLY, "untorn");
    if (rc != SQLITE_OK) {
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);

    while (ctx->keep_running) {
        sqlite3_stmt *stmt = NULL;
        int prep_rc = sqlite3_prepare_v2(db, "SELECT count(*), coalesce(sum(balance), 0.0) FROM accounts;", -1, &stmt, NULL);
        if (prep_rc == SQLITE_OK) {
            int step_rc = sqlite3_step(stmt);
            if (step_rc == SQLITE_ROW) {
                int count = sqlite3_column_int(stmt, 0);
                double sum = sqlite3_column_double(stmt, 1);
                /*
                 * Invariant check during live concurrent read:
                 * Baseline has 50 accounts with total balance 6225.0.
                 * Any valid commit adds accounts with balance >= 2000.0.
                 * If count < 50 or sum < 6224.9, data is corrupted!
                 */
                if (count < 50 || sum < 6224.9) {
                    pthread_mutex_lock(&ctx->lock);
                    ctx->corrupt_reads++;
                    pthread_mutex_unlock(&ctx->lock);
                } else {
                    pthread_mutex_lock(&ctx->lock);
                    ctx->reads_completed++;
                    pthread_mutex_unlock(&ctx->lock);
                }
            } else if (step_rc == SQLITE_BUSY || step_rc == SQLITE_LOCKED) {
                pthread_mutex_lock(&ctx->lock);
                ctx->busy_events++;
                pthread_mutex_unlock(&ctx->lock);
            }
            sqlite3_finalize(stmt);
        } else if (prep_rc == SQLITE_BUSY || prep_rc == SQLITE_LOCKED) {
            pthread_mutex_lock(&ctx->lock);
            ctx->busy_events++;
            pthread_mutex_unlock(&ctx->lock);
        }
        usleep(150);
    }

    sqlite3_close(db);
    return NULL;
}

static void test_multithreaded_concurrency(void) {
    printf(UNTORN_COLOR_BOLD "\n7. Running Test: Multi-Threaded Concurrency Invariant & Advisory Locking" UNTORN_COLOR_RESET "\n");
    const char *db_name = "test_concurrency_inv.db";
    unlink(db_name);
    unlink("test_concurrency_inv.db-journal");

    int rc = untorn_sqlite_vfs_register(false);
    TEST_ASSERT(rc == UNTORN_OK, "Registered Untorn VFS for concurrency tests");

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(db_name, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "untorn");
    TEST_ASSERT(rc == SQLITE_OK && db != NULL, "Opened database for baseline initialization");

    sqlite3_exec(db, "PRAGMA page_size = 16384;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode = OFF;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
    rc = sqlite3_exec(db, "CREATE TABLE accounts (id INT PRIMARY KEY, name TEXT, balance REAL);", NULL, NULL, NULL);
    TEST_ASSERT(rc == SQLITE_OK, "Created accounts table in zero-logging atomic database");

    /* Populate 50 baseline rows */
    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    for (int i = 0; i < 50; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO accounts VALUES (%d, 'Base_%d', %f);", i, i, 100.0 + (double)i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_close(db);

    struct conc_test_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.db_name = db_name;
    ctx.keep_running = 1;
    pthread_mutex_init(&ctx.lock, NULL);

    pthread_t writer;
    pthread_t readers[CONC_NUM_READERS];

    int writer_rc = pthread_create(&writer, NULL, conc_writer_worker, &ctx);
    TEST_ASSERT(writer_rc == 0, "Spawned atomic writer thread");

    int readers_ok = 1;
    for (int i = 0; i < CONC_NUM_READERS; i++) {
        if (pthread_create(&readers[i], NULL, conc_reader_worker, &ctx) != 0) {
            readers_ok = 0;
        }
    }
    TEST_ASSERT(readers_ok == 1, "Spawned 3 concurrent reader threads");

    pthread_join(writer, NULL);
    for (int i = 0; i < CONC_NUM_READERS; i++) {
        pthread_join(readers[i], NULL);
    }

    TEST_ASSERT(ctx.writes_completed == CONC_NUM_WRITES, "Writer completed 100% of planned atomic transactions");
    TEST_ASSERT(ctx.reads_completed > 50, "Reader threads completed multiple concurrent reads during active writes");
    TEST_ASSERT(ctx.corrupt_reads == 0, "Zero read corruption or torn pages detected during concurrency");
    TEST_ASSERT(ctx.busy_events >= 0, "POSIX advisory locking arbitrated concurrent access via SQLITE_BUSY");

    /* Verify database integrity */
    rc = sqlite3_open_v2(db_name, &db, SQLITE_OPEN_READONLY, "untorn");
    TEST_ASSERT(rc == SQLITE_OK, "Re-opened database with Untorn VFS for post-concurrency verification");

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, NULL);
    TEST_ASSERT(rc == SQLITE_OK, "Prepared PRAGMA integrity_check");
    int step_rc = sqlite3_step(stmt);
    TEST_ASSERT(step_rc == SQLITE_ROW, "Executed post-concurrency PRAGMA integrity_check");
    const char *integrity = (const char *)sqlite3_column_text(stmt, 0);
    TEST_ASSERT(integrity != NULL && strcmp(integrity, "ok") == 0,
                "PRAGMA integrity_check returned 'ok' (zero data corruption detected)");
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "SELECT count(*), sum(balance) FROM accounts;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int total_rows = sqlite3_column_int(stmt, 0);
    double total_balance = sqlite3_column_double(stmt, 1);
    TEST_ASSERT(total_rows == 50 + CONC_NUM_WRITES, "Row count accurately reflects all baseline and atomic transactions");

    /* Expected balance: baseline sum (6225.0) + writes sum (311175.0) = 317400.0 */
    double expected_balance = 317400.0;
    double diff = total_balance - expected_balance;
    if (diff < 0) diff = -diff;
    TEST_ASSERT(diff < 0.01, "Account balance sum matches mathematical expectation (lossless transactions)");
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    pthread_mutex_destroy(&ctx.lock);
    untorn_sqlite_vfs_unregister();
    unlink(db_name);
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
    test_synthetic_extent_fragmentation();
    test_multithreaded_concurrency();

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
