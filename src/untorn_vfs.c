#include "untorn.h"
#include "untorn_internal.h"
#include <sqlite3.h>

/*
 * UntornFile structure extending standard sqlite3_file
 */
typedef struct {
    sqlite3_file base;               /* Base SQLite file handle */
    int fd;                          /* Underlying OS file descriptor */
    struct untorn_atomic_caps caps;  /* Discovered atomic capabilities */
    bool is_main_db;                 /* True if main database file */
    bool direct_io;                  /* True if opened with O_DIRECT */
    char *aligned_buffer;            /* Reusable 4KB-aligned scratch buffer */
    size_t aligned_buffer_size;      /* Size of aligned scratch buffer */
    int lock_level;                  /* Current SQLite lock level (SQLITE_LOCK_*) */
    off_t contiguous_extent_size;    /* Cached size of verified contiguous extent */
} UntornFile;

/* Forward declarations for VFS I/O methods */
static int untorn_io_close(sqlite3_file *pFile);
static int untorn_io_read(sqlite3_file *pFile, void *zBuf, int iAmt, sqlite3_int64 iOfst);
static int untorn_io_write(sqlite3_file *pFile, const void *zBuf, int iAmt, sqlite3_int64 iOfst);
static int untorn_io_truncate(sqlite3_file *pFile, sqlite3_int64 size);
static int untorn_io_sync(sqlite3_file *pFile, int flags);
static int untorn_io_file_size(sqlite3_file *pFile, sqlite3_int64 *pSize);
static int untorn_io_lock(sqlite3_file *pFile, int eLock);
static int untorn_io_unlock(sqlite3_file *pFile, int eLock);
static int untorn_io_check_reserved_lock(sqlite3_file *pFile, int *pResOut);
static int untorn_io_file_control(sqlite3_file *pFile, int op, void *pArg);
static int untorn_io_sector_size(sqlite3_file *pFile);
static int untorn_io_device_characteristics(sqlite3_file *pFile);

static const sqlite3_io_methods untorn_io_methods = {
    3,                              /* iVersion */
    untorn_io_close,                /* xClose */
    untorn_io_read,                 /* xRead */
    untorn_io_write,                /* xWrite */
    untorn_io_truncate,             /* xTruncate */
    untorn_io_sync,                 /* xSync */
    untorn_io_file_size,            /* xFileSize */
    untorn_io_lock,                 /* xLock */
    untorn_io_unlock,               /* xUnlock */
    untorn_io_check_reserved_lock,  /* xCheckReservedLock */
    untorn_io_file_control,         /* xFileControl */
    untorn_io_sector_size,          /* xSectorSize */
    untorn_io_device_characteristics,/* xDeviceCharacteristics */
    NULL,                           /* xShmMap */
    NULL,                           /* xShmLock */
    NULL,                           /* xShmBarrier */
    NULL,                           /* xShmUnmap */
    NULL,                           /* xFetch */
    NULL                            /* xUnfetch */
};

/* VFS Implementation functions */
static int untorn_vfs_open(sqlite3_vfs *pVfs, const char *zName, sqlite3_file *pFile,
                           int flags, int *pOutFlags);
static int untorn_vfs_delete(sqlite3_vfs *pVfs, const char *zName, int syncDir);
static int untorn_vfs_access(sqlite3_vfs *pVfs, const char *zName, int flags, int *pResOut);
static int untorn_vfs_full_pathname(sqlite3_vfs *pVfs, const char *zName, int nOut, char *zOut);

static sqlite3_vfs untorn_vfs = {
    3,                              /* iVersion */
    sizeof(UntornFile),             /* szOsFile */
    512,                            /* mxPathname */
    NULL,                           /* pNext */
    "untorn",                       /* zName */
    NULL,                           /* pAppData */
    untorn_vfs_open,                /* xOpen */
    untorn_vfs_delete,              /* xDelete */
    untorn_vfs_access,              /* xAccess */
    untorn_vfs_full_pathname,       /* xFullPathname */
    NULL,                           /* xDlOpen */
    NULL,                           /* xDlError */
    NULL,                           /* xDlSym */
    NULL,                           /* xDlClose */
    NULL,                           /* xRandomness */
    NULL,                           /* xSleep */
    NULL,                           /* xCurrentTime */
    NULL,                           /* xGetLastError */
    NULL,                           /* xCurrentTimeInt64 */
    NULL,                           /* xSetSystemCall */
    NULL,                           /* xGetSystemCall */
    NULL                            /* xNextSystemCall */
};

/* Delegate to standard default VFS for helper functions */
static sqlite3_vfs *orig_vfs = NULL;

