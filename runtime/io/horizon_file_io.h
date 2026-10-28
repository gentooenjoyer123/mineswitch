/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#ifndef MINESWITCH_HORIZON_FILE_IO_H
#define MINESWITCH_HORIZON_FILE_IO_H
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/_iovec.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Regular-file operations shared by libjava, libnio and the VM. SD opens have
 * independent logical positions/access modes over a shared native backing.
 * The locks protect descriptor positions, including positioned/vector IO.
 * Callers must use the same bridge for every operation on a shared descriptor.
 * Socket IO uses the socket layer instead; no emulated dup aliases are offered.
 */
int nxio_open(const char *path, int flags, int mode);
int nxio_close(int fd);
ssize_t nxio_read(int fd, void *buffer, size_t size);
ssize_t nxio_write(int fd, const void *buffer, size_t size);
ssize_t nxio_pread(int fd, void *buffer, size_t size, off_t offset);
ssize_t nxio_pwrite(int fd, const void *buffer, size_t size, off_t offset);
ssize_t nxio_readv(int fd, const struct iovec *iov, int count);
ssize_t nxio_writev(int fd, const struct iovec *iov, int count);
off_t nxio_seek(int fd, off_t offset, int origin);
int nxio_fsync(int fd);
int nxio_truncate(int fd, off_t length);
int nxio_fstat(int fd, struct stat *info);
/* Path metadata, using an owned shared file's real descriptor only when fsdev's
 * extra-reader implementation fails with TargetLocked. No synthetic inode,
 * size, or clock-derived timestamps are substituted. */
int nxio_stat_path(const char *path, struct stat *info);
int nxio_lstat_path(const char *path, struct stat *info);
int nxio_descriptor_limit(void);
/* Returns a malloc-owned sdmc:/ path with stored directory-entry spelling.
 * Ambiguous/unverifiable non-ASCII case aliases fail with ENOTSUP. */
char *nxio_canonical_sd_path(const char *path);
/* Canonical pathname key for a tracked SD descriptor; caller frees. */
char *nxio_file_key(int fd);
/* Exclusive whole-file claim backed by the open writable Horizon FS handle.
 * Return 0 on grant/release, -1 with errno on failure. Shared/region locks are
 * not supported. FS mandatory exclusion lasts until the last local descriptor
 * for the writable backing closes, even after releasing this Java claim. */
int nxio_lock_whole_file(int fd);
int nxio_unlock_whole_file(int fd);
#ifdef __cplusplus
}
#endif
#endif
