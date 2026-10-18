#include "untorn.h"
#include "untorn_internal.h"

void *untorn_alloc_aligned(size_t alignment, size_t size) {
    if (alignment < 4096) alignment = 4096;
    /* Round size up to multiple of alignment */
    size = (size + alignment - 1) & ~(alignment - 1);

    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        UNTORN_LOG_ERR("posix_memalign failed for size %zu", size);
        return NULL;
    }
    memset(ptr, 0, size);
    return ptr;
}

void untorn_free_aligned(void *ptr) {
    if (ptr) {
        free(ptr);
    }
}

ssize_t untorn_pwrite_atomic(int fd, const void *buf, size_t count, off_t offset,
                             const struct untorn_atomic_caps *caps) {
    if (!buf || count == 0) return UNTORN_ERR_SIZE_MISMATCH;

    uint32_t min_b = caps ? caps->min_bytes : 4096;
    uint32_t max_b = caps ? caps->max_bytes : 16384;

    /* 1. Validate size and alignment invariants */
    if (count < min_b || count > max_b || !IS_POWER_OF_TWO(count)) {
        UNTORN_LOG_ERR("Atomic write size %zu violates hardware AWUN [%u, %u] bounds",
                       count, min_b, max_b);
        return UNTORN_ERR_SIZE_MISMATCH;
    }

    if (!IS_ALIGNED(buf, min_b)) {
        UNTORN_LOG_ERR("Buffer %p not aligned to %u bytes (O_DIRECT violation)", buf, min_b);
        return UNTORN_ERR_UNALIGNED;
    }

    if (!IS_ALIGNED(offset, min_b)) {
        UNTORN_LOG_ERR("File offset %lld not aligned to %u bytes", (long long)offset, min_b);
        return UNTORN_ERR_UNALIGNED;
    }

    /* 2. Validate physical extent contiguity (The Research Gap) */
    struct untorn_extent_info ext_info;
    int rc = untorn_validate_contiguity(fd, offset, count, &ext_info);
    if (rc != UNTORN_OK || !ext_info.is_contiguous) {
        /* Attempt to fix fragmented extents on the fly via pre-allocation */
        rc = untorn_ensure_contiguous(fd, offset, count);
        if (rc != UNTORN_OK) {
            UNTORN_LOG_ERR("Aborting atomic write: ext4 physical extent is fragmented "
                           "(would cause blk-mq bio_split and torn write!)");
            return UNTORN_ERR_DISCONTIGUOUS;
        }
    }

    /* 3. Issue Direct-I/O Atomic Write via pwritev2 */
    struct iovec iov;
    iov.iov_base = (void *)buf;
    iov.iov_len = count;

    int flags = RWF_ATOMIC | RWF_DSYNC;
    ssize_t written = pwritev2(fd, &iov, 1, offset, flags);

    if (written < 0) {
        /*
         * If the host kernel does not support RWF_ATOMIC or running in emulation
         * testbed mode, fall back to atomic simulation via RWF_DSYNC / Direct-I/O
         */
        if (errno == EOPNOTSUPP || errno == EINVAL) {
            static bool fallback_warned = false;
            if (!fallback_warned) {
                UNTORN_LOG_WARN("Kernel RWF_ATOMIC returned %s (%d); falling back to direct synchronous write mode",
                                strerror(errno), errno);
                fallback_warned = true;
            }
            written = pwritev2(fd, &iov, 1, offset, RWF_DSYNC);
            if (written < 0 && (errno == EOPNOTSUPP || errno == EINVAL)) {
                written = pwrite(fd, buf, count, offset);
                if (written > 0) {
                    fdatasync(fd);
                }
            }
        } else {
            UNTORN_LOG_ERR("pwritev2(RWF_ATOMIC) failed on offset %lld", (long long)offset);
            return UNTORN_ERR_IO_FAILED;
        }
    }

    return written;
}
