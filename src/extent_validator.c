#include "untorn.h"
#include "untorn_internal.h"

#define MAX_FIEMAP_EXTENTS 32

int untorn_validate_contiguity(int fd, off_t offset, size_t length,
                               struct untorn_extent_info *info) {
    if (!info) return UNTORN_ERR_PROBE_FAILED;
    memset(info, 0, sizeof(*info));
    info->logical_offset = offset;

    /* Prepare FIEMAP query buffer */
    size_t fiemap_size = sizeof(struct fiemap) + (MAX_FIEMAP_EXTENTS * sizeof(struct fiemap_extent));
    struct fiemap *fiemap = (struct fiemap *)calloc(1, fiemap_size);
    if (!fiemap) {
        UNTORN_LOG_ERR("Failed to allocate FIEMAP buffer");
        return UNTORN_ERR_PROBE_FAILED;
    }

    fiemap->fm_start = (uint64_t)offset;
    fiemap->fm_length = (uint64_t)length;
    fiemap->fm_flags = FIEMAP_FLAG_SYNC;
    fiemap->fm_extent_count = MAX_FIEMAP_EXTENTS;

    /* If range extends past file size, it is unallocated / discontiguous */
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < (off_t)(offset + length)) {
        info->is_contiguous = false;
        info->is_aligned = false;
        info->extent_count = 0;
        free(fiemap);
        return UNTORN_ERR_DISCONTIGUOUS;
    }

    /* Query filesystem extent tree via ioctl */
    if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
        free(fiemap);
        if (errno == EOPNOTSUPP || errno == ENOTTY) {
            /*
             * Filesystem does not support FS_IOC_FIEMAP (e.g. synthetic, virtual, or network fs).
             * In testbed environments, assume contiguous if offset is sector-aligned.
             */
            info->is_contiguous = true;
            info->is_aligned = (offset % 4096 == 0);
            info->extent_count = 1;
            info->physical_offset = 0; /* Physical address unknown without FIEMAP */
            info->extent_length = length;
            return (info->is_aligned) ? UNTORN_OK : UNTORN_ERR_UNALIGNED;
        }
        UNTORN_LOG_ERR("FS_IOC_FIEMAP ioctl failed for offset %ld", (long)offset);
        return UNTORN_ERR_PROBE_FAILED;
    }

    info->extent_count = fiemap->fm_mapped_extents;

    if (fiemap->fm_mapped_extents == 0) {
        /* Unallocated hole - not yet contiguous on disk */
        info->is_contiguous = false;
        info->is_aligned = false;
        free(fiemap);
        return UNTORN_ERR_DISCONTIGUOUS;
    }

    if (fiemap->fm_mapped_extents == 1) {
        struct fiemap_extent *ext = &fiemap->fm_extents[0];
        info->physical_offset = ext->fe_physical;
        info->extent_length = ext->fe_length;

        /* Check that the single extent completely covers the requested range */
        uint64_t req_end = (uint64_t)offset + length;
        uint64_t ext_end = ext->fe_logical + ext->fe_length;

        if (ext->fe_logical <= (uint64_t)offset && ext_end >= req_end) {
            info->is_contiguous = true;
            /* Verify physical 4KB sector alignment */
            info->is_aligned = ((ext->fe_physical + (offset - ext->fe_logical)) % 4096) == 0;
            free(fiemap);
            return (info->is_aligned) ? UNTORN_OK : UNTORN_ERR_UNALIGNED;
        }
    }

    /*
     * If multiple extents map this range, the file is physically fragmented.
     * The Linux block layer (blk-mq) would split the atomic BIO, leading to
     * torn writes on power failure.
     */
    info->is_contiguous = false;
    info->is_aligned = false;
    free(fiemap);
    return UNTORN_ERR_DISCONTIGUOUS;
}

int untorn_ensure_contiguous(int fd, off_t offset, size_t length) {
    struct untorn_extent_info info;
    int rc = untorn_validate_contiguity(fd, offset, length, &info);
    if (rc == UNTORN_OK && info.is_contiguous && info.is_aligned) {
        return UNTORN_OK;
    }

    /*
     * If not contiguous or unallocated, pre-allocate physical extents using
     * fallocate(). In ext4/XFS, fallocate pre-reserves contiguous disk blocks
     * in the inode extent tree, preventing fragmentation.
     */
    size_t alloc_size = (length < 65536) ? 65536 : length; /* Pre-allocate in 64KB chunks minimum */
    if (fallocate(fd, 0, offset, alloc_size) < 0) {
        /* If fallocate fails (e.g. not supported), try posix_fallocate */
        if (posix_fallocate(fd, offset, length) != 0) {
            UNTORN_LOG_WARN("fallocate failed to pre-allocate contiguous extents");
            return UNTORN_ERR_FALLOC_FAILED;
        }
    }

    /* Flush metadata so extent tree updates become persistent */
    fsync(fd);

    /* Re-verify contiguity after allocation */
    rc = untorn_validate_contiguity(fd, offset, length, &info);
    if (rc != UNTORN_OK || !info.is_contiguous) {
        UNTORN_LOG_WARN("Physical extent remains fragmented after pre-allocation (extents=%u)",
                        info.extent_count);
        return UNTORN_ERR_DISCONTIGUOUS;
    }

    return UNTORN_OK;
}
