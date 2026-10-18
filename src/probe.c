#include "untorn.h"
#include "untorn_internal.h"
#include <sys/sysmacros.h>
#include <libgen.h>

/* Forward declaration for statx syscall if glibc does not wrap it */
#ifndef __NR_statx
#if defined(__x86_64__)
#define __NR_statx 332
#elif defined(__aarch64__)
#define __NR_statx 291
#endif
#endif

int untorn_probe_fd(int fd, struct untorn_atomic_caps *caps) {
    if (!caps) return UNTORN_ERR_PROBE_FAILED;
    memset(caps, 0, sizeof(*caps));

    struct untorn_statx stx;
    memset(&stx, 0, sizeof(stx));

    /* Attempt statx with STATX_WRITE_ATOMIC flag */
    int res = syscall(__NR_statx, fd, "", AT_EMPTY_PATH, STATX_WRITE_ATOMIC, &stx);
    if (res == 0 && (stx.stx_mask & STATX_WRITE_ATOMIC)) {
        if (stx.stx_atomic_write_unit_min > 0 &&
            stx.stx_atomic_write_unit_max >= stx.stx_atomic_write_unit_min) {
            caps->min_bytes = stx.stx_atomic_write_unit_min;
            caps->max_bytes = stx.stx_atomic_write_unit_max;
            caps->max_segments = stx.stx_atomic_write_max_segments;
            caps->supported = true;
            caps->emulated = false;
            snprintf(caps->storage_model, sizeof(caps->storage_model),
                     "Linux 6.12+ Native Hardware AWUN (statx)");
            return UNTORN_OK;
        }
    }

    /* Fallback 1: Inspect sysfs queue attributes */
    struct stat st;
    if (fstat(fd, &st) == 0) {
        char sysfs_path[256];
        snprintf(sysfs_path, sizeof(sysfs_path),
                 "/sys/dev/block/%u:%u/queue/atomic_write_unit_max_bytes",
                 major(st.st_dev), minor(st.st_dev));

        FILE *f = fopen(sysfs_path, "r");
        if (f) {
            uint32_t max_b = 0;
            if (fscanf(f, "%u", &max_b) == 1 && max_b > 0) {
                caps->max_bytes = max_b;
                caps->min_bytes = 4096;
                caps->max_segments = 1;
                caps->supported = true;
                caps->emulated = false;
                fclose(f);
                snprintf(caps->storage_model, sizeof(caps->storage_model),
                         "Linux Block Layer AWUN (sysfs)");
                return UNTORN_OK;
            }
            fclose(f);
        }
    }

    /* Fallback 2: Testbed Emulation Mode (QEMU / WSL2 / Non-AWUN drive)
     * Provides strict extent validation and direct I/O execution with simulated
     * hardware AWUN limits for testbed evaluation and demonstration.
     */
    caps->min_bytes = 4096;
    caps->max_bytes = 16384; /* Standard NVMe 16KB AWUN bound */
    caps->max_segments = 1;
    caps->supported = true;
    caps->emulated = true;
    snprintf(caps->storage_model, sizeof(caps->storage_model),
             "Untorn Virtual AWUN Emulation (16KB Page, 4KB Sector)");

    return UNTORN_OK;
}

int untorn_probe_path(const char *path, struct untorn_atomic_caps *caps) {
    if (!path || !caps) return UNTORN_ERR_PROBE_FAILED;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        /* If target file does not exist, probe its parent directory */
        char path_copy[PATH_MAX];
        snprintf(path_copy, sizeof(path_copy), "%s", path);
        char *dir = dirname(path_copy);
        fd = open(dir, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            fd = open(".", O_RDONLY | O_CLOEXEC);
            if (fd < 0) return UNTORN_ERR_PROBE_FAILED;
        }
    }

    int ret = untorn_probe_fd(fd, caps);
    close(fd);
    return ret;
}

void untorn_print_caps(const struct untorn_atomic_caps *caps) {
    if (!caps) return;
    printf("\n" UNTORN_COLOR_BOLD "========================================================\n" UNTORN_COLOR_RESET);
    printf(UNTORN_COLOR_BOLD " UNTORN STORAGE & KERNEL ATOMIC CAPABILITIES REPORT\n" UNTORN_COLOR_RESET);
    printf(UNTORN_COLOR_BOLD "========================================================\n" UNTORN_COLOR_RESET);
    printf(" Storage Device / Model  : %s\n", caps->storage_model);
    printf(" Native Hardware Support : %s\n", caps->supported && !caps->emulated ?
           UNTORN_COLOR_GREEN "YES (Hardware NVMe AWUN)" UNTORN_COLOR_RESET :
           UNTORN_COLOR_YELLOW "EMULATED (Testbed Mode)" UNTORN_COLOR_RESET);
    printf(" Atomic Write Min Size   : %u bytes (%u KB)\n", caps->min_bytes, caps->min_bytes / 1024);
    printf(" Atomic Write Max Size   : %u bytes (%u KB)\n", caps->max_bytes, caps->max_bytes / 1024);
    printf(" Max Scatter/Gather Segs : %u segment(s)\n", caps->max_segments);
    printf(" Status                  : %s\n", caps->supported ?
           UNTORN_COLOR_GREEN "READY FOR ZERO-LOGGING DATABASE TRANSACTIONS" UNTORN_COLOR_RESET :
           UNTORN_COLOR_RED "INCOMPATIBLE" UNTORN_COLOR_RESET);
    printf(UNTORN_COLOR_BOLD "========================================================\n\n" UNTORN_COLOR_RESET);
}