static int untorn_io_close(sqlite3_file *pFile) {
    UntornFile *p = (UntornFile *)pFile;
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    if (p->aligned_buffer) {
        untorn_free_aligned(p->aligned_buffer);
        p->aligned_buffer = NULL;
    }
    return SQLITE_OK;
}

static int untorn_io_read(sqlite3_file *pFile, void *zBuf, int iAmt, sqlite3_int64 iOfst) {
    UntornFile *p = (UntornFile *)pFile;

    /*
     * When direct_io (O_DIRECT) is active, memory buffer address, file offset,
     * and byte count must align to 4096-byte boundaries. SQLite's zBuf is not
     * guaranteed to be aligned (e.g. SQLite database header reads of 100 bytes).
     * In such cases, route through our aligned bounce buffer to prevent EINVAL.
     */
    if (p->direct_io && (!IS_ALIGNED(zBuf, 4096) || (iOfst % 4096 != 0) || (iAmt % 4096 != 0))) {
        off_t aligned_offset = (iOfst / 4096) * 4096;
        size_t offset_delta = (size_t)(iOfst - aligned_offset);
        size_t needed_bytes = ((offset_delta + iAmt + 4095) / 4096) * 4096;

        if (!p->aligned_buffer || p->aligned_buffer_size < needed_bytes) {
            untorn_free_aligned(p->aligned_buffer);
            p->aligned_buffer = untorn_alloc_aligned(4096, needed_bytes);
            p->aligned_buffer_size = needed_bytes;
        }

        ssize_t n = pread(p->fd, p->aligned_buffer, needed_bytes, aligned_offset);
        if (n < 0) return SQLITE_IOERR_READ;

        if ((size_t)n <= offset_delta) {
            memset(zBuf, 0, iAmt);
            return SQLITE_IOERR_SHORT_READ;
        }

        size_t available = (size_t)n - offset_delta;
        size_t to_copy = (available < (size_t)iAmt) ? available : (size_t)iAmt;
        memcpy(zBuf, p->aligned_buffer + offset_delta, to_copy);

        if (to_copy < (size_t)iAmt) {
            memset((char *)zBuf + to_copy, 0, iAmt - to_copy);
            return SQLITE_IOERR_SHORT_READ;
        }
        return SQLITE_OK;
    }

    ssize_t n = pread(p->fd, zBuf, iAmt, iOfst);
    if (n < 0) return SQLITE_IOERR_READ;
    if (n < iAmt) {
        memset((char *)zBuf + n, 0, iAmt - n);
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}

static int untorn_io_write(sqlite3_file *pFile, const void *zBuf, int iAmt, sqlite3_int64 iOfst) {
    UntornFile *p = (UntornFile *)pFile;

    /*
     * If this is the main database file and the write size matches atomic
     * boundaries (e.g. 4KB, 8KB, 16KB), dispatch through the Untorn
     * Direct-I/O Hardware Atomic write path.
     */
    if (p->is_main_db && iAmt >= (int)p->caps.min_bytes && IS_POWER_OF_TWO(iAmt)) {
        const void *write_buf = zBuf;

        /* Verify memory alignment for Direct-I/O */
        if (!IS_ALIGNED(zBuf, p->caps.min_bytes)) {
            if (!p->aligned_buffer || (size_t)iAmt > p->aligned_buffer_size) {
                untorn_free_aligned(p->aligned_buffer);
                p->aligned_buffer = untorn_alloc_aligned(4096, iAmt);
                p->aligned_buffer_size = iAmt;
            }
            memcpy(p->aligned_buffer, zBuf, iAmt);
            write_buf = p->aligned_buffer;
        }

        /* Check if write falls within cached contiguous extent */
        if ((off_t)(iOfst + iAmt) > p->contiguous_extent_size) {
            untorn_ensure_contiguous(p->fd, iOfst, iAmt);
            p->contiguous_extent_size = iOfst + iAmt + 65536;
        }

        ssize_t written = untorn_pwrite_atomic(p->fd, write_buf, iAmt, iOfst, &p->caps);
        if (written == iAmt) {
            return SQLITE_OK;
        }
        UNTORN_LOG_WARN("Atomic write failed, falling back to pwrite: written=%zd (amt=%d)", written, iAmt);
    }

    /* Fallback standard write */
    ssize_t n = pwrite(p->fd, zBuf, iAmt, iOfst);
    if (n != iAmt) return SQLITE_IOERR_WRITE;
    return SQLITE_OK;
}

static int untorn_io_truncate(sqlite3_file *pFile, sqlite3_int64 size) {
    UntornFile *p = (UntornFile *)pFile;
    if (ftruncate(p->fd, size) < 0) return SQLITE_IOERR_TRUNCATE;
    return SQLITE_OK;
}

static int untorn_io_sync(sqlite3_file *pFile, int flags) {
    (void)flags;
    UntornFile *p = (UntornFile *)pFile;
    /*
     * Hardware atomic writes are issued with RWF_DSYNC or synchronous NVMe
     * persistence. Hence, expensive full-file fsync barriers can be bypassed!
     */
    if (p->is_main_db && p->caps.supported) {
        return SQLITE_OK;
    }
    if (fdatasync(p->fd) < 0) return SQLITE_IOERR_FSYNC;
    return SQLITE_OK;
}

static int untorn_io_file_size(sqlite3_file *pFile, sqlite3_int64 *pSize) {
    UntornFile *p = (UntornFile *)pFile;
    struct stat st;
    if (fstat(p->fd, &st) < 0) return SQLITE_IOERR_FSTAT;
    *pSize = st.st_size;
    return SQLITE_OK;
}

static int untorn_io_lock(sqlite3_file *pFile, int eLock) {
    UntornFile *p = (UntornFile *)pFile;
    if (p->lock_level >= eLock) {
        return SQLITE_OK;
    }

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0; /* Entire file */

    if (eLock == SQLITE_LOCK_SHARED) {
        fl.l_type = F_RDLCK;
        if (fcntl(p->fd, F_SETLK, &fl) < 0) {
            return (errno == EAGAIN || errno == EACCES) ? SQLITE_BUSY : SQLITE_IOERR_LOCK;
        }
    } else if (eLock >= SQLITE_LOCK_RESERVED) {
        fl.l_type = F_WRLCK;
        if (fcntl(p->fd, F_SETLK, &fl) < 0) {
            return (errno == EAGAIN || errno == EACCES) ? SQLITE_BUSY : SQLITE_IOERR_LOCK;
        }
    }

    p->lock_level = eLock;
    return SQLITE_OK;
}

static int untorn_io_unlock(sqlite3_file *pFile, int eLock) {
    UntornFile *p = (UntornFile *)pFile;
    if (p->lock_level <= eLock) {
        return SQLITE_OK;
    }

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0; /* Entire file */

    if (eLock == SQLITE_LOCK_SHARED) {
        fl.l_type = F_RDLCK;
        if (fcntl(p->fd, F_SETLK, &fl) < 0) {
            return SQLITE_IOERR_UNLOCK;
        }
    } else if (eLock == SQLITE_LOCK_NONE) {
        fl.l_type = F_UNLCK;
        if (fcntl(p->fd, F_SETLK, &fl) < 0) {
            return SQLITE_IOERR_UNLOCK;
        }
    }

    p->lock_level = eLock;
    return SQLITE_OK;
}

static int untorn_io_check_reserved_lock(sqlite3_file *pFile, int *pResOut) {
    UntornFile *p = (UntornFile *)pFile;
    if (p->lock_level >= SQLITE_LOCK_RESERVED) {
        *pResOut = 1;
        return SQLITE_OK;
    }

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(p->fd, F_GETLK, &fl) < 0) {
        *pResOut = 0;
        return SQLITE_IOERR_CHECKRESERVEDLOCK;
    }

    *pResOut = (fl.l_type != F_UNLCK);
    return SQLITE_OK;
}

