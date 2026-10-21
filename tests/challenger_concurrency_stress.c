#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sqlite3.h>
#include "untorn.h"
#include "untorn_internal.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define CONC_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        g_tests_failed++; \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
    } \
} while(0)

/* ==================================================================== */
/* PART 1: HIGH-CONTENTION MULTI-THREADED STRESS (1 WRITER, 6 READERS) */
/* ==================================================================== */

#define MT_NUM_READERS 6
#define MT_NUM_WRITES  300

struct mt_stress_ctx {
    const char *db_name;
    volatile int keep_running;
    int writes_completed;
    int reads_completed;
    int corrupt_reads;
    int busy_events;
    pthread_mutex_t mtx;
};

static void *mt_writer(void *arg) {
    struct mt_stress_ctx *ctx = (struct mt_stress_ctx *)arg;
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(ctx->db_name, &db, SQLITE_OPEN_READWRITE, "untorn");
    if (rc != SQLITE_OK) return NULL;
    sqlite3_busy_timeout(db, 5000);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "INSERT INTO accounts (id, name, balance) VALUES (?, ?, ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }

    for (int i = 0; i < MT_NUM_WRITES; i++) {
        int tx_rc;
        int retries = 0;
        do {
            tx_rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
            if (tx_rc == SQLITE_BUSY || tx_rc == SQLITE_LOCKED) {
                pthread_mutex_lock(&ctx->mtx);
                ctx->busy_events++;
                pthread_mutex_unlock(&ctx->mtx);
                usleep(200);
                retries++;
            }
        } while ((tx_rc == SQLITE_BUSY || tx_rc == SQLITE_LOCKED) && retries < 1000);

        if (tx_rc != SQLITE_OK) break;

        sqlite3_bind_int(stmt, 1, 10000 + i);
        char name[32];
        snprintf(name, sizeof(name), "MT_Stress_%d", i);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 3, 5000.0 + (double)i);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);

        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

        pthread_mutex_lock(&ctx->mtx);
        ctx->writes_completed++;
        pthread_mutex_unlock(&ctx->mtx);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    ctx->keep_running = 0;
    return NULL;
}

static void *mt_reader(void *arg) {
    struct mt_stress_ctx *ctx = (struct mt_stress_ctx *)arg;
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(ctx->db_name, &db, SQLITE_OPEN_READONLY, "untorn");
    if (rc != SQLITE_OK) return NULL;
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
                 * Invariant: baseline 50 rows, sum >= 6224.9
                 * Any read seeing fewer rows or decreased sum is a dirty / torn read!
                 */
                if (count < 50 || sum < 6224.9) {
                    pthread_mutex_lock(&ctx->mtx);
                    ctx->corrupt_reads++;
                    pthread_mutex_unlock(&ctx->mtx);
                } else {
                    pthread_mutex_lock(&ctx->mtx);
                    ctx->reads_completed++;
                    pthread_mutex_unlock(&ctx->mtx);
                }
            } else if (step_rc == SQLITE_BUSY || step_rc == SQLITE_LOCKED) {
                pthread_mutex_lock(&ctx->mtx);
                ctx->busy_events++;
                pthread_mutex_unlock(&ctx->mtx);
            }
            sqlite3_finalize(stmt);
        } else if (prep_rc == SQLITE_BUSY || prep_rc == SQLITE_LOCKED) {
            pthread_mutex_lock(&ctx->mtx);
            ctx->busy_events++;
            pthread_mutex_unlock(&ctx->mtx);
        }
        usleep(50);
    }

    sqlite3_close(db);
    return NULL;
}

