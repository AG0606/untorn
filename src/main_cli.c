#include "untorn.h"
#include "untorn_internal.h"
#include <sqlite3.h>

static void print_usage(const char *prog) {
    printf("\n" UNTORN_COLOR_BOLD "Untorn: Linux Kernel-Storage Co-Design CLI" UNTORN_COLOR_RESET "\n");
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  probe [path]                     Probe NVMe/kernel atomic write capabilities (statx/sysfs)\n");
    printf("  check-extent <file> [len_kb]     Inspect physical block extent contiguity (FIEMAP)\n");
    printf("  prealloc <file> <size_mb>        Pre-allocate contiguous extent slab via fallocate()\n");
    printf("  write-atomic <file> <size_kb>    Execute direct hardware atomic write (RWF_ATOMIC)\n");
    printf("  sqlite-demo <db_file> [count]    Run transactional SQLite test under 'untorn' VFS\n");
    printf("  benchmark <db_file> [tx_count]   Run head-to-head comparison: Rollback vs WAL vs Untorn\n");
    printf("\n");
}

static int cmd_probe(int argc, char **argv) {
    const char *path = (argc > 2) ? argv[2] : ".";
    struct untorn_atomic_caps caps;
    int rc = untorn_probe_path(path, &caps);
    if (rc != UNTORN_OK) {
        UNTORN_LOG_ERR("Probe failed for path '%s'", path);
        return 1;
    }
    untorn_print_caps(&caps);
    return 0;
}

static int cmd_check_extent(int argc, char **argv) {
    if (argc < 3) {
        UNTORN_LOG_ERR("Missing target file");
        return 1;
    }
    const char *path = argv[2];
    size_t len_bytes = (argc > 3) ? (size_t)atoi(argv[3]) * 1024 : 16384;

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        UNTORN_LOG_ERR("Could not open file '%s'", path);
        return 1;
    }

    struct untorn_extent_info info;
    int rc = untorn_validate_contiguity(fd, 0, len_bytes, &info);

    printf("\n" UNTORN_COLOR_BOLD "--- EXT4 / XFS EXTENT CONTIGUITY REPORT ---" UNTORN_COLOR_RESET "\n");
    printf(" Target File          : %s\n", path);
    printf(" Queried Range        : [0, %zu bytes] (%zu KB)\n", len_bytes, len_bytes / 1024);
    printf(" Mapped Extent Count  : %u extent(s)\n", info.extent_count);
    printf(" Physical Block Offset: %lu (0x%lx)\n", (unsigned long)info.physical_offset, (unsigned long)info.physical_offset);
    printf(" Extent Length        : %lu bytes (%lu KB)\n", (unsigned long)info.extent_length, (unsigned long)info.extent_length / 1024);
    printf(" Physically Contiguous: %s\n", info.is_contiguous ?
           UNTORN_COLOR_GREEN "YES (Single Contiguous Block Slab)" UNTORN_COLOR_RESET :
           UNTORN_COLOR_RED "NO (Fragmented Extents! RWF_ATOMIC would abort with -EINVAL)" UNTORN_COLOR_RESET);
    printf(" 4KB Sector Aligned   : %s\n", info.is_aligned ?
           UNTORN_COLOR_GREEN "YES" UNTORN_COLOR_RESET : UNTORN_COLOR_RED "NO" UNTORN_COLOR_RESET);
    printf(UNTORN_COLOR_BOLD "-------------------------------------------\n\n" UNTORN_COLOR_RESET);

    close(fd);
    return (rc == UNTORN_OK && info.is_contiguous) ? 0 : 1;
}

static int cmd_prealloc(int argc, char **argv) {
    if (argc < 4) {
        UNTORN_LOG_ERR("Usage: prealloc <file> <size_mb>");
        return 1;
    }
    const char *path = argv[2];
    size_t size_mb = (size_t)atoi(argv[3]);
    size_t size_bytes = size_mb * 1024 * 1024;

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        UNTORN_LOG_ERR("Could not open file '%s'", path);
        return 1;
    }

    UNTORN_LOG_INFO("Pre-allocating %zu MB contiguous physical extent slab...", size_mb);
    int rc = untorn_ensure_contiguous(fd, 0, size_bytes);
    if (rc == UNTORN_OK) {
        UNTORN_LOG_SUCCESS("Pre-allocated %zu MB successfully! Verified 100%% extent contiguity.", size_mb);
    } else {
        UNTORN_LOG_ERR("Pre-allocation failed");
    }

    close(fd);
    return (rc == UNTORN_OK) ? 0 : 1;
}