static int untorn_io_file_control(sqlite3_file *pFile, int op, void *pArg) {
    UntornFile *p = (UntornFile *)pFile;
    if (op == SQLITE_FCNTL_SYNC) {
        return untorn_io_sync(pFile, 0);
    }
    if (op == SQLITE_FCNTL_VFS_POINTER) {
        *(sqlite3_vfs **)pArg = &untorn_vfs;
        return SQLITE_OK;
    }
    (void)p;
    return SQLITE_NOTFOUND;
}

static int untorn_io_sector_size(sqlite3_file *pFile) {
    UntornFile *p = (UntornFile *)pFile;
    return (p->caps.min_bytes > 0) ? (int)p->caps.min_bytes : 4096;
}

static int untorn_io_device_characteristics(sqlite3_file *pFile) {
    UntornFile *p = (UntornFile *)pFile;
    int flags = SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SAFE_APPEND;

    if (p->is_main_db && p->caps.supported) {
        /*
         * Tell SQLite the hardware guarantees atomic writes across page sizes!
         * This informs SQLite that torn writes are physically impossible.
         */
        flags |= SQLITE_IOCAP_ATOMIC;
        if (p->caps.max_bytes >= 4096)  flags |= SQLITE_IOCAP_ATOMIC4K;
        if (p->caps.max_bytes >= 8192)  flags |= SQLITE_IOCAP_ATOMIC8K;
        if (p->caps.max_bytes >= 16384) flags |= SQLITE_IOCAP_ATOMIC16K;
    }

    return flags;
}