static void test_multithreaded_high_contention(void) {
    printf("\n--- Test 1: High-Contention Multi-Threading (1 Writer, %d Readers, %d Writes) ---\n",
           MT_NUM_READERS, MT_NUM_WRITES);
    const char *db_name = "stress_mt_conc.db";
    unlink(db_name);

    int rc = untorn_sqlite_vfs_register(false);
    CONC_ASSERT(rc == UNTORN_OK, "Untorn VFS registered");

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(db_name, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "untorn");
    CONC_ASSERT(rc == SQLITE_OK, "Opened stress database");

    sqlite3_exec(db, "PRAGMA page_size = 16384;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode = OFF;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TABLE accounts (id INT PRIMARY KEY, name TEXT, balance REAL);", NULL, NULL, NULL);

    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    for (int i = 0; i < 50; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO accounts VALUES (%d, 'Base_%d', %f);", i, i, 100.0 + (double)i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_close(db);

    struct mt_stress_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.db_name = db_name;
    ctx.keep_running = 1;
    pthread_mutex_init(&ctx.mtx, NULL);

    pthread_t writer_tid;
    pthread_t reader_tids[MT_NUM_READERS];

    pthread_create(&writer_tid, NULL, mt_writer, &ctx);
    for (int i = 0; i < MT_NUM_READERS; i++) {
        pthread_create(&reader_tids[i], NULL, mt_reader, &ctx);
    }

    pthread_join(writer_tid, NULL);
    for (int i = 0; i < MT_NUM_READERS; i++) {
        pthread_join(reader_tids[i], NULL);
    }

    printf("  [INFO] Writes completed: %d / %d\n", ctx.writes_completed, MT_NUM_WRITES);
    printf("  [INFO] Reads completed:  %d\n", ctx.reads_completed);
    printf("  [INFO] Busy events:      %d\n", ctx.busy_events);
    printf("  [INFO] Corrupt reads:    %d\n", ctx.corrupt_reads);

    CONC_ASSERT(ctx.writes_completed == MT_NUM_WRITES, "All writer transactions committed");
    CONC_ASSERT(ctx.reads_completed > 100, "Substantial concurrent reads verified");
    CONC_ASSERT(ctx.corrupt_reads == 0, "Zero dirty or corrupt reads observed");

    /* Verify post-stress integrity */
    rc = sqlite3_open_v2(db_name, &db, SQLITE_OPEN_READONLY, "untorn");
    CONC_ASSERT(rc == SQLITE_OK, "Reopened database with Untorn VFS");

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, NULL);
    CONC_ASSERT(rc == SQLITE_OK, "Prepared PRAGMA integrity_check");
    int step_rc = sqlite3_step(stmt);
    CONC_ASSERT(step_rc == SQLITE_ROW, "Executed PRAGMA integrity_check");
    const char *integ = (const char *)sqlite3_column_text(stmt, 0);
    CONC_ASSERT(integ != NULL && strcmp(integ, "ok") == 0, "PRAGMA integrity_check = 'ok'");
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "SELECT count(*), sum(balance) FROM accounts;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int total_rows = sqlite3_column_int(stmt, 0);
    double total_sum = sqlite3_column_double(stmt, 1);
    CONC_ASSERT(total_rows == 50 + MT_NUM_WRITES, "Row count precisely matches 50 + MT_NUM_WRITES");

    /* Calculate expected sum: 6225.0 + sum_{i=0..299}(5000 + i) = 6225 + 300*5000 + (299*300/2) = 6225 + 1500000 + 44850 = 1551075.0 */
    double expected_sum = 6225.0 + (MT_NUM_WRITES * 5000.0) + ((double)(MT_NUM_WRITES - 1) * MT_NUM_WRITES / 2.0);
    double diff = total_sum - expected_sum;
    if (diff < 0) diff = -diff;
    CONC_ASSERT(diff < 0.01, "Account balance sum strictly matches mathematical expectation");
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    pthread_mutex_destroy(&ctx.mtx);
    untorn_sqlite_vfs_unregister();
    unlink(db_name);
}

/* ==================================================================== */
/* PART 2: MULTI-PROCESS ADVISORY LOCKING CONCURRENCY (FORKED WORKERS)  */
/* ==================================================================== */

#define MP_WRITES_PER_WORKER 75