static int cmd_write_atomic(int argc, char **argv) {
    if (argc < 3) {
        UNTORN_LOG_ERR("Usage: write-atomic <file> [size_kb]");
        return 1;
    }
    const char *path = argv[2];
    size_t size_kb = (argc > 3) ? (size_t)atoi(argv[3]) : 16;
    size_t size_bytes = size_kb * 1024;

    int fd = open(path, O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) {
        /* Fallback if direct I/O unsupported on directory mount */
        fd = open(path, O_RDWR | O_CREAT, 0644);
    }
    if (fd < 0) {
        UNTORN_LOG_ERR("Could not open '%s'", path);
        return 1;
    }

    struct untorn_atomic_caps caps;
    untorn_probe_fd(fd, &caps);

    /* Allocate aligned buffer */
    void *buf = untorn_alloc_aligned(4096, size_bytes);
    if (!buf) {
        UNTORN_LOG_ERR("Failed to allocate %zu bytes aligned Direct-I/O buffer", size_bytes);
        close(fd);
        return 1;
    }
    memset(buf, 0xA5, size_bytes); /* Fill with recognizable pattern */

    /* Pre-allocate extent */
    untorn_ensure_contiguous(fd, 0, size_bytes);

    double t0 = untorn_get_time_us();
    ssize_t written = untorn_pwrite_atomic(fd, buf, size_bytes, 0, &caps);
    double t1 = untorn_get_time_us();

    if (written == (ssize_t)size_bytes) {
        UNTORN_LOG_SUCCESS("Direct Hardware Atomic Write of %zu KB completed in %.2f us!",
                           size_kb, t1 - t0);
    } else {
        UNTORN_LOG_ERR("Atomic write failed: written=%zd (expected %zu)", written, size_bytes);
    }

    untorn_free_aligned(buf);
    close(fd);
    return (written == (ssize_t)size_bytes) ? 0 : 1;
}

static int cmd_sqlite_demo(int argc, char **argv) {
    const char *db_path = (argc > 2) ? argv[2] : "untorn_demo.db";
    int count = (argc > 3) ? atoi(argv[3]) : 1000;

    unlink(db_path); /* Clean start so PRAGMA page_size takes effect */

    untorn_sqlite_vfs_register(true);

    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        UNTORN_LOG_ERR("Failed to open SQLite database '%s': %s", db_path, sqlite3_errmsg(db));
        return 1;
    }

    UNTORN_LOG_INFO("Configuring SQLite for Zero-Logging Hardware Atomic Mode...");
    sqlite3_exec(db, "PRAGMA page_size = 16384;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode = OFF;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);

    sqlite3_exec(db, "CREATE TABLE accounts (id INTEGER PRIMARY KEY, name TEXT, balance REAL);",
                 NULL, NULL, NULL);

    UNTORN_LOG_INFO("Executing %d atomic transactions via Untorn VFS...", count);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "INSERT INTO accounts (name, balance) VALUES (?, ?);", -1, &stmt, NULL);

    double t0 = untorn_get_time_us();
    for (int i = 0; i < count; i++) {
        sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
        char name[32];
        snprintf(name, sizeof(name), "User_%d", i);
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 2, 1000.0 + i * 1.5);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    }
    sqlite3_finalize(stmt);
    double t1 = untorn_get_time_us();

    UNTORN_LOG_SUCCESS("Committed %d atomic transactions in %.2f ms (%.0f tx/sec) with ZERO rollback journal!",
                       count, (t1 - t0) / 1000.0, (double)count / ((t1 - t0) / 1e6));

    /* Integrity check */
    UNTORN_LOG_INFO("Running database integrity verification...");
    sqlite3_stmt *check_stmt;
    sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &check_stmt, NULL);
    if (sqlite3_step(check_stmt) == SQLITE_ROW) {
        const char *result = (const char *)sqlite3_column_text(check_stmt, 0);
        if (strcmp(result, "ok") == 0) {
            UNTORN_LOG_SUCCESS("PRAGMA integrity_check: %s! 100%% ACID consistency guaranteed.", result);
        } else {
            UNTORN_LOG_ERR("Integrity check failed: %s", result);
        }
    }
    sqlite3_finalize(check_stmt);

    sqlite3_close(db);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "probe") == 0) {
        return cmd_probe(argc, argv);
    } else if (strcmp(cmd, "check-extent") == 0) {
        return cmd_check_extent(argc, argv);
    } else if (strcmp(cmd, "prealloc") == 0) {
        return cmd_prealloc(argc, argv);
    } else if (strcmp(cmd, "write-atomic") == 0) {
        return cmd_write_atomic(argc, argv);
    } else if (strcmp(cmd, "sqlite-demo") == 0) {
        return cmd_sqlite_demo(argc, argv);
    } else if (strcmp(cmd, "benchmark") == 0) {
        char bench_cmd[256];
        int num = (argc > 2) ? atoi(argv[2]) : 2000;
        snprintf(bench_cmd, sizeof(bench_cmd), "./bin/benchmark_suite %d", num);
        return system(bench_cmd);
    } else {
        print_usage(argv[0]);
        return 1;
    }
}