static int untorn_vfs_open(sqlite3_vfs *pVfs, const char *zName, sqlite3_file *pFile,
                           int flags, int *pOutFlags) {
    (void)pVfs;
    UntornFile *p = (UntornFile *)pFile;
    memset(p, 0, sizeof(*p));
    pFile->pMethods = &untorn_io_methods;

    int open_flags = 0;
    if (flags & SQLITE_OPEN_READWRITE) open_flags |= O_RDWR;
    if (flags & SQLITE_OPEN_READONLY)  open_flags |= O_RDONLY;
    if (flags & SQLITE_OPEN_CREATE)    open_flags |= O_CREAT;

    p->is_main_db = (flags & SQLITE_OPEN_MAIN_DB) != 0;

    /* For main database files, use O_DIRECT to bypass OS page-cache corruption */
    if (p->is_main_db) {
        int direct_fd = open(zName, open_flags | O_DIRECT | O_CLOEXEC, 0644);
        if (direct_fd >= 0) {
            p->fd = direct_fd;
            p->direct_io = true;
        } else {
            /* Fallback to standard open if O_DIRECT unsupported by mount */
            p->fd = open(zName, open_flags | O_CLOEXEC, 0644);
            p->direct_io = false;
        }
    } else {
        p->fd = open(zName, open_flags | O_CLOEXEC, 0644);
    }

    if (p->fd < 0) return SQLITE_CANTOPEN;

    /* Discover atomic capabilities for this file */
    untorn_probe_fd(p->fd, &p->caps);

    if (pOutFlags) *pOutFlags = flags;
    return SQLITE_OK;
}

static int untorn_vfs_delete(sqlite3_vfs *pVfs, const char *zName, int syncDir) {
    (void)pVfs; (void)syncDir;
    if (unlink(zName) < 0 && errno != ENOENT) return SQLITE_IOERR_DELETE;
    return SQLITE_OK;
}

static int untorn_vfs_access(sqlite3_vfs *pVfs, const char *zName, int flags, int *pResOut) {
    (void)pVfs;
    int amode = F_OK;
    if (flags & SQLITE_ACCESS_READWRITE) amode = R_OK | W_OK;
    if (flags & SQLITE_ACCESS_READ)      amode = R_OK;
    *pResOut = (access(zName, amode) == 0);
    return SQLITE_OK;
}

static int untorn_vfs_full_pathname(sqlite3_vfs *pVfs, const char *zName, int nOut, char *zOut) {
    (void)pVfs;
    char resolved[PATH_MAX];
    if (realpath(zName, resolved) != NULL) {
        snprintf(zOut, nOut, "%s", resolved);
    } else {
        snprintf(zOut, nOut, "%s", zName);
    }
    return SQLITE_OK;
}

int untorn_sqlite_vfs_register(bool make_default) {
    orig_vfs = sqlite3_vfs_find(NULL);
    if (!orig_vfs) {
        UNTORN_LOG_ERR("Could not locate default SQLite VFS");
        return UNTORN_ERR_NOT_SUPPORTED;
    }

    /* Inherit fallback methods from standard VFS */
    untorn_vfs.xRandomness = orig_vfs->xRandomness;
    untorn_vfs.xSleep = orig_vfs->xSleep;
    untorn_vfs.xCurrentTime = orig_vfs->xCurrentTime;
    untorn_vfs.xGetLastError = orig_vfs->xGetLastError;
    untorn_vfs.xCurrentTimeInt64 = orig_vfs->xCurrentTimeInt64;

    int rc = sqlite3_vfs_register(&untorn_vfs, make_default ? 1 : 0);
    if (rc != SQLITE_OK) {
        UNTORN_LOG_ERR("Failed to register SQLite Untorn VFS: %d", rc);
        return UNTORN_ERR_NOT_SUPPORTED;
    }

    UNTORN_LOG_SUCCESS("Registered 'untorn' SQLite VFS (make_default=%s)",
                       make_default ? "true" : "false");
    return UNTORN_OK;
}

int untorn_sqlite_vfs_unregister(void) {
    int rc = sqlite3_vfs_unregister(&untorn_vfs);
    return (rc == SQLITE_OK) ? UNTORN_OK : UNTORN_ERR_NOT_SUPPORTED;
}