static void run_mp_writer_process(const char *db_name, int worker_id) {
    untorn_sqlite_vfs_register(false);
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(db_name, &db, SQLITE_OPEN_READWRITE, "untorn");
    if (rc != SQLITE_OK) exit(1);
    sqlite3_busy_timeout(db, 10000);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "INSERT INTO accounts (id, name, balance) VALUES (?, ?, ?);", -1, &stmt, NULL);

    int committed = 0;
    for (int i = 0; i < MP_WRITES_PER_WORKER; i++) {
        int tx_rc;
        int retries = 0;
        do {
            tx_rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
            if (tx_rc == SQLITE_BUSY || tx_rc == SQLITE_LOCKED) {
                usleep(500 + (rand() % 500));
                retries++;
            }
        } while ((tx_rc == SQLITE_BUSY || tx_rc == SQLITE_LOCKED) && retries < 2000);

        if (tx_rc != SQLITE_OK) break;

        int row_id = (worker_id * 10000) + i;
        sqlite3_bind_int(stmt, 1, row_id);
        char name[32];
        snprintf(name, sizeof(name), "Proc_%d_%d", worker_id, i);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 3, 1000.0 * worker_id + (double)i);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);

        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        committed++;
        usleep(100);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    untorn_sqlite_vfs_unregister();
    exit((committed == MP_WRITES_PER_WORKER) ? 0 : 2);
}

static void test_multiprocess_advisory_locking(void) {
    printf("\n--- Test 2: Multi-Process POSIX Advisory Locking (2 Concurrent Writers via fork()) ---\n");
    const char *db_name = "stress_mp_conc.db";
    unlink(db_name);

    int rc = untorn_sqlite_vfs_register(false);
    CONC_ASSERT(rc == UNTORN_OK, "Untorn VFS registered for multi-process test");

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(db_name, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "untorn");
    CONC_ASSERT(rc == SQLITE_OK, "Created multi-process test database");

    sqlite3_exec(db, "PRAGMA page_size = 16384;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode = OFF;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TABLE accounts (id INT PRIMARY KEY, name TEXT, balance REAL);", NULL, NULL, NULL);

    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    for (int i = 0; i < 20; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO accounts VALUES (%d, 'MP_Base_%d', %f);", i, i, 50.0 + (double)i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_close(db);
    untorn_sqlite_vfs_unregister();

    /* Fork 2 writer processes contending on the same DB file via POSIX advisory locks */
    pid_t pid1 = fork();
    if (pid1 == 0) {
        run_mp_writer_process(db_name, 1);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        run_mp_writer_process(db_name, 2);
    }

    int status1 = 0, status2 = 0;
    waitpid(pid1, &status1, 0);
    waitpid(pid2, &status2, 0);

    CONC_ASSERT(WIFEXITED(status1) && WEXITSTATUS(status1) == 0,
                "Writer process 1 completed all transactions via POSIX lock arbitration");
    CONC_ASSERT(WIFEXITED(status2) && WEXITSTATUS(status2) == 0,
                "Writer process 2 completed all transactions via POSIX lock arbitration");

    /* Verify database integrity */
    untorn_sqlite_vfs_register(false);
    rc = sqlite3_open_v2(db_name, &db, SQLITE_OPEN_READONLY, "untorn");
    CONC_ASSERT(rc == SQLITE_OK, "Opened database post-multiprocess run");

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, NULL);
    CONC_ASSERT(rc == SQLITE_OK, "Prepared PRAGMA integrity_check");
    int step_rc = sqlite3_step(stmt);
    CONC_ASSERT(step_rc == SQLITE_ROW, "Executed PRAGMA integrity_check");
    const char *integ = (const char *)sqlite3_column_text(stmt, 0);
    CONC_ASSERT(integ != NULL && strcmp(integ, "ok") == 0,
                "PRAGMA integrity_check returned 'ok' under multi-process lock contention");
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "SELECT count(*) FROM accounts;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int total_rows = sqlite3_column_int(stmt, 0);
    int expected_rows = 20 + (2 * MP_WRITES_PER_WORKER);
    CONC_ASSERT(total_rows == expected_rows, "Total row count exactly equals baseline + 2*MP_WRITES");
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    untorn_sqlite_vfs_unregister();
    unlink(db_name);
}

int main(void) {
    printf("====================================================================\n");
    printf(" CHALLENGER CONCURRENCY & POSIX ADVISORY LOCKING EMPIRICAL HARNESS  \n");
    printf("====================================================================\n");

    test_multithreaded_high_contention();
    test_multiprocess_advisory_locking();

    printf("\n====================================================================\n");
    printf(" CONCURRENCY HARNESS RESULTS: %d / %d PASSED (%d FAILED)\n",
           g_tests_passed, g_tests_run, g_tests_failed);
    printf("====================================================================\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
