/* Ordinary configuration and channel operations against the actual bridge. */
#include "horizon_file_io.h"
#include "horizon_file_share.h"
#include <sdk_shim.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks;
static void check(int ok, const char *name) {
    if (!ok) { fprintf(stderr, "FAIL %s errno=%d\n", name, errno); exit(1); }
    ++checks;
}
static void seed(const char *path, const char *text) {
    int fd = nxio_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    check(fd >= 0, "seed open");
    check(nxio_write(fd, text, strlen(text)) == (ssize_t)strlen(text), "seed data");
    check(nxio_close(fd) == 0, "seed close");
}
static void nested_channels(void) {
    const char *path = "sdmc:/owned/config-lock.tmp";
    int outer = nxio_open(path, O_CREAT | O_WRONLY, 0600);
    check(outer >= 0 && nxio_lock_whole_file(outer) == 0, "outer channel owns real whole-file claim");
    int inner = nxio_open(path, O_CREAT | O_WRONLY, 0600);
    check(inner >= 0 && inner != outer, "nested writer opens a distinct logical channel");
    check(nx_test_native_handles() == 1, "nested writers retain one native backing");
    char *first = nxio_file_key(outer), *second = nxio_file_key(inner);
    check(first && second && !strcmp(first, second), "nested Java channels share canonical overlap key");
    free(first); free(second);
    check(nxio_lock_whole_file(inner) < 0 && errno == EAGAIN, "second local whole-file claim is rejected");
    check(nxio_close(inner) == 0, "inner channel closes independently");
    check(nxio_lock_whole_file(outer) < 0 && errno == EAGAIN, "inner close does not release outer claim");
    FsFileSystem independent;
    check(fsOpenSdCardFileSystem(&independent) == 0, "independent filesystem session");
    FsFile contender;
    check(fsFsOpenFile(&independent, "/owned/config-lock.tmp", FsOpenMode_Write, &contender) == 0xe02,
          "real native writer still excludes independent session");
    check(nxio_unlock_whole_file(outer) == 0, "outer Java claim released");
    nx_test_fail_claim_probe();
    check(nxio_lock_whole_file(outer) < 0 && errno == ENOTSUP, "unrelated native failure never grants a lock");
    check(nxio_lock_whole_file(outer) == 0, "claim can be reacquired after an honest failure");
    check(nxio_close(outer) == 0, "closing owner releases native backing");
    check(fsFsOpenFile(&independent, "/owned/config-lock.tmp", FsOpenMode_Write, &contender) == 0,
          "independent writer succeeds after last close");
    fsFileClose(&contender); fsFsClose(&independent);
    check(nx_test_native_handles() == 0 && nx_test_sdk_handles() == 0, "nested channel cleanup");
}
static void config_read_then_write(void) {
    const char *path = "sdmc:/owned/config.properties";
    seed(path, "abcdef");
    int first = nxio_open(path, O_RDONLY, 0), second = nxio_open(path, O_RDONLY, 0);
    check(first >= 0 && second >= 0 && first != second, "independent readers open");
    char bytes[16] = {0};
    check(nxio_read(first, bytes, 3) == 3 && !memcmp(bytes, "abc", 3), "first reader position");
    check(nxio_read(second, bytes, 1) == 1 && bytes[0] == 'a', "second reader starts independently");
    check(nx_test_native_handles() == 1, "local readers share native access");
    int writer = nxio_open(path, O_WRONLY | O_TRUNC, 0600);
    check(writer >= 0, "configuration write opens while input streams remain alive");
    check(nxio_write(writer, "XYZ123", 6) == 6 && nxio_fsync(writer) == 0, "replacement data is real and flushed");
    check(nxio_read(first, bytes, 3) == 3 && !memcmp(bytes, "123", 3), "reader cursor survives access upgrade and observes new contents");
    check(nxio_read(second, bytes, 2) == 2 && !memcmp(bytes, "YZ", 2), "other reader cursor remains independent");
    check(nxio_write(first, "!", 1) < 0 && errno == EBADF, "read-only logical channel cannot write shared writable backing");
    check(nxio_read(writer, bytes, 1) < 0 && errno == EBADF, "write-only logical channel cannot read");
    check(nxio_truncate(first, 0) < 0 && errno == EBADF, "read-only channel cannot truncate");
    check(nxio_open(path, O_CREAT | O_EXCL | O_WRONLY, 0600) < 0 && errno == EEXIST, "exclusive create remains exclusive");
    check(nxio_close(writer) == 0, "writer closes while readers stay live");
    struct stat info;
    check(nxio_stat_path(path, &info) == 0 && info.st_size == 6, "real path metadata works with reader-owned writable backing");
    check(nxio_seek(first, 0, SEEK_SET) == 0 && nxio_read(first, bytes, 6) == 6 && !memcmp(bytes, "XYZ123", 6),
          "remaining reader still works after writer close");
    check(nxio_close(first) == 0 && nx_test_native_handles() == 1, "one reader close retains backing");
    check(nxio_close(second) == 0 && nx_test_native_handles() == 0, "last reader close releases backing");
}
static void positioned_channels(void) {
    const char *path = "sdmc:/owned/positions.bin";
    seed(path, "0123456789");
    int a = nxio_open(path, O_RDWR, 0), b = nxio_open(path, O_RDWR, 0);
    check(a >= 0 && b >= 0, "two read/write channels");
    check(nxio_seek(a, 2, SEEK_SET) == 2 && nxio_seek(b, 7, SEEK_SET) == 7, "different channel positions set");
    char bytes[8] = {0};
    check(nxio_pread(a, bytes, 3, 4) == 3 && !memcmp(bytes, "456", 3), "positioned read uses requested range");
    check(nxio_seek(a, 0, SEEK_CUR) == 2 && nxio_seek(b, 0, SEEK_CUR) == 7, "positioned read preserves both cursors");
    check(nxio_pwrite(b, "ab", 2, 1) == 2, "positioned write changes real bytes");
    check(nxio_seek(a, 0, SEEK_CUR) == 2 && nxio_seek(b, 0, SEEK_CUR) == 7, "positioned write preserves both cursors");
    check(nxio_truncate(a, 4) == 0, "shared file truncation succeeds");
    check(nxio_seek(b, 0, SEEK_CUR) == 7 && nxio_read(b, bytes, 1) == 0, "other channel position survives truncate beyond EOF");
    check(nxio_seek(b, 0, SEEK_CUR) == 7, "past-EOF read does not move logical position");
    check(nxio_pread(a, bytes, 3, 20) == 0 && nxio_seek(a, 0, SEEK_CUR) == 2, "positioned read past EOF restores its original cursor");
    bytes[0] = '!';
    check(nxio_seek(b, 4, SEEK_SET) == 4 && nxio_read(b, bytes, 1) == 0 && bytes[0] == '!', "at-EOF read leaves destination unchanged");
    check(nxio_seek(b, 2, SEEK_SET) == 2 && nxio_read(b, bytes, 6) == 2 && !memcmp(bytes, "b3", 2), "cross-EOF read returns only remaining real bytes");
    check(nxio_seek(b, 7, SEEK_SET) == 7, "position beyond EOF for metadata failure check");
    nx_test_fail_next_stat();
    check(nxio_read(b, bytes, 1) < 0 && errno == EIO, "failed size query is not converted to EOF");
    check(nxio_seek(b, 0, SEEK_SET) == 0, "position reset for read failure check");
    nx_test_fail_next_read();
    check(nxio_read(b, bytes, 1) < 0 && errno == EIO && nxio_seek(b, 0, SEEK_CUR) == 0, "genuine native read error is preserved");
    check(nxio_seek(b, 0, SEEK_SET) == 0 && nxio_read(b, bytes, 4) == 4 && !memcmp(bytes, "0ab3", 4), "truncated content is visible");
    check(nxio_seek(a, -1, SEEK_SET) < 0 && errno == EINVAL, "negative position rejected");
    check(nxio_close(a) == 0 && nxio_close(b) == 0, "positioned channels close");
}
struct writer { int fd; char tag; unsigned errors; };
static void *append_worker(void *argument) {
    struct writer *writer = argument;
    char prefix[2] = {writer->tag, '|'};
    struct iovec vectors[2] = {{prefix, 2}, {"line\n", 5}};
    for (unsigned i = 0; i < 200; ++i) if (nxio_writev(writer->fd, vectors, 2) != 7) ++writer->errors;
    return NULL;
}
static void concurrent_appends(void) {
    const char *path = "sdmc:/owned/append.txt";
    seed(path, "");
    struct writer writers[2] = {{nxio_open(path, O_WRONLY | O_APPEND, 0), 'A', 0},
                                {nxio_open(path, O_WRONLY | O_APPEND, 0), 'B', 0}};
    check(writers[0].fd >= 0 && writers[1].fd >= 0, "independent append channels");
    pthread_t threads[2];
    check(pthread_create(&threads[0], NULL, append_worker, &writers[0]) == 0, "append worker one");
    check(pthread_create(&threads[1], NULL, append_worker, &writers[1]) == 0, "append worker two");
    check(pthread_join(threads[0], NULL) == 0 && pthread_join(threads[1], NULL) == 0, "append workers join");
    check(writers[0].errors == 0 && writers[1].errors == 0, "all real vector writes succeed");
    int reader = nxio_open(path, O_RDONLY, 0);
    check(reader >= 0, "reader can coexist with append writers");
    char contents[2800];
    check(nxio_read(reader, contents, sizeof(contents)) == sizeof(contents), "complete append byte count");
    unsigned a = 0, b = 0;
    for (unsigned i = 0; i < sizeof(contents); i += 7) {
        check((contents[i] == 'A' || contents[i] == 'B') && !memcmp(contents + i + 1, "|line\n", 6),
              "vector append records never interleave");
        if (contents[i] == 'A') ++a; else ++b;
    }
    check(a == 200 && b == 200, "both append writers retain all records");
    check(nxio_close(writers[0].fd) == 0 && nxio_close(reader) == 0 && nxio_close(writers[1].fd) == 0,
          "concurrent channel lifecycle closes all backings");
}
static void upgrade_failures(void) {
    const char *path = "sdmc:/owned/upgrade.txt";
    seed(path, "abcdefgh");
    int reader = nxio_open(path, O_RDONLY, 0);
    check(reader >= 0 && nxio_seek(reader, 2, SEEK_SET) == 2, "reader before contended upgrade");
    FsFileSystem independent;
    FsFile foreign;
    check(fsOpenSdCardFileSystem(&independent) == 0 && fsFsOpenFile(&independent, "/owned/upgrade.txt", FsOpenMode_Read, &foreign) == 0,
          "independent native reader coexists before upgrade");
    check(nxio_open(path, O_WRONLY, 0) < 0 && errno == EACCES, "foreign reader causes honest writer-open failure");
    char byte;
    check(nxio_read(reader, &byte, 1) == 1 && byte == 'c', "failed upgrade restores existing reader and its position");
    fsFileClose(&foreign); fsFsClose(&independent);
    nx_test_fail_upgrade(1);
    check(nxio_open(path, O_WRONLY, 0) < 0 && errno == EACCES, "native upgrade and restore failure reported");
    check(nxio_read(reader, &byte, 1) < 0 && errno == EACCES, "unavailable backing never invents a successful read");
    check(nxio_open(path, O_RDONLY, 0) < 0 && errno == EACCES, "new reader cannot report success with unavailable native backing");
    nx_test_clear_failures();
    check(nxio_read(reader, &byte, 1) == 1 && byte == 'd', "reader recovers actual access without losing position");
    int writer = nxio_open(path, O_WRONLY, 0);
    check(writer >= 0, "writer upgrade retries after contention clears");
    check(nxio_close(writer) == 0 && nxio_close(reader) == 0, "upgrade retry closes cleanly");
}
static void libc_close_lifetime(void) {
    const char *path = "sdmc:/owned/lifetime.txt", *other = "sdmc:/owned/other.txt";
    seed(path, "first"); seed(other, "another ordinary file");
    int fd = nxio_open(path, O_WRONLY, 0);
    check(fd >= 0 && nxio_lock_whole_file(fd) == 0, "tracked writer before ordinary libc close");
    check(nx_test_close(fd) == 0 && nx_test_native_handles() == 0, "libc close releases backing and claim through devoptab");
    int raw = nx_test_open(other, O_RDONLY);
    check(raw == fd, "descriptor slot reused by unrelated SDK file");
    char *key = nxio_file_key(raw);
    check(!key && errno == ENOTSUP, "reused untracked descriptor cannot inherit a canonical key");
    free(key);
    FsFileSystem independent; FsFile foreign;
    check(fsOpenSdCardFileSystem(&independent) == 0 && fsFsOpenFile(&independent, "/owned/lifetime.txt", FsOpenMode_Write, &foreign) == 0,
          "separate native writer after libc close");
    struct stat info;
    check(nxio_stat_path(path, &info) < 0, "metadata cannot borrow an unrelated reused descriptor");
    fsFileClose(&foreign); fsFsClose(&independent);
    check(nxio_stat_path(path, &info) == 0 && info.st_size == 5, "metadata returns actual file after native writer closes");
    check(nx_test_close(raw) == 0, "untracked SDK descriptor closes normally");
}
static unsigned char fixture_byte(size_t offset, unsigned salt) {
    return (unsigned char)(offset * 29u + offset / 251u + salt);
}
static int fixture_matches(const char *data, size_t size, size_t offset, unsigned salt) {
    for (size_t i = 0; i < size; ++i)
        if ((unsigned char)data[i] != fixture_byte(offset + i, salt)) return 0;
    return 1;
}
static void seed_pattern(const char *path, size_t size, unsigned salt) {
    char *data = malloc(size);
    check(data != NULL, "allocate ordinary patterned fixture");
    for (size_t i = 0; i < size; ++i) data[i] = (char)fixture_byte(i, salt);
    int fd = nxio_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    check(fd >= 0 && nxio_write(fd, data, size) == (ssize_t)size && nxio_close(fd) == 0,
          "write and close ordinary patterned fixture");
    free(data);
}
static int same_backend_io(const struct nx_test_io_stats *a, const struct nx_test_io_stats *b) {
    return a->opens == b->opens && a->reads == b->reads && a->stats == b->stats && a->seeks == b->seeks;
}
static void jar_read_pages(void) {
    const char *path = "sdmc:/owned/ReadAhead.JaR";
    const size_t length = 3 * NXIO_JAR_READAHEAD_BYTES + 211;
    seed_pattern(path, length, 43);
    struct nx_test_io_stats before, after;
    struct nxio_shared_io_stats baseline, cold, warm;
    nxio_shared_io_snapshot(&baseline); nx_test_io_snapshot(&before);
    int a = nxio_open("sdmc:/owned/./ReadAhead.JaR", O_RDONLY, 0);
    int b = nxio_open("sdmc:/owned/../owned/ReadAhead.JaR", O_RDONLY, 0);
    check(a >= 0 && b >= 0 && a != b, "canonical aliases of mixed-case JAR suffix open independent readers");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&cold);
    check(after.opens == before.opens + 1 && cold.cache_bytes == baseline.cache_bytes,
          "canonical readers share one native handle and allocate cache lazily");
    char bytes[64];
    check(nxio_seek(a, 123, SEEK_SET) == 123, "JAR initial logical offset");
    nx_test_io_snapshot(&before);
    check(nxio_read(a, bytes, 37) == 37 && fixture_matches(bytes, 37, 123, 43), "cold JAR read returns exact requested bytes");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&cold);
    check(after.reads == before.reads + 1 && after.stats == before.stats + 1 && after.seeks == before.seeks + 1 &&
          after.read_bytes == before.read_bytes + NXIO_JAR_CACHE_PAGE_BYTES,
          "cold small JAR read performs one real aligned 4 KiB read with one seek and stat");
    check(cold.cache_bytes == baseline.cache_bytes + NXIO_JAR_READAHEAD_BYTES &&
          cold.native_bytes == baseline.native_bytes + NXIO_JAR_CACHE_PAGE_BYTES &&
          cold.logical_bytes == baseline.logical_bytes + 37,
          "JAR telemetry distinguishes requested bytes, one filled page and 32 KiB allocation");
    FsFileSystem independent; FsFile contender;
    check(fsOpenSdCardFileSystem(&independent) == 0 &&
          fsFsOpenFile(&independent, "/owned/ReadAhead.JaR", FsOpenMode_Write, &contender) == 0xe02,
          "cached native reader keeps genuine foreign writer exclusion");
    fsFsClose(&independent);
    nx_test_io_snapshot(&before);
    check(nxio_read(a, bytes, 17) == 17 && fixture_matches(bytes, 17, 160, 43), "adjacent JAR read uses correct bytes");
    check(nxio_pread(b, bytes, 37, 123) == 37 && fixture_matches(bytes, 37, 123, 43), "other descriptor reuses shared JAR page");
    const off_t offsets[] = {333, 2000, 3200, 789};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
        check(nxio_pread(b, bytes, 29, offsets[i]) == 29 && fixture_matches(bytes, 29, (size_t)offsets[i], 43),
              "random and backward read inside aligned JAR page returns genuine bytes");
    check(nxio_pread(b, bytes, 1, NXIO_JAR_CACHE_PAGE_BYTES - 1) == 1 &&
          fixture_matches(bytes, 1, NXIO_JAR_CACHE_PAGE_BYTES - 1, 43), "last aligned cached byte is a hit");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&warm);
    check(same_backend_io(&before, &after), "seven cached JAR reads make zero native open/read/stat/seek calls");
    check(warm.cache_hits == cold.cache_hits + 7 && warm.logical_reads == cold.logical_reads + 7 &&
          warm.native_reads == cold.native_reads && warm.native_stats == cold.native_stats,
          "cache telemetry agrees with independent backend operation counters");
    check(nxio_seek(a, 0, SEEK_CUR) == 177 && nxio_seek(b, 0, SEEK_CUR) == 0,
          "shared cached reads preserve separate cursors and pread restores its cursor");
    const off_t distant[] = {20 * NXIO_JAR_CACHE_PAGE_BYTES + 53, 11 * NXIO_JAR_CACHE_PAGE_BYTES + 101,
                             2 * NXIO_JAR_CACHE_PAGE_BYTES + 19};
    nx_test_io_snapshot(&before); nxio_shared_io_snapshot(&cold);
    for (unsigned i = 0; i < 3; ++i)
        check(nxio_pread(b, bytes, 30, distant[i]) == 30 && fixture_matches(bytes, 30, (size_t)distant[i], 43),
              "tiny distant header and directory reads fetch genuine pages");
    for (unsigned i = 0; i < 24; ++i) {
        off_t offset = distant[i % 3] - (off_t)(i % 11);
        check(nxio_pread(b, bytes, 30, offset) == 30 && fixture_matches(bytes, 30, (size_t)offset, 43),
              "alternating distant and backward tiny reads retain independent cached pages");
    }
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&warm);
    check(after.reads == before.reads + 3 && after.seeks == before.seeks + 3 && after.stats == before.stats &&
          after.read_bytes == before.read_bytes + 3 * NXIO_JAR_CACHE_PAGE_BYTES,
          "27 alternating tiny header/directory reads fetch just three 4 KiB pages");
    check(warm.cache_hits == cold.cache_hits + 24 && warm.logical_reads == cold.logical_reads + 27,
          "only whole logical reads served from preexisting pages count as cache hits");
    nx_test_io_snapshot(&before);
    check(nxio_pread(b, bytes, 32, 6 * NXIO_JAR_CACHE_PAGE_BYTES - 16) == 32 &&
          fixture_matches(bytes, 32, 6 * NXIO_JAR_CACHE_PAGE_BYTES - 16, 43), "cold cross-page request joins two genuine page fragments");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 2 && after.seeks == before.seeks + 2 && after.stats == before.stats &&
          after.read_bytes == before.read_bytes + 2 * NXIO_JAR_CACHE_PAGE_BYTES,
          "cold cross-page range fills exactly two aligned pages and reuses stable size");
    nx_test_io_snapshot(&before); nxio_shared_io_snapshot(&cold);
    check(nxio_pread(b, bytes, 32, 6 * NXIO_JAR_CACHE_PAGE_BYTES - 16) == 32 &&
          fixture_matches(bytes, 32, 6 * NXIO_JAR_CACHE_PAGE_BYTES - 16, 43), "cached cross-page request joins retained page fragments");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&warm);
    check(same_backend_io(&before, &after) && warm.cache_hits == cold.cache_hits + 1,
          "whole logical cross-page hit makes no backend calls and counts once");
    nx_test_io_snapshot(&before); nxio_shared_io_snapshot(&cold);
    check(nxio_pread(b, bytes, 32, 2 * NXIO_JAR_CACHE_PAGE_BYTES - 16) == 32 &&
          fixture_matches(bytes, 32, 2 * NXIO_JAR_CACHE_PAGE_BYTES - 16, 43), "mixed cold/warm cross-page read joins correct bytes");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&warm);
    check(after.reads == before.reads + 1 && after.read_bytes == before.read_bytes + NXIO_JAR_CACHE_PAGE_BYTES &&
          warm.cache_hits == cold.cache_hits, "partial page-cache coverage fills only missing page and does not claim a whole-read hit");
    char *large = malloc(NXIO_JAR_READAHEAD_BYTES + 1);
    check(large != NULL, "allocate large JAR destination");
    nx_test_io_snapshot(&before);
    check(nxio_pread(b, large, NXIO_JAR_READAHEAD_BYTES + 1, 0) == NXIO_JAR_READAHEAD_BYTES + 1 &&
          fixture_matches(large, NXIO_JAR_READAHEAD_BYTES + 1, 0, 43), "oversized JAR request reads actual direct bytes");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 1 && after.read_bytes == before.read_bytes + NXIO_JAR_READAHEAD_BYTES + 1,
          "oversized request issues exactly one requested-size native read");
    free(large);
    nx_test_io_snapshot(&before);
    check(nxio_seek(b, 0, SEEK_END) == (off_t)length, "JAR SEEK_END reuses verified size");
    bytes[0] = '!';
    check(nxio_read(b, bytes, 8) == 0 && bytes[0] == '!' && nxio_seek(b, 0, SEEK_CUR) == (off_t)length,
          "cached-size EOF preserves buffer and logical cursor");
    check(nxio_pread(a, bytes, 8, (off_t)length + 100) == 0 && nxio_seek(a, 0, SEEK_CUR) == 177,
          "beyond-EOF positioned JAR read preserves original logical cursor");
    nx_test_io_snapshot(&after);
    check(same_backend_io(&before, &after), "stable JAR size avoids all native EOF and SEEK_END work");
    struct stat info;
    check(nxio_fstat(a, &info) == 0 && info.st_size == (off_t)length, "explicit JAR fstat returns genuine file metadata");
    nx_test_io_snapshot(&after);
    check(after.stats == before.stats + 1, "explicit JAR fstat remains a real backend stat");
    memset(bytes, '!', sizeof(bytes)); nx_test_io_snapshot(&before);
    check(nxio_pread(a, bytes, 20, (off_t)length - 5) == 5 && fixture_matches(bytes, 5, length - 5, 43) && bytes[5] == '!',
          "cross-EOF cached read returns only real remaining bytes");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 1 && after.read_bytes == before.read_bytes + 211,
          "last aligned JAR page fill is clipped to genuine EOF");
    check(nxio_close(a) == 0, "one JAR descriptor closes independently");
    nx_test_io_snapshot(&before);
    check(nxio_pread(b, bytes, 4, (off_t)length - 4) == 4 && fixture_matches(bytes, 4, length - 4, 43),
          "remaining JAR descriptor retains native lifetime and cached data");
    nx_test_io_snapshot(&after);
    check(same_backend_io(&before, &after), "intermediate logical close retains shared JAR pages");
    check(nxio_close(b) == 0, "last JAR descriptor closes");
    nxio_shared_io_snapshot(&warm);
    check(warm.cache_bytes == baseline.cache_bytes, "last JAR close releases its allocated page cache");
    check(unlink(path) == 0, "remove read-ahead fixture");
}
static void jar_page_lru(void) {
    const char *path = "sdmc:/owned/lru.jar";
    const size_t page = NXIO_JAR_CACHE_PAGE_BYTES;
    seed_pattern(path, (NXIO_JAR_CACHE_PAGES + 3) * page, 61);
    int fd = nxio_open(path, O_RDONLY, 0);
    check(fd >= 0, "JAR LRU fixture opens");
    char bytes[32]; struct nx_test_io_stats before, after;
    struct nxio_shared_io_stats baseline, full;
    nxio_shared_io_snapshot(&baseline); nx_test_io_snapshot(&before);
    for (unsigned i = 0; i < NXIO_JAR_CACHE_PAGES; ++i)
        check(nxio_pread(fd, bytes, 8, (off_t)(i * page + 101)) == 8 && fixture_matches(bytes, 8, i * page + 101, 61),
              "eight tiny reads fill eight separate aligned page slots");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&full);
    check(after.reads == before.reads + NXIO_JAR_CACHE_PAGES &&
          after.read_bytes == before.read_bytes + NXIO_JAR_CACHE_PAGES * page &&
          full.cache_bytes == baseline.cache_bytes + NXIO_JAR_READAHEAD_BYTES,
          "all eight retained pages consume exactly one 32 KiB backing allocation");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 8, 19) == 8 && fixture_matches(bytes, 8, 19, 61), "backward first-page hit refreshes its LRU priority");
    nx_test_io_snapshot(&after);
    check(same_backend_io(&before, &after), "backward reads reuse data preceding original fill request");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 8, (off_t)(NXIO_JAR_CACHE_PAGES * page + 55)) == 8 &&
          fixture_matches(bytes, 8, NXIO_JAR_CACHE_PAGES * page + 55, 61), "ninth distinct page fits by evicting one LRU slot");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 1 && after.read_bytes == before.read_bytes + page, "LRU replacement fills only one new 4 KiB page");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 8, 11) == 8 && fixture_matches(bytes, 8, 11, 61) &&
          nxio_pread(fd, bytes, 8, (off_t)(2 * page + 77)) == 8 && fixture_matches(bytes, 8, 2 * page + 77, 61),
          "recently touched page zero and unevicted page two retain genuine data");
    nx_test_io_snapshot(&after);
    check(same_backend_io(&before, &after), "LRU retains recently touched and non-oldest slots");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 8, (off_t)(page + 101)) == 8 && fixture_matches(bytes, 8, page + 101, 61),
          "oldest untouched page one can be read again correctly");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 1 && after.read_bytes == before.read_bytes + page,
          "page one was actually evicted rather than allowing a ninth retained slot");
    check(nxio_close(fd) == 0, "LRU fixture native lifetime closes");
    fd = nxio_open(path, O_RDONLY, 0);
    char *large = malloc(NXIO_JAR_READAHEAD_BYTES);
    check(fd >= 0 && large != NULL, "unaligned maximum-size page-cache destination opens");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fd, large, NXIO_JAR_READAHEAD_BYTES, 123) == NXIO_JAR_READAHEAD_BYTES &&
          fixture_matches(large, NXIO_JAR_READAHEAD_BYTES, 123, 61), "32 KiB unaligned logical read safely crosses nine pages with eight slots");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + NXIO_JAR_CACHE_PAGES + 1 &&
          after.read_bytes == before.read_bytes + (NXIO_JAR_CACHE_PAGES + 1) * page,
          "unaligned maximum-size read fills exactly its nine real aligned pages");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fd, large, NXIO_JAR_READAHEAD_BYTES, (off_t)page) == NXIO_JAR_READAHEAD_BYTES &&
          fixture_matches(large, NXIO_JAR_READAHEAD_BYTES, page, 61), "most recent eight pages remain reusable after nine-page logical read");
    nx_test_io_snapshot(&after);
    check(same_backend_io(&before, &after), "full 32 KiB logical read from eight resident pages avoids all backend IO");
    free(large);
    check(nxio_close(fd) == 0 && unlink(path) == 0, "LRU fixture closes and is removed");
}
static void jar_direct_and_writable(void) {
    const char *paths[] = {"sdmc:/owned/archive.jar.tmp", "sdmc:/owned/writable.jar", "sdmc:/owned/direct.jar"};
    char bytes[32]; struct nx_test_io_stats before, after;
    struct nxio_shared_io_stats baseline, now;
    nxio_shared_io_snapshot(&baseline);
    seed_pattern(paths[0], 97, 5);
    int reader = nxio_open(paths[0], O_RDONLY, 0);
    check(reader >= 0, "non-JAR suffix fixture opens");
    nx_test_io_snapshot(&before);
    for (int i = 0; i < 2; ++i)
        check(nxio_pread(reader, bytes, 16, 3) == 16 && fixture_matches(bytes, 16, 3, 5), "non-JAR reads remain genuine");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&now);
    check(after.reads == before.reads + 2 && after.stats == before.stats + 2 && after.seeks == before.seeks + 2 &&
          now.cache_bytes == baseline.cache_bytes, "JAR-looking non-suffix path never caches bytes or size");
    check(nxio_close(reader) == 0, "non-JAR fixture closes");
    seed_pattern(paths[1], 97, 9);
    int writer = nxio_open(paths[1], O_RDWR, 0);
    reader = nxio_open(paths[1], O_RDONLY, 0);
    check(writer >= 0 && reader >= 0, "writable JAR backing and logical reader open");
    nx_test_io_snapshot(&before);
    for (int i = 0; i < 2; ++i)
        check(nxio_pread(reader, bytes, 16, 3) == 16 && fixture_matches(bytes, 16, 3, 9), "mutable JAR reads real contents");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&now);
    check(after.reads == before.reads + 2 && after.stats == before.stats + 2 && now.cache_bytes == baseline.cache_bytes,
          "writable native JAR backing never caches data or size");
    check(nxio_pwrite(writer, "changed!", 8, 0) == 8 && nxio_close(writer) == 0, "writer changes JAR bytes then closes its logical fd");
    nx_test_io_snapshot(&before);
    for (int i = 0; i < 2; ++i)
        check(nxio_pread(reader, bytes, 8, 0) == 8 && !memcmp(bytes, "changed!", 8), "remaining reader sees actual changed JAR data");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&now);
    check(after.reads == before.reads + 2 && after.stats == before.stats + 2 && now.cache_bytes == baseline.cache_bytes,
          "writer logical close does not make its native writable backing cacheable");
    check(nxio_close(reader) == 0, "mutable JAR native lifetime ends");
    reader = nxio_open(paths[1], O_RDONLY, 0);
    check(reader >= 0 && nxio_read(reader, bytes, 8) == 8 && !memcmp(bytes, "changed!", 8), "fresh read-only lifetime sees changed real contents");
    nxio_shared_io_snapshot(&now);
    check(now.cache_bytes == baseline.cache_bytes + NXIO_JAR_READAHEAD_BYTES, "fresh genuine read-only native lifetime permits caching");
    check(nxio_close(reader) == 0, "fresh readonly JAR closes");
    seed_pattern(paths[2], 97, 21);
    reader = nxio_open(paths[2], O_RDONLY, 0);
    char *large = malloc(NXIO_JAR_READAHEAD_BYTES + 1);
    check(reader >= 0 && large != NULL, "oversized near-EOF fixture opens");
    memset(large, '!', NXIO_JAR_READAHEAD_BYTES + 1); nx_test_io_snapshot(&before);
    check(nxio_pread(reader, large, NXIO_JAR_READAHEAD_BYTES + 1, 90) == 7 &&
          fixture_matches(large, 7, 90, 21) && large[7] == '!', "oversized near-EOF read preserves clipped result and destination");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&now);
    check(after.reads == before.reads + 1 && after.read_bytes == before.read_bytes + 7 && now.cache_bytes == baseline.cache_bytes,
          "original oversized request remains direct even when EOF clips below window size");
    free(large);
    check(nxio_close(reader) == 0, "direct JAR fixture closes");
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) check(unlink(paths[i]) == 0, "remove uncached fixture");
}
static void jar_native_failures(void) {
    const char *path = "sdmc:/owned/errors.jar";
    seed_pattern(path, 3 * NXIO_JAR_READAHEAD_BYTES, 17);
    int fd = nxio_open(path, O_RDONLY, 0);
    check(fd >= 0, "JAR error fixture opens");
    struct nx_test_io_stats before, after;
    char bytes[32]; memset(bytes, '!', sizeof(bytes));
    nx_test_fail_next_stat(); nx_test_io_snapshot(&before);
    check(nxio_read(fd, bytes, 16) < 0 && errno == EIO && bytes[0] == '!' && nxio_seek(fd, 0, SEEK_CUR) == 0,
          "cold JAR stat failure preserves error, buffer and position");
    nx_test_io_snapshot(&after);
    check(after.stats == before.stats + 1 && after.reads == before.reads && after.seeks == before.seeks,
          "failed JAR stat prevents native seek and read");
    nx_test_fail_next_seek(); nx_test_io_snapshot(&before);
    check(nxio_read(fd, bytes, 16) < 0 && errno == EIO && bytes[0] == '!' && nxio_seek(fd, 0, SEEK_CUR) == 0,
          "JAR fill seek failure remains a real error");
    nx_test_io_snapshot(&after);
    check(after.seeks == before.seeks + 1 && after.reads == before.reads, "failed fill seek never claims a native read");
    nx_test_fail_next_read();
    check(nxio_read(fd, bytes, 16) < 0 && errno == EIO && bytes[0] == '!' && nxio_seek(fd, 0, SEEK_CUR) == 0,
          "JAR fill read failure preserves buffer and logical position");
    nx_test_shorten_next_read(5); nx_test_io_snapshot(&before);
    check(nxio_read(fd, bytes, 16) == 5 && fixture_matches(bytes, 5, 0, 17) && bytes[5] == '!' && nxio_seek(fd, 0, SEEK_CUR) == 5,
          "short native JAR fill returns only actual initialized bytes and advances by five");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 1 && after.read_bytes == before.read_bytes + 5, "shim short fill reports actual backend bytes");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 5, 0) == 5 && fixture_matches(bytes, 5, 0, 17), "initialized short-fill bytes are reusable");
    nx_test_io_snapshot(&after);
    check(same_backend_io(&before, &after), "short-fill hit issues zero backend calls");
    nx_test_fail_next_read();
    check(nxio_pread(fd, bytes, 1, 5) < 0 && errno == EIO && nxio_seek(fd, 0, SEEK_CUR) == 5,
          "first byte beyond short fill performs real read and preserves genuine failure");
    check(nxio_read(fd, bytes, 16) == 16 && fixture_matches(bytes, 16, 5, 17), "JAR read recovers correctly after short-fill miss failure");
    nx_test_fail_next_seek(); nx_test_fail_next_read(); nx_test_fail_next_stat(); nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 8, 5) == 8 && fixture_matches(bytes, 8, 5, 17), "valid JAR cache hit requires no injected backend operations");
    nx_test_io_snapshot(&after);
    check(same_backend_io(&before, &after), "cache hit bypasses backend while preserving pending error injections");
    struct stat info;
    check(nxio_fstat(fd, &info) < 0 && errno == EIO, "explicit stat still observes genuine failure after a cache hit");
    check(nxio_pread(fd, bytes, 8, 2 * NXIO_JAR_READAHEAD_BYTES) < 0 && errno == EIO,
          "uncached JAR range observes pending genuine seek failure");
    check(nxio_pread(fd, bytes, 8, 2 * NXIO_JAR_READAHEAD_BYTES) < 0 && errno == EIO,
          "next uncached JAR range observes pending genuine read failure");
    nx_test_shorten_next_read(0); bytes[0] = '!';
    check(nxio_pread(fd, bytes, 8, 2 * NXIO_JAR_READAHEAD_BYTES) == 0 && bytes[0] == '!' && nxio_seek(fd, 0, SEEK_CUR) == 21,
          "zero-byte native fill invents neither contents nor cursor movement");
    check(nxio_pread(fd, bytes, 8, 2 * NXIO_JAR_READAHEAD_BYTES) == 8 &&
          fixture_matches(bytes, 8, 2 * NXIO_JAR_READAHEAD_BYTES, 17), "zero-byte short fill does not become a false cached EOF");
    check(nxio_close(fd) == 0 && unlink(path) == 0, "JAR failure fixture closes and is removed");
}
static int reopen_jar_reader(int fd, const char *path) {
    check(nxio_close(fd) == 0, "end JAR native lifetime before independent short-read case");
    fd = nxio_open(path, O_RDONLY, 0);
    check(fd >= 0, "new uncached JAR lifetime for short-read case");
    return fd;
}
static void jar_page_short_reads(void) {
    const char *path = "sdmc:/owned/page-errors.jar";
    const size_t page = NXIO_JAR_CACHE_PAGE_BYTES;
    seed_pattern(path, 3 * page, 83);
    int fd = nxio_open(path, O_RDONLY, 0);
    check(fd >= 0, "JAR page short-read fixture opens");
    char bytes[64]; struct nx_test_io_stats before, after;
    struct nxio_shared_io_stats logical_before, logical_after;
    nx_test_queue_read_result(7); nx_test_queue_read_result(20);
    nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 20, 100) == 20 && fixture_matches(bytes, 20, 100, 83),
          "page fill ending before requested offset falls back to requested real range");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 2 && after.seeks == before.seeks + 2 && after.read_bytes == before.read_bytes + 27,
          "incomplete prefix invokes exactly one bounded direct fallback instead of false EOF or repeated refill");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 1, 0) == 1 && fixture_matches(bytes, 1, 0, 83), "discarded incomplete slot can refill genuine page prefix");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 1 && after.read_bytes == before.read_bytes + page,
          "prefix-only incomplete page is discarded after direct fallback");
    fd = reopen_jar_reader(fd, path);
    check(nxio_seek(fd, 100, SEEK_SET) == 100, "position reader inside an uncached page");
    memset(bytes, '!', sizeof(bytes)); nx_test_queue_read_result(110); nx_test_io_snapshot(&before);
    check(nxio_read(fd, bytes, 20) == 10 && fixture_matches(bytes, 10, 100, 83) && bytes[10] == '!' &&
          nxio_seek(fd, 0, SEEK_CUR) == 110, "short page fill covering requested offset returns only genuine suffix progress");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 1 && after.read_bytes == before.read_bytes + 110,
          "partially covered requested fragment stops at native short fill without retry");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 10, 100) == 10 && fixture_matches(bytes, 10, 100, 83), "genuine initialized suffix of short page remains readable");
    nx_test_io_snapshot(&after);
    check(same_backend_io(&before, &after), "fully covered portion of a short page can be a genuine cache hit");
    nx_test_fail_next_read();
    check(nxio_pread(fd, bytes, 1, 110) < 0 && errno == EIO && nxio_seek(fd, 0, SEEK_CUR) == 110,
          "first byte beyond initialized short page still observes actual backend read failure");
    const ssize_t fallback_results[] = {3, -1, 0};
    for (unsigned i = 0; i < sizeof(fallback_results) / sizeof(fallback_results[0]); ++i) {
        fd = reopen_jar_reader(fd, path);
        check(nxio_seek(fd, 100, SEEK_SET) == 100, "set offset for bounded fallback outcome");
        memset(bytes, '!', sizeof(bytes));
        nx_test_queue_read_result(7); nx_test_queue_read_result(fallback_results[i]); nx_test_io_snapshot(&before);
        ssize_t result = nxio_read(fd, bytes, 20);
        int saved = errno;
        check(result == fallback_results[i] && (result != -1 || saved == EIO) &&
              (result <= 0 ? bytes[0] == '!' : fixture_matches(bytes, (size_t)result, 100, 83) && bytes[result] == '!') &&
              nxio_seek(fd, 0, SEEK_CUR) == 100 + (result > 0 ? result : 0),
              "single direct fallback preserves its real short, zero or error outcome and logical cursor");
        nx_test_io_snapshot(&after);
        check(after.reads == before.reads + 2 && after.read_bytes == before.read_bytes + 7 + (result > 0 ? (uint64_t)result : 0),
              "fallback short/error/zero never retries indefinitely or claims unread bytes");
        check(nxio_pread(fd, bytes, 20, 100) == 20 && fixture_matches(bytes, 20, 100, 83),
              "later read can recover actual contents after bounded fallback outcome");
    }
    fd = reopen_jar_reader(fd, path);
    nx_test_queue_read_result(0); nx_test_queue_read_result(20); nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 20, 100) == 20 && fixture_matches(bytes, 20, 100, 83),
          "zero-byte prefix fill at nonzero requested offset gets one genuine direct fallback");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 2 && after.read_bytes == before.read_bytes + 20,
          "empty prefix fallback performs exactly two attempts and counts only real returned bytes");
    fd = reopen_jar_reader(fd, path);
    nx_test_queue_read_result(7); nx_test_queue_read_result(16); nx_test_io_snapshot(&before);
    check(nxio_pread(fd, bytes, 16, (off_t)page - 8) == 16 && fixture_matches(bytes, 16, page - 8, 83),
          "prefix-short fallback can read all requested bytes across the next page boundary");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 2 && after.read_bytes == before.read_bytes + 23,
          "single direct fallback covers the entire remaining logical range without another page fill");
    fd = reopen_jar_reader(fd, path);
    check(nxio_seek(fd, (off_t)page - 8, SEEK_SET) == (off_t)page - 8, "position crossing two uncached pages");
    memset(bytes, '!', sizeof(bytes)); nx_test_queue_read_result((ssize_t)page); nx_test_queue_read_result(-1);
    nx_test_io_snapshot(&before); nxio_shared_io_snapshot(&logical_before);
    check(nxio_read(fd, bytes, 16) == 8 && fixture_matches(bytes, 8, page - 8, 83) && bytes[8] == '!' &&
          nxio_seek(fd, 0, SEEK_CUR) == (off_t)page, "second-page read failure returns first-page bytes already copied");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&logical_after);
    check(after.reads == before.reads + 2 && after.read_bytes == before.read_bytes + page &&
          logical_after.logical_bytes == logical_before.logical_bytes + 8 && logical_after.cache_hits == logical_before.cache_hits,
          "partial-progress telemetry counts copied bytes and never labels a failed mixed read as a whole cache hit");
    check(nxio_read(fd, bytes, 16) == 16 && fixture_matches(bytes, 16, page, 83), "next logical read resumes at actual partial-progress cursor");
    for (int fail_seek = 0; fail_seek < 2; ++fail_seek) {
        fd = reopen_jar_reader(fd, path);
        check(nxio_pread(fd, bytes, 8, (off_t)page - 8) == 8 && nxio_seek(fd, (off_t)page - 8, SEEK_SET) == (off_t)page - 8,
              "cache first page before a later-page native failure");
        memset(bytes, '!', sizeof(bytes));
        if (fail_seek) nx_test_fail_next_seek(); else nx_test_fail_next_read();
        nx_test_io_snapshot(&before); nxio_shared_io_snapshot(&logical_before);
        check(nxio_read(fd, bytes, 16) == 8 && fixture_matches(bytes, 8, page - 8, 83) && bytes[8] == '!' &&
              nxio_seek(fd, 0, SEEK_CUR) == (off_t)page, "later-page seek/read failure preserves prior cached progress");
        nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&logical_after);
        check(after.seeks == before.seeks + 1 && after.reads == before.reads + (fail_seek ? 0 : 1) &&
              logical_after.cache_hits == logical_before.cache_hits,
              "cached prefix plus failed native suffix is not counted as a complete logical cache hit");
    }
    fd = reopen_jar_reader(fd, path);
    check(nxio_seek(fd, (off_t)page - 8, SEEK_SET) == (off_t)page - 8, "position for later-page positive short fill");
    memset(bytes, '!', sizeof(bytes)); nx_test_queue_read_result((ssize_t)page); nx_test_queue_read_result(3);
    nx_test_io_snapshot(&before);
    check(nxio_read(fd, bytes, 16) == 11 && fixture_matches(bytes, 11, page - 8, 83) && bytes[11] == '!' &&
          nxio_seek(fd, 0, SEEK_CUR) == (off_t)page + 3, "later-page positive short fill returns exact accumulated real progress");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 2 && after.read_bytes == before.read_bytes + page + 3,
          "later-page short fill ends logical call after two genuine native reads");
    fd = reopen_jar_reader(fd, path);
    check(nxio_seek(fd, (off_t)page - 8, SEEK_SET) == (off_t)page - 8, "position for later-page aligned zero fill");
    memset(bytes, '!', sizeof(bytes)); nx_test_queue_read_result((ssize_t)page); nx_test_queue_read_result(0);
    nx_test_io_snapshot(&before);
    check(nxio_read(fd, bytes, 16) == 8 && fixture_matches(bytes, 8, page - 8, 83) && bytes[8] == '!' &&
          nxio_seek(fd, 0, SEEK_CUR) == (off_t)page, "later-page aligned zero fill returns earlier bytes without inventing suffix progress");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 2 && after.read_bytes == before.read_bytes + page,
          "observed aligned zero ends logical call without an unwarranted direct retry");
    check(nxio_read(fd, bytes, 8) == 8 && fixture_matches(bytes, 8, page, 83), "next call retries real data after earlier aligned zero fill");
    check(nxio_close(fd) == 0 && unlink(path) == 0, "page error fixture closes and is removed");
}
static void jar_upgrade_invalidation(void) {
    const char *path = "sdmc:/owned/upgrade.jar";
    seed(path, "abcdefgh");
    struct nxio_shared_io_stats baseline, now;
    struct nx_test_io_stats before, after;
    nxio_shared_io_snapshot(&baseline);
    int reader = nxio_open(path, O_RDONLY, 0);
    char bytes[16];
    check(reader >= 0 && nxio_read(reader, bytes, 4) == 4 && !memcmp(bytes, "abcd", 4), "JAR upgrade begins with cached readonly data");
    FsFileSystem independent; FsFile foreign;
    check(fsOpenSdCardFileSystem(&independent) == 0 &&
          fsFsOpenFile(&independent, "/owned/upgrade.jar", FsOpenMode_Read, &foreign) == 0, "independent JAR reader permits genuine upgrade contention");
    nx_test_io_snapshot(&before);
    check(nxio_open(path, O_WRONLY, 0) < 0 && errno == EACCES, "contended cached JAR upgrade fails honestly");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&now);
    check(after.opens == before.opens + 2 && now.cache_bytes == baseline.cache_bytes,
          "failed upgrade closes old lifetime, discards cache and really restores readonly native handle");
    nx_test_io_snapshot(&before);
    check(nxio_pread(reader, bytes, 4, 0) == 4 && !memcmp(bytes, "abcd", 4) && nxio_seek(reader, 0, SEEK_CUR) == 4,
          "restored JAR reader preserves its logical cursor and genuine contents");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 1 && after.stats == before.stats + 1,
          "restored JAR lifetime revalidates real size and reads fresh bytes");
    fsFileClose(&foreign); fsFsClose(&independent);
    int writer = nxio_open(path, O_WRONLY | O_TRUNC, 0);
    check(writer >= 0, "cached JAR upgrades to real writable backing");
    nxio_shared_io_snapshot(&now);
    check(now.cache_bytes == baseline.cache_bytes, "successful upgrade frees readonly JAR cache before mutation");
    check(nxio_write(writer, "NEW!", 4) == 4 && nxio_read(reader, bytes, 4) == 0,
          "upgraded JAR reader uses new truncated size at retained cursor");
    check(nxio_pread(reader, bytes, 8, 0) == 4 && !memcmp(bytes, "NEW!", 4), "upgraded JAR reader sees new bytes instead of old cache");
    check(nxio_truncate(writer, 2) == 0 && nxio_pread(reader, bytes, 8, 0) == 2 && !memcmp(bytes, "NE", 2),
          "subsequent JAR truncate keeps real size and bytes visible");
    check(nxio_close(writer) == 0, "JAR upgrade writer closes while reader lives");
    nx_test_io_snapshot(&before);
    check(nxio_pread(reader, bytes, 2, 0) == 2 && nxio_pread(reader, bytes, 2, 0) == 2, "remaining upgraded reader continues real reads");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&now);
    check(after.reads == before.reads + 2 && after.stats == before.stats + 2 && now.cache_bytes == baseline.cache_bytes,
          "upgraded native backing stays uncached after writer logical close");
    check(nxio_close(reader) == 0, "JAR upgraded backing closes");
    seed(path, "abcdefgh"); reader = nxio_open(path, O_RDONLY, 0);
    check(reader >= 0 && nxio_read(reader, bytes, 4) == 4, "JAR failed-restore fixture has a populated cache");
    nx_test_fail_upgrade(1);
    check(nxio_open(path, O_WRONLY, 0) < 0 && errno == EACCES, "cached JAR upgrade and restore both fail honestly");
    nxio_shared_io_snapshot(&now);
    check(now.cache_bytes == baseline.cache_bytes, "failed native restore cannot retain cached JAR allocation");
    check(nxio_pread(reader, bytes, 4, 0) < 0 && errno == EACCES && nxio_seek(reader, 0, SEEK_CUR) == 4,
          "unavailable JAR backing never serves old cached bytes and retains logical cursor");
    check(nxio_open(path, O_RDONLY, 0) < 0 && errno == EACCES, "unavailable JAR backing rejects new reader honestly");
    nx_test_clear_failures();
    int raw = nx_test_open(path, O_WRONLY | O_TRUNC, 0600);
    check(raw >= 0 && nx_test_write(raw, "new", 3) == 3 && nx_test_close(raw) == 0,
          "independent native writer changes real file only while old reader exclusion is absent");
    nx_test_io_snapshot(&before);
    check(nxio_pread(reader, bytes, 8, 0) == 3 && !memcmp(bytes, "new", 3), "reopened JAR reader gets actual changed data and shorter size");
    nx_test_io_snapshot(&after);
    check(after.opens == before.opens + 1 && after.stats == before.stats + 1 && after.reads == before.reads + 1,
          "recovery really reopens, restats and rereads the modified JAR");
    check(nxio_read(reader, bytes, 1) == 0 && nxio_seek(reader, 0, SEEK_CUR) == 4 && nxio_seek(reader, 0, SEEK_END) == 3,
          "recovered JAR cursor remains beyond genuine new EOF until explicitly moved");
    check(nxio_close(reader) == 0 && unlink(path) == 0, "JAR upgrade fixture closes and is removed");
}
struct jar_reader { int fd; off_t cursor; unsigned tag, errors; };
static void *jar_read_worker(void *argument) {
    struct jar_reader *reader = argument;
    char bytes[64];
    for (unsigned i = 0; i < 150; ++i) {
        size_t offset = (i * 9973u + reader->tag * NXIO_JAR_READAHEAD_BYTES) % (3 * NXIO_JAR_READAHEAD_BYTES - sizeof(bytes));
        for (int repeat = 0; repeat < 2; ++repeat) {
            if (nxio_pread(reader->fd, bytes, sizeof(bytes), (off_t)offset) != (ssize_t)sizeof(bytes) ||
                !fixture_matches(bytes, sizeof(bytes), offset, 71) || nxio_seek(reader->fd, 0, SEEK_CUR) != reader->cursor)
                ++reader->errors;
        }
    }
    return NULL;
}
static void concurrent_jar_reads(void) {
    const char *path = "sdmc:/owned/concurrent.jar";
    seed_pattern(path, 3 * NXIO_JAR_READAHEAD_BYTES, 71);
    struct jar_reader readers[2] = {{nxio_open(path, O_RDONLY, 0), 73, 0, 0},
                                    {nxio_open(path, O_RDONLY, 0), 29, 1, 0}};
    check(readers[0].fd >= 0 && readers[1].fd >= 0 && nx_test_native_handles() == 1,
          "concurrent JAR readers use distinct logical descriptors over one real handle");
    check(nxio_seek(readers[0].fd, readers[0].cursor, SEEK_SET) == readers[0].cursor &&
          nxio_seek(readers[1].fd, readers[1].cursor, SEEK_SET) == readers[1].cursor, "concurrent JAR readers start with distinct cursors");
    struct nxio_shared_io_stats before, after;
    nxio_shared_io_snapshot(&before);
    pthread_t threads[2];
    check(pthread_create(&threads[0], NULL, jar_read_worker, &readers[0]) == 0 &&
          pthread_create(&threads[1], NULL, jar_read_worker, &readers[1]) == 0, "two JAR positioned-read workers start");
    check(pthread_join(threads[0], NULL) == 0 && pthread_join(threads[1], NULL) == 0, "two JAR read workers finish");
    check(readers[0].errors == 0 && readers[1].errors == 0,
          "600 concurrent positioned reads never mix cache ranges or change independent cursors");
    nxio_shared_io_snapshot(&after);
    check(after.logical_reads == before.logical_reads + 600 &&
          after.cache_bytes == before.cache_bytes + NXIO_JAR_READAHEAD_BYTES,
          "concurrent readers retain one bounded shared page cache and count all logical reads");
    check(nxio_close(readers[0].fd) == 0 && nxio_close(readers[1].fd) == 0 && unlink(path) == 0,
          "concurrent JAR descriptors and fixture close cleanly");
}
static void jar_cache_budget(void) {
    enum { windows = NXIO_JAR_CACHE_BUDGET_BYTES / NXIO_JAR_READAHEAD_BYTES };
    int fds[windows + 1]; char path[128], byte;
    struct nxio_shared_io_stats baseline, full, now;
    struct nx_test_io_stats before, after;
    nxio_shared_io_snapshot(&baseline);
    check(baseline.cache_bytes == 0, "JAR quota test starts without surviving cache allocations");
    for (unsigned i = 0; i <= windows; ++i) {
        snprintf(path, sizeof(path), "sdmc:/owned/budget-%03u.jar", i);
        seed(path, "ordinary fixture");
        fds[i] = nxio_open(path, O_RDONLY, 0);
        check(fds[i] >= 0, "independent budget fixture opens");
        nxio_shared_io_snapshot(&now);
        check(now.cache_bytes == (uint64_t)(i < windows ? i : windows) * NXIO_JAR_READAHEAD_BYTES,
              "unread JAR backing consumes no cache quota");
        check(nxio_read(fds[i], &byte, 1) == 1 && byte == 'o', "budget fixture returns genuine first byte");
        nxio_shared_io_snapshot(&now);
        check(now.cache_bytes == (uint64_t)(i < windows ? i + 1 : windows) * NXIO_JAR_READAHEAD_BYTES &&
              now.cache_peak_bytes <= NXIO_JAR_CACHE_BUDGET_BYTES, "actual JAR allocations never exceed active global quota");
    }
    nxio_shared_io_snapshot(&full); nx_test_io_snapshot(&before);
    check(full.cache_bytes == NXIO_JAR_CACHE_BUDGET_BYTES, "separate JAR windows fill exactly the global quota");
    check(nxio_pread(fds[windows], &byte, 1, 0) == 1 && byte == 'o' &&
          nxio_pread(fds[windows], &byte, 1, 0) == 1 && byte == 'o', "budget-exhausted JAR reads fall back to actual file IO");
    nx_test_io_snapshot(&after);
    check(after.reads == before.reads + 2 && after.seeks == before.seeks + 2 && after.stats == before.stats,
          "quota fallback issues real reads while genuine reader exclusion still permits stable size reuse");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fds[0], &byte, 1, 0) == 1 && byte == 'o', "quota exhaustion does not evict an existing JAR window");
    nx_test_io_snapshot(&after);
    check(same_backend_io(&before, &after), "existing JAR cache remains usable at full quota");
    check(nxio_close(fds[0]) == 0, "one cached native lifetime releases its quota"); fds[0] = -1;
    nxio_shared_io_snapshot(&now);
    check(now.cache_bytes == NXIO_JAR_CACHE_BUDGET_BYTES - NXIO_JAR_READAHEAD_BYTES, "last-close releases exactly one cache allocation");
    check(nxio_pread(fds[windows], &byte, 1, 0) == 1 && byte == 'o', "previous fallback JAR can allocate newly released quota");
    nx_test_io_snapshot(&before);
    check(nxio_pread(fds[windows], &byte, 1, 0) == 1 && byte == 'o', "newly allocated fallback window returns cached genuine data");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&now);
    check(same_backend_io(&before, &after) && now.cache_bytes == NXIO_JAR_CACHE_BUDGET_BYTES,
          "released quota is reusable and newly allocated window avoids backend IO");
    for (unsigned i = 0; i <= windows; ++i) {
        if (fds[i] >= 0) check(nxio_close(fds[i]) == 0, "budget fixture descriptor closes");
        snprintf(path, sizeof(path), "sdmc:/owned/budget-%03u.jar", i);
        check(unlink(path) == 0, "budget fixture file removed");
    }
    nxio_shared_io_snapshot(&now);
    check(now.cache_bytes == 0 && now.cache_peak_bytes == NXIO_JAR_CACHE_BUDGET_BYTES,
          "all cache memory is released and peak telemetry records the actual bounded allocation");
}
static void jar_whole_cache(void) {
    nxio_jar_whole_configure(1);
    const char *path = "sdmc:/owned/Whole.jar";
    const size_t length = 2 * NXIO_JAR_WHOLE_CHUNK_BYTES + 12345;
    seed_pattern(path, length, 71);
    struct nx_test_io_stats before, after;
    struct nxio_shared_io_stats base, now;
    nxio_shared_io_snapshot(&base);
    int a = nxio_open(path, O_RDONLY, 0), b = nxio_open(path, O_RDONLY, 0);
    check(a >= 0 && b >= 0, "whole-cache JAR readers open");
    char bytes[97];
    nx_test_io_snapshot(&before);
    check(nxio_pread(a, bytes, 97, 5000) == 97 && fixture_matches(bytes, 97, 5000, 71), "first JAR read returns genuine bytes");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&now);
    check(after.reads == before.reads + 3 && after.read_bytes == before.read_bytes + length,
          "first read loads the whole JAR with three large sequential reads");
    check(now.whole_files == base.whole_files + 1 && now.whole_bytes == base.whole_bytes + length, "whole-cache telemetry records the resident copy");
    nx_test_io_snapshot(&before);
    const off_t offsets[] = {0, 1, (off_t)NXIO_JAR_WHOLE_CHUNK_BYTES - 50, (off_t)NXIO_JAR_WHOLE_CHUNK_BYTES * 2, (off_t)length - 97, 777777};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
        check(nxio_pread(b, bytes, 97, offsets[i]) == 97 && fixture_matches(bytes, 97, (size_t)offsets[i], 71),
              "resident JAR serves any offset, across chunk boundaries, for every descriptor");
    check(nxio_pread(a, bytes, 97, (off_t)length - 10) == 10 && fixture_matches(bytes, 10, length - 10, 71), "resident read is clamped at EOF");
    check(nxio_seek(a, 100, SEEK_SET) == 100 && nxio_read(a, bytes, 50) == 50 && fixture_matches(bytes, 50, 100, 71) &&
          nxio_read(a, bytes, 50) == 50 && fixture_matches(bytes, 50, 150, 71), "sequential resident reads advance the logical position");
    nx_test_io_snapshot(&after);
    check(same_backend_io(&before, &after), "resident JAR reads make no native IO");
    check(nxio_close(b) == 0, "one reader closes");
    nxio_shared_io_snapshot(&now);
    check(now.whole_bytes == base.whole_bytes + length, "copy stays while another descriptor uses the JAR");
    check(nxio_close(a) == 0, "last reader closes");
    nxio_shared_io_snapshot(&now);
    check(now.whole_bytes == base.whole_bytes, "closing the last descriptor frees the resident copy");
    /* A write upgrade invalidates the copy; memory pressure trims and disables. */
    a = nxio_open(path, O_RDONLY, 0);
    check(a >= 0 && nxio_pread(a, bytes, 10, 20) == 10 && fixture_matches(bytes, 10, 20, 71), "copy reloads for a new reader");
    int w = nxio_open(path, O_RDWR, 0);
    nxio_shared_io_snapshot(&now);
    check(w >= 0 && now.whole_bytes == base.whole_bytes, "writable open drops the resident copy");
    check(nxio_close(w) == 0, "writer closes");
    check(nxio_pread(a, bytes, 10, 30) == 10 && fixture_matches(bytes, 10, 30, 71), "reader still reads genuine bytes after upgrade");
    nxio_shared_io_snapshot(&now);
    check(now.whole_bytes == base.whole_bytes, "a backing that was opened for writing is not cached again");
    check(nxio_close(a) == 0, "upgraded reader closes");
    a = nxio_open(path, O_RDONLY, 0);
    check(a >= 0 && nxio_pread(a, bytes, 10, 40) == 10 && fixture_matches(bytes, 10, 40, 71), "fresh read-only backing caches again");
    size_t released = nxio_jar_cache_trim();
    nxio_shared_io_snapshot(&now);
    check(released == length && now.whole_bytes == base.whole_bytes && now.whole_trims == base.whole_trims + 1,
          "memory-pressure trim releases every resident copy");
    nx_test_io_snapshot(&before);
    check(nxio_pread(a, bytes, 97, 3000000) == 97 && fixture_matches(bytes, 97, 3000000, 71), "after trim reads fall back to genuine native data");
    nx_test_io_snapshot(&after); nxio_shared_io_snapshot(&now);
    check(after.read_bytes == before.read_bytes + NXIO_JAR_CACHE_PAGE_BYTES && now.whole_bytes == base.whole_bytes,
          "after trim the JAR stays on the bounded page cache");
    check(nxio_close(a) == 0 && unlink(path) == 0, "whole-cache fixture removed");
}
int main(void) {
    nxio_jar_whole_configure(0);
    check(mkdir("sdmc:", 0700) == 0 && mkdir("sdmc:/owned", 0700) == 0, "owned local fixture directories");
    nested_channels(); config_read_then_write(); positioned_channels(); concurrent_appends(); upgrade_failures(); libc_close_lifetime();
    jar_read_pages(); jar_page_lru(); jar_direct_and_writable(); jar_native_failures(); jar_page_short_reads();
    jar_upgrade_invalidation(); concurrent_jar_reads(); jar_cache_budget(); jar_whole_cache();
    check(nx_test_native_handles() == 0 && nx_test_sdk_handles() == 0, "no native or logical descriptor leaks");
    const char *files[] = {"config-lock.tmp", "config.properties", "positions.bin", "append.txt", "upgrade.txt", "lifetime.txt", "other.txt"};
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        char path[128]; snprintf(path, sizeof(path), "sdmc:/owned/%s", files[i]);
        check(unlink(path) == 0, "owned fixture file removed");
    }
    check(rmdir("sdmc:/owned") == 0 && rmdir("sdmc:") == 0, "fixture directories removed");
    printf("FILE_SHARING_HOST_PASS checks=%u\n", checks);
    return 0;
}
