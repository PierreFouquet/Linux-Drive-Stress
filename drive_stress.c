/*
 * drive_stress.c - engine for the Linux drive stress tests.
 *
 * Design notes
 * ------------
 * Data generation must be much faster than the drive or the test measures the
 * CPU instead. Block contents come from xorshift64 seeded per block via
 * splitmix64, which fills memory at several GB/s. Because the seed is derived
 * from (seed, offset), the expected contents of any block can be recomputed
 * on demand - so random-offset I/O is fully verifiable.
 *
 * Reads must reach the platter/flash rather than the page cache. O_DIRECT is
 * used when the filesystem supports it; otherwise the engine falls back to
 * fsync + POSIX_FADV_DONTNEED, which evicts the clean pages it just wrote.
 *
 * Queue depth comes from running num_files worker threads, each issuing
 * synchronous pread/pwrite against its own file: the device sees roughly
 * num_files outstanding requests.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "drive_stress.h"

/* ------------------------------------------------------------------ */
/* Interruption                                                        */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void install_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* Clock                                                               */
/* ------------------------------------------------------------------ */

static double now_sec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* Deterministic, offset-addressable data generation                   */
/* ------------------------------------------------------------------ */

static inline uint64_t splitmix64(uint64_t x)
{
    x += UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    return x ^ (x >> 31);
}

#define DS_XORSHIFT(v) do {  \
        (v) ^= (v) << 13;    \
        (v) ^= (v) >> 7;     \
        (v) ^= (v) << 17;    \
    } while (0)

static inline uint64_t xorshift64(uint64_t *s)
{
    uint64_t x = *s;
    DS_XORSHIFT(x);
    *s = x;
    return x;
}

/*
 * Number of independent generator lanes. A single xorshift chain is one long
 * dependency chain, so the CPU sits idle waiting on it; running several
 * independent lanes lets it overlap them. Eight lanes emit exactly one 64-byte
 * cache line per inner iteration and measured 3.3x a single chain (3.6 -> 11.7
 * GiB/s). Sixteen and thirty-two regress on register pressure and on the
 * per-granule seeding cost, so eight is the sweet spot.
 */
#define DS_LANES 8

/*
 * Fill one granule of 'len' bytes with the contents defined by (seed, offset).
 * This has to outrun the drive by a wide margin or the test measures the CPU;
 * see fill_granule's lane count above.
 */
static void fill_granule(void *dst, size_t len, uint64_t seed, long long offset)
{
    uint64_t v[DS_LANES];
    uint64_t s = seed ^ ((uint64_t)offset * UINT64_C(0x9E3779B97F4A7C15));
    uint64_t *p = (uint64_t *)dst;
    size_t words = len / 8;
    size_t i = 0;
    size_t k;

    for (k = 0; k < DS_LANES; k++) {
        s = splitmix64(s);
        v[k] = s != 0 ? s : UINT64_C(0x123456789ABCDEF);
    }
    /* Constant trip count: the compiler unrolls this and keeps v[] in
     * registers, which is what makes the lanes independent. */
    for (; i + DS_LANES <= words; i += DS_LANES) {
        for (k = 0; k < DS_LANES; k++) {
            DS_XORSHIFT(v[k]);
            p[i + k] = v[k];
        }
    }
    /* Unreachable for the block sizes we accept (all multiples of 512), but
     * keeps the function total. */
    for (k = 0; i < words; i++, k++) {
        DS_XORSHIFT(v[k % DS_LANES]);
        p[i] = v[k % DS_LANES];
    }
    if ((len & 7u) != 0) {
        uint64_t tail = v[0];
        DS_XORSHIFT(tail);
        memcpy((char *)dst + words * 8, &tail, len & 7u);
    }
}

/* ------------------------------------------------------------------ */
/* Latency histogram (log-linear, 8 sub-buckets per octave)            */
/* ------------------------------------------------------------------ */

static inline int hist_bucket(uint64_t v)
{
    int msb, sub, b;

    if (v < 8) {
        return (int)v;
    }
    msb = 63 - __builtin_clzll(v);
    sub = (int)((v >> (msb - 3)) & 7u);
    b = (msb - 2) * 8 + sub;
    return b < DS_HIST_BUCKETS ? b : DS_HIST_BUCKETS - 1;
}

static inline uint64_t hist_value(int b)
{
    int msb, sub;

    if (b < 8) {
        return (uint64_t)b;
    }
    msb = b / 8 + 2;
    sub = b % 8;
    return ((uint64_t)8 + (uint64_t)sub) << (msb - 3);
}

static uint64_t hist_percentile(const uint64_t *h, uint64_t total, double pct)
{
    uint64_t target, acc = 0;
    int i;

    if (total == 0) {
        return 0;
    }
    target = (uint64_t)((double)total * pct / 100.0);
    if (target == 0) {
        target = 1;
    }
    for (i = 0; i < DS_HIST_BUCKETS; i++) {
        acc += h[i];
        if (acc >= target) {
            return hist_value(i);
        }
    }
    return hist_value(DS_HIST_BUCKETS - 1);
}

/* ------------------------------------------------------------------ */
/* Full-descriptor I/O helpers                                         */
/* ------------------------------------------------------------------ */

static int pwrite_all(int fd, const void *buf, size_t len, long long off)
{
    const char *p = (const char *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = pwrite(fd, p + done, len - done, (off_t)(off + (long long)done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = ENOSPC;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int pread_all(int fd, void *buf, size_t len, long long off)
{
    char *p = (char *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = pread(fd, p + done, len - done, (off_t)(off + (long long)done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EIO; /* short read: file is smaller than we wrote */
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Workers                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    long long seq_write_bytes;  /* phase 1: sequential write */
    long long seq_read_bytes;   /* phase 3: sequential read */
    long long reverify_bytes;   /* phase 5: post-random re-read */
    long long rand_read_ops;    /* phase 4 */
    long long rand_write_ops;   /* phase 4 */
    long long verified_bytes;
    long long mismatches;
    long long io_errors;
    uint64_t  hist[DS_HIST_BUCKETS];
} WorkerStats;

typedef struct {
    const DsConfig *cfg;
    int        index;
    char       path[4096];
    int        fd;
    int        direct;        /* O_DIRECT actually in effect */
    void      *buf;           /* seq_block, aligned */
    void      *scratch;       /* rand_block, aligned - one granule of expected data */
    uint8_t   *gen;           /* 1 bit per granule: 0 = seed_a, 1 = seed_b */
    long long  granules;      /* file_size / rand_block */
    long long  iter;          /* iteration number, set by main before the gate */
    uint64_t   base_seed;
    uint64_t   seed_a;        /* re-derived each iteration */
    uint64_t   seed_b;
    uint64_t   rng;           /* private PRNG for offset/op selection */
    WorkerStats st;
} Worker;

static pthread_barrier_t g_barrier;
static int g_direct_warned = 0;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Startup gate. Workers are created before the barrier is armed, so they park
 * here until the main thread knows how many threads it actually got. 1 means
 * go, -1 means setup failed and they should exit without touching the barrier.
 */
static pthread_mutex_t g_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_gate = 0;

/* Set by main, read by workers immediately after the iteration gate. */
static int g_finished = 0;

static inline int gen_get(const Worker *w, long long g)
{
    return (w->gen[g >> 3] >> (g & 7)) & 1;
}

static inline void gen_set(Worker *w, long long g, int bit)
{
    if (bit) {
        w->gen[g >> 3] |= (uint8_t)(1u << (g & 7));
    } else {
        w->gen[g >> 3] &= (uint8_t)~(1u << (g & 7));
    }
}

/* A badly failing drive can produce millions of mismatches; print the first
 * few in full and let the summary carry the count. */
#define DS_MAX_REPORTS 20

static long long g_reported = 0;

/* Report a mismatch, naming the first differing byte. */
static void report_mismatch(const Worker *w, const void *got, const void *want,
                            size_t len, long long off)
{
    const unsigned char *g = (const unsigned char *)got;
    const unsigned char *e = (const unsigned char *)want;
    size_t i;

    for (i = 0; i < len && g[i] == e[i]; i++) {
        /* locate first difference */
    }
    pthread_mutex_lock(&g_log_lock);
    if (g_reported < DS_MAX_REPORTS) {
        fprintf(stderr, "  DATA CORRUPTION: %s offset %lld: byte %zu is 0x%02x,"
                " expected 0x%02x\n", w->path, off, i,
                i < len ? g[i] : 0u, i < len ? e[i] : 0u);
    } else if (g_reported == DS_MAX_REPORTS) {
        fprintf(stderr, "  DATA CORRUPTION: further reports suppressed;"
                " see the mismatch count in the summary\n");
    }
    g_reported++;
    pthread_mutex_unlock(&g_log_lock);
}

/*
 * Compare [off, off+len) of 'data' against the canonical contents, honouring
 * the per-granule seed bitmap, and return the number of bad granules.
 *
 * Regenerating one granule at a time into a scratch buffer that stays resident
 * in L1 measured 22% faster than materialising a second copy of the whole
 * block (9970 vs 8193 MiB/s verified), and it shrinks the per-worker buffer
 * from seq_block to rand_block - 4 KiB instead of 1 MiB by default - so the
 * expected data never evicts the data being checked.
 */
static long long verify_range(Worker *w, const void *data, size_t len,
                              long long off)
{
    size_t gran = w->cfg->rand_block;
    long long bad = 0;
    /* Walk the granule index instead of dividing by a runtime value on every
     * granule; 'off' is always granule-aligned. */
    long long gidx = off / (long long)gran;
    size_t done;

    for (done = 0; done < len; done += gran, gidx++) {
        long long o = off + (long long)done;
        uint64_t seed = gen_get(w, gidx) ? w->seed_b : w->seed_a;
        const char *got = (const char *)data + done;

        fill_granule(w->scratch, gran, seed, o);
        if (memcmp(got, w->scratch, gran) != 0) {
            report_mismatch(w, got, w->scratch, gran, o);
            bad++;
        }
    }
    w->st.verified_bytes += (long long)len;
    return bad;
}

/* Fill [off, off+len) with granules generated from a single seed. */
static void fill_uniform(const Worker *w, void *dst, size_t len, long long off,
                         uint64_t seed)
{
    size_t gran = w->cfg->rand_block;
    size_t done;

    for (done = 0; done < len; done += gran) {
        fill_granule((char *)dst + done, gran, seed, off + (long long)done);
    }
}

static void log_errno(Worker *w, const char *what, long long off)
{
    pthread_mutex_lock(&g_log_lock);
    fprintf(stderr, "  I/O ERROR: %s on %s at offset %lld: %s\n",
            what, w->path, off, strerror(errno));
    pthread_mutex_unlock(&g_log_lock);
    w->st.io_errors++;
}

/*
 * Open the worker's file, preferring O_DIRECT. Filesystems such as tmpfs and
 * some overlayfs configurations reject O_DIRECT with EINVAL; fall back to
 * buffered I/O and warn once, since the fallback changes what is measured.
 */
static int worker_open(Worker *w)
{
    int flags = O_RDWR | O_CREAT;

    w->direct = 0;
    if (w->cfg->direct_io) {
        w->fd = open(w->path, flags | O_DIRECT, 0644);
        if (w->fd >= 0) {
            w->direct = 1;
            return 0;
        }
        if (errno != EINVAL && errno != ENOTSUP && errno != EOPNOTSUPP) {
            log_errno(w, "open(O_DIRECT)", 0);
            return -1;
        }
        pthread_mutex_lock(&g_log_lock);
        if (!g_direct_warned) {
            g_direct_warned = 1;
            fprintf(stderr,
                    "WARNING: O_DIRECT unsupported here (%s); falling back to\n"
                    "         buffered I/O with POSIX_FADV_DONTNEED between phases.\n"
                    "         Reads still reach the device, but results include\n"
                    "         page-cache overhead. Use --no-direct to silence this.\n",
                    strerror(errno));
        }
        pthread_mutex_unlock(&g_log_lock);
    }
    w->fd = open(w->path, flags, 0644);
    if (w->fd < 0) {
        log_errno(w, "open", 0);
        return -1;
    }
    return 0;
}

/*
 * Push everything to the device and make sure the next read cannot be served
 * from RAM. With O_DIRECT the data never entered the page cache, so only the
 * metadata flush is needed.
 */
static void drop_cache(Worker *w)
{
    if (fsync(w->fd) < 0) {
        log_errno(w, "fsync", 0);
    }
    if (!w->direct) {
        if (posix_fadvise(w->fd, 0, 0, POSIX_FADV_DONTNEED) != 0) {
            pthread_mutex_lock(&g_log_lock);
            fprintf(stderr, "  WARNING: posix_fadvise(DONTNEED) failed on %s;"
                            " reads may hit the page cache\n", w->path);
            pthread_mutex_unlock(&g_log_lock);
        }
    }
}

/* Phase 1: sequential write of the whole file. */
static void phase_seq_write(Worker *w)
{
    long long off;

    memset(w->gen, 0, (size_t)((w->granules + 7) / 8));
    for (off = 0; off < w->cfg->file_size && !g_stop;
         off += (long long)w->cfg->seq_block) {
        fill_uniform(w, w->buf, w->cfg->seq_block, off, w->seed_a);
        if (pwrite_all(w->fd, w->buf, w->cfg->seq_block, off) < 0) {
            log_errno(w, "sequential write", off);
            return;
        }
        w->st.seq_write_bytes += (long long)w->cfg->seq_block;
    }
}

/*
 * Phase 3 and 5: sequential read of the whole file, verifying as it goes.
 * 'counter' selects which phase the bytes are attributed to.
 */
static void phase_seq_read(Worker *w, long long *counter)
{
    long long off;

    for (off = 0; off < w->cfg->file_size && !g_stop;
         off += (long long)w->cfg->seq_block) {
        if (pread_all(w->fd, w->buf, w->cfg->seq_block, off) < 0) {
            log_errno(w, "sequential read", off);
            return;
        }
        *counter += (long long)w->cfg->seq_block;
        if (w->cfg->verify) {
            w->st.mismatches += verify_range(w, w->buf, w->cfg->seq_block, off);
        }
    }
}

/*
 * Phase 4: timed random read/write mix at rand_block granularity. This is the
 * phase that exercises the drive's queue, FTL and garbage collection; the
 * sequential phases mostly measure raw bandwidth.
 */
static void phase_random(Worker *w)
{
    size_t gran = w->cfg->rand_block;
    uint64_t deadline;

    if (w->cfg->mixed_sec <= 0) {
        return;
    }
    deadline = now_ns() + (uint64_t)w->cfg->mixed_sec * UINT64_C(1000000000);

    while (!g_stop) {
        long long g = (long long)(xorshift64(&w->rng) % (uint64_t)w->granules);
        long long off = g * (long long)gran;
        int is_read = (int)(xorshift64(&w->rng) % 100u) < w->cfg->read_pct;
        uint64_t t0, t1;

        if (is_read) {
            t0 = now_ns();
            if (pread_all(w->fd, w->buf, gran, off) < 0) {
                log_errno(w, "random read", off);
                break;
            }
            t1 = now_ns();
            w->st.rand_read_ops++;
            if (w->cfg->verify) {
                w->st.mismatches += verify_range(w, w->buf, gran, off);
            }
        } else {
            fill_granule(w->buf, gran, w->seed_b, off);
            t0 = now_ns();
            if (pwrite_all(w->fd, w->buf, gran, off) < 0) {
                log_errno(w, "random write", off);
                break;
            }
            t1 = now_ns();
            gen_set(w, g, 1);
            w->st.rand_write_ops++;
        }
        w->st.hist[hist_bucket(t1 - t0)]++;

        /* Each op is already timed, so the deadline costs no extra clock read. */
        if (t1 >= deadline) {
            break;
        }
    }
    drop_cache(w); /* the re-verify pass must not read what we just cached */
}

/*
 * Workers are created once and live for the whole run, stepping through
 * iterations in lockstep with the main thread. Creating them once means a
 * pthread_create failure can only happen before the barrier is in use: there
 * is never a thread parked on a barrier that will not be reached. (This
 * matters because pthread_barrier_wait is not a cancellation point, so a
 * stranded thread could not be cancelled out of it.)
 */
static void *worker_main(void *arg)
{
    Worker *w = (Worker *)arg;
    int go;

    pthread_mutex_lock(&g_gate_lock);
    while (g_gate == 0) {
        pthread_cond_wait(&g_gate_cv, &g_gate_lock);
    }
    go = g_gate;
    pthread_mutex_unlock(&g_gate_lock);
    if (go < 0) {
        return NULL;
    }

    for (;;) {
        pthread_barrier_wait(&g_barrier);   /* iteration gate */
        if (g_finished) {
            break;
        }

        /* Fresh data every iteration so the drive never sees a replay. */
        memset(&w->st, 0, sizeof w->st);
        w->seed_a = splitmix64(w->base_seed
                               ^ ((uint64_t)w->index * UINT64_C(0x1000193))
                               ^ ((uint64_t)w->iter * UINT64_C(0x9E3779B97F4A7C15)));
        w->seed_b = splitmix64(w->seed_a);
        phase_seq_write(w);

        pthread_barrier_wait(&g_barrier);
        drop_cache(w);

        pthread_barrier_wait(&g_barrier);
        phase_seq_read(w, &w->st.seq_read_bytes);

        pthread_barrier_wait(&g_barrier);
        phase_random(w);

        pthread_barrier_wait(&g_barrier);
        if (w->cfg->verify && w->cfg->mixed_sec > 0) {
            phase_seq_read(w, &w->st.reverify_bytes);
        }

        pthread_barrier_wait(&g_barrier);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Setup and teardown                                                  */
/* ------------------------------------------------------------------ */

static void worker_path(const DsConfig *cfg, int i, char *out, size_t cap)
{
    if (cfg->multi_mode) {
        snprintf(out, cap, "%s%d.dat", cfg->path, i);
    } else {
        snprintf(out, cap, "%s", cfg->path);
    }
}

/* Refuse to scribble on a block device or an existing directory. */
static int check_target(const char *path)
{
    struct stat sb;

    if (stat(path, &sb) != 0) {
        return 0; /* does not exist yet: fine, we will create it */
    }
    if (S_ISBLK(sb.st_mode) || S_ISCHR(sb.st_mode)) {
        fprintf(stderr,
                "REFUSING to write to device node %s.\n"
                "This tool writes regular files. To test a specific drive, mount it\n"
                "and point the path at a file on that mount.\n", path);
        return -1;
    }
    if (S_ISDIR(sb.st_mode)) {
        fprintf(stderr, "ERROR: %s is a directory, not a file.\n", path);
        return -1;
    }
    return 0;
}

static int check_space(const char *path, long long needed)
{
    struct statvfs vfs;
    char dir[4096];
    const char *slash;
    long long avail;

    slash = strrchr(path, '/');
    if (slash == NULL) {
        snprintf(dir, sizeof dir, ".");
    } else {
        size_t n = (size_t)(slash - path);
        if (n == 0) {
            n = 1; /* path is "/something" */
        }
        if (n >= sizeof dir) {
            n = sizeof dir - 1;
        }
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
    if (statvfs(dir, &vfs) != 0) {
        return 0; /* cannot tell; let the write fail naturally */
    }
    avail = (long long)vfs.f_bavail * (long long)vfs.f_frsize;
    if (avail < needed) {
        fprintf(stderr,
                "ERROR: need %.1f MiB in %s but only %.1f MiB is available.\n",
                (double)needed / 1048576.0, dir, (double)avail / 1048576.0);
        return -1;
    }
    return 0;
}

static int worker_init(Worker *w, const DsConfig *cfg, int i, uint64_t base_seed)
{
    memset(w, 0, sizeof *w);
    w->cfg = cfg;
    w->index = i;
    w->fd = -1;
    w->granules = cfg->file_size / (long long)cfg->rand_block;
    w->base_seed = base_seed;
    w->rng = splitmix64(base_seed ^ ((uint64_t)i * UINT64_C(0x9E3779B9)) ^ UINT64_C(0xDEADBEEF));
    if (w->rng == 0) {
        w->rng = 1;
    }
    worker_path(cfg, i, w->path, sizeof w->path);

    if (posix_memalign(&w->buf, DS_ALIGN, cfg->seq_block) != 0 ||
        posix_memalign(&w->scratch, DS_ALIGN, cfg->rand_block) != 0) {
        fprintf(stderr, "ERROR: cannot allocate aligned I/O buffers\n");
        return -1;
    }
    w->gen = (uint8_t *)calloc(1, (size_t)((w->granules + 7) / 8));
    if (w->gen == NULL) {
        fprintf(stderr, "ERROR: cannot allocate verification bitmap\n");
        return -1;
    }
    return 0;
}

static void worker_free(Worker *w)
{
    if (w->fd >= 0) {
        close(w->fd);
        w->fd = -1;
    }
    free(w->buf);
    free(w->scratch);
    free(w->gen);
    w->buf = w->scratch = NULL;
    w->gen = NULL;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static void print_rate(const char *label, long long bytes, double sec)
{
    if (bytes <= 0) {
        return;
    }
    printf("  %-12s %9.1f MiB in %6.2f s  %8.1f MiB/s\n", label,
           (double)bytes / 1048576.0, sec,
           sec > 0 ? (double)bytes / 1048576.0 / sec : 0.0);
}

static void print_random(const DsConfig *cfg, const WorkerStats *tot, double sec)
{
    long long ops = tot->rand_read_ops + tot->rand_write_ops;
    uint64_t total = 0;
    int i;
    char label[32];

    if (ops <= 0 || sec <= 0) {
        return;
    }
    for (i = 0; i < DS_HIST_BUCKETS; i++) {
        total += tot->hist[i];
    }
    snprintf(label, sizeof label, "rand %zuKiB", cfg->rand_block / 1024);
    printf("  %-12s %9.0f IOPS  %8.1f MiB/s  (%lld rd / %lld wr, %d%% read target)\n",
           label, (double)ops / sec,
           (double)(ops * (long long)cfg->rand_block) / 1048576.0 / sec,
           tot->rand_read_ops, tot->rand_write_ops, cfg->read_pct);
    printf("  %-12s p50 %.3f  p95 %.3f  p99 %.3f  p99.9 %.3f  max %.3f  (ms)\n", "",
           (double)hist_percentile(tot->hist, total, 50.0) / 1e6,
           (double)hist_percentile(tot->hist, total, 95.0) / 1e6,
           (double)hist_percentile(tot->hist, total, 99.0) / 1e6,
           (double)hist_percentile(tot->hist, total, 99.9) / 1e6,
           (double)hist_percentile(tot->hist, total, 100.0) / 1e6);
}

static void accumulate(WorkerStats *dst, const WorkerStats *src)
{
    int i;

    dst->seq_write_bytes += src->seq_write_bytes;
    dst->seq_read_bytes += src->seq_read_bytes;
    dst->reverify_bytes += src->reverify_bytes;
    dst->rand_read_ops += src->rand_read_ops;
    dst->rand_write_ops += src->rand_write_ops;
    dst->verified_bytes += src->verified_bytes;
    dst->mismatches += src->mismatches;
    dst->io_errors += src->io_errors;
    for (i = 0; i < DS_HIST_BUCKETS; i++) {
        dst->hist[i] += src->hist[i];
    }
}

/* ------------------------------------------------------------------ */
/* Driver                                                             */
/* ------------------------------------------------------------------ */

int ds_run(DsConfig *cfg)
{
    Worker *workers;
    pthread_t *threads;
    WorkerStats grand;
    uint64_t base_seed;
    double run_start;
    long long iter = 0;
    int i, rc = 0, barrier_up = 0, spawned = 0;

    /* Round the file size down so every block size divides it evenly. */
    cfg->file_size -= cfg->file_size % (long long)cfg->seq_block;
    if (cfg->file_size < (long long)cfg->seq_block) {
        cfg->file_size = (long long)cfg->seq_block;
    }

    base_seed = cfg->seed;
    if (base_seed == 0) {
        base_seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
    }

    memset(&grand, 0, sizeof grand);
    workers = (Worker *)calloc((size_t)cfg->num_files, sizeof *workers);
    threads = (pthread_t *)calloc((size_t)cfg->num_files, sizeof *threads);
    if (workers == NULL || threads == NULL) {
        fprintf(stderr, "ERROR: out of memory for %d workers\n", cfg->num_files);
        free(workers);
        free(threads);
        return 1;
    }

    for (i = 0; i < cfg->num_files; i++) {
        char path[4096];
        worker_path(cfg, i, path, sizeof path);
        if (check_target(path) != 0) {
            rc = 1;
            goto cleanup;
        }
    }
    {
        char path[4096];
        worker_path(cfg, 0, path, sizeof path);
        if (check_space(path, cfg->file_size * cfg->num_files) != 0) {
            rc = 1;
            goto cleanup;
        }
    }

    for (i = 0; i < cfg->num_files; i++) {
        if (worker_init(&workers[i], cfg, i, base_seed) != 0) {
            rc = 1;
            goto cleanup;
        }
    }

    install_handlers();

    printf("Linux drive stress test\n");
    printf("  target        %s%s\n", cfg->path,
           cfg->multi_mode ? "<n>.dat" : "");
    printf("  files/threads %d  (effective queue depth ~%d)\n",
           cfg->num_files, cfg->num_files);
    printf("  size per file %.1f MiB  (%.1f MiB total)\n",
           (double)cfg->file_size / 1048576.0,
           (double)cfg->file_size * cfg->num_files / 1048576.0);
    printf("  sequential    %zu KiB blocks\n", cfg->seq_block / 1024);
    printf("  random        %zu KiB blocks, %d%% reads, %d s per iteration\n",
           cfg->rand_block / 1024, cfg->read_pct, cfg->mixed_sec);
    printf("  O_DIRECT      %s\n", cfg->direct_io ? "requested" : "disabled");
    printf("  verify        %s\n", cfg->verify ? "on" : "off");
    printf("  base seed     0x%016" PRIx64 "\n", base_seed);
    printf("Press Ctrl+C to stop.\n\n");
    fflush(stdout);

    for (i = 0; i < cfg->num_files; i++) {
        if (worker_open(&workers[i]) != 0) {
            rc = 1;
            goto cleanup;
        }
    }

    /* Create every worker first, then arm a barrier sized to what we got. */
    for (i = 0; i < cfg->num_files; i++) {
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i]) != 0) {
            fprintf(stderr, "ERROR: cannot create worker thread %d: %s\n",
                    i, strerror(errno));
            break;
        }
        spawned++;
    }
    if (spawned != cfg->num_files) {
        pthread_mutex_lock(&g_gate_lock);
        g_gate = -1;
        pthread_cond_broadcast(&g_gate_cv);
        pthread_mutex_unlock(&g_gate_lock);
        for (i = 0; i < spawned; i++) {
            pthread_join(threads[i], NULL);
        }
        rc = 1;
        goto cleanup;
    }

    if (pthread_barrier_init(&g_barrier, NULL,
                             (unsigned)cfg->num_files + 1u) != 0) {
        fprintf(stderr, "ERROR: cannot initialise barrier\n");
        pthread_mutex_lock(&g_gate_lock);
        g_gate = -1;
        pthread_cond_broadcast(&g_gate_cv);
        pthread_mutex_unlock(&g_gate_lock);
        for (i = 0; i < spawned; i++) {
            pthread_join(threads[i], NULL);
        }
        rc = 1;
        goto cleanup;
    }
    barrier_up = 1;

    pthread_mutex_lock(&g_gate_lock);
    g_gate = 1;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_lock);

    run_start = now_sec();

    for (;;) {
        WorkerStats it;
        double t_a, t_write, t_drop, t_read, t_rand, t_verify;
        int last = 0;

        if (g_stop) {
            last = 1;
        }
        if (cfg->iterations > 0 && iter >= cfg->iterations) {
            last = 1;
        }
        if (cfg->duration_sec > 0 &&
            now_sec() - run_start >= (double)cfg->duration_sec) {
            last = 1;
        }

        if (!last) {
            iter++;
            for (i = 0; i < cfg->num_files; i++) {
                workers[i].iter = iter;
            }
            if (cfg->recreate && iter > 1) {
                for (i = 0; i < cfg->num_files; i++) {
                    close(workers[i].fd);
                    workers[i].fd = -1;
                    remove(workers[i].path);
                    if (worker_open(&workers[i]) != 0) {
                        last = 1;
                        rc = 1;
                        break;
                    }
                }
            }
        }

        /* Workers read this the moment they clear the iteration gate. */
        g_finished = last;
        pthread_barrier_wait(&g_barrier);
        if (last) {
            break;
        }

        printf("Iteration %lld\n", iter);
        fflush(stdout);

        t_a = now_sec();
        pthread_barrier_wait(&g_barrier); t_write = now_sec();
        pthread_barrier_wait(&g_barrier); t_drop = now_sec();
        pthread_barrier_wait(&g_barrier); t_read = now_sec();
        pthread_barrier_wait(&g_barrier); t_rand = now_sec();
        pthread_barrier_wait(&g_barrier); t_verify = now_sec();

        memset(&it, 0, sizeof it);
        for (i = 0; i < cfg->num_files; i++) {
            accumulate(&it, &workers[i].st);
        }

        if (g_stop) {
            printf("  interrupted mid-iteration; throughput not reported\n");
        } else {
            print_rate("seq write", it.seq_write_bytes, t_write - t_a);
            print_rate("seq read", it.seq_read_bytes, t_read - t_drop);
            print_random(cfg, &it, t_rand - t_read);
            print_rate("re-verify", it.reverify_bytes, t_verify - t_rand);
        }
        if (cfg->verify) {
            printf("  %-12s %9.1f MiB checked, %lld mismatch%s\n", "verify",
                   (double)it.verified_bytes / 1048576.0, it.mismatches,
                   it.mismatches == 1 ? "" : "es");
        }
        if (it.io_errors > 0) {
            printf("  %-12s %lld\n", "io errors", it.io_errors);
        }
        printf("\n");
        fflush(stdout);

        accumulate(&grand, &it);
        if (it.mismatches > 0 || it.io_errors > 0) {
            rc = 2;
        }
    }

    for (i = 0; i < cfg->num_files; i++) {
        pthread_join(threads[i], NULL);
    }

    {
        double elapsed = now_sec() - run_start;
        long long total_write = grand.seq_write_bytes +
            grand.rand_write_ops * (long long)cfg->rand_block;
        long long total_read = grand.seq_read_bytes + grand.reverify_bytes +
            grand.rand_read_ops * (long long)cfg->rand_block;

        printf("=== summary after %lld iteration%s, %.1f s ===\n", iter,
               iter == 1 ? "" : "s", elapsed);
        printf("  written       %.1f MiB (%.1f MiB/s average)\n",
               (double)total_write / 1048576.0,
               elapsed > 0 ? (double)total_write / 1048576.0 / elapsed : 0.0);
        printf("  read          %.1f MiB (%.1f MiB/s average)\n",
               (double)total_read / 1048576.0,
               elapsed > 0 ? (double)total_read / 1048576.0 / elapsed : 0.0);
        printf("  random ops    %lld read / %lld write\n",
               grand.rand_read_ops, grand.rand_write_ops);
        printf("  verified      %.1f MiB\n",
               (double)grand.verified_bytes / 1048576.0);
        printf("  mismatches    %lld\n", grand.mismatches);
        printf("  io errors     %lld\n", grand.io_errors);
        if (grand.mismatches == 0 && grand.io_errors == 0) {
            printf("  result        PASS - no corruption or I/O errors detected\n");
        } else {
            printf("  result        FAIL - the drive returned bad data or errors\n");
        }
    }

cleanup:
    if (barrier_up) {
        pthread_barrier_destroy(&g_barrier);
    }
    for (i = 0; i < cfg->num_files; i++) {
        if (workers[i].path[0] != '\0') {
            remove(workers[i].path);
        }
        worker_free(&workers[i]);
    }
    free(workers);
    free(threads);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define DS_DEFAULT_SIZE_MB   10
#define DS_DEFAULT_SEQ_KB    1024
#define DS_DEFAULT_RAND_KB   4
#define DS_DEFAULT_READ_PCT  70
#define DS_DEFAULT_MIXED_SEC 10

void ds_config_defaults(DsConfig *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->file_size = (long long)DS_DEFAULT_SIZE_MB * 1024 * 1024;
    cfg->num_files = 1;
    cfg->seq_block = (size_t)DS_DEFAULT_SEQ_KB * 1024;
    cfg->rand_block = (size_t)DS_DEFAULT_RAND_KB * 1024;
    cfg->direct_io = 1;
    cfg->verify = 1;
    cfg->read_pct = DS_DEFAULT_READ_PCT;
    cfg->mixed_sec = DS_DEFAULT_MIXED_SEC;
    cfg->recreate = 0;
    cfg->iterations = 0;
    cfg->duration_sec = 0;
    cfg->seed = 0;
}

void ds_usage(const char *prog, int multi_mode)
{
    printf("Usage: %s [<size_mb>] [%s] [options]\n\n", prog,
           multi_mode ? "<num_files>" : "<path>");
    printf("Each worker thread owns one file and issues synchronous O_DIRECT\n"
           "I/O against it, so the file count sets the device queue depth.\n\n");
    printf("Options:\n");
    printf("  --size-mb N       size of each file in MiB (default %d)\n",
           DS_DEFAULT_SIZE_MB);
    printf("  --files N         worker threads / files, i.e. queue depth%s\n",
           multi_mode ? " (default 2)" : " (default 1)");
    printf("  --path P          %s\n", multi_mode
           ? "file path prefix (default ./stress_test_file_)"
           : "file to write (default ./stress_test_file_linux.dat)");
    printf("  --seq-block KB    sequential block size (default %d)\n",
           DS_DEFAULT_SEQ_KB);
    printf("  --rand-block KB   random block size (default %d)\n",
           DS_DEFAULT_RAND_KB);
    printf("  --read-pct N      %% of random ops that are reads (default %d)\n",
           DS_DEFAULT_READ_PCT);
    printf("  --mixed-sec N     seconds of random I/O per iteration"
           " (default %d, 0 disables)\n", DS_DEFAULT_MIXED_SEC);
    printf("  --iterations N    stop after N iterations (default: run forever)\n");
    printf("  --duration N      stop after N seconds (default: run forever)\n");
    printf("  --seed N          fixed base seed for reproducible runs\n");
    printf("  --recreate        delete and recreate the files each iteration\n");
    printf("  --no-direct       use buffered I/O instead of O_DIRECT\n");
    printf("  --no-verify       skip data integrity checking\n");
    printf("  --help            show this help\n\n");
    printf("Exit status: 0 clean, 1 setup error, 2 corruption or I/O errors.\n");
}

static int parse_ll(const char *s, long long *out, long long min, long long max,
                    const char *name)
{
    char *end = NULL;
    long long v;

    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || (end != NULL && *end != '\0')) {
        fprintf(stderr, "ERROR: %s: '%s' is not a number\n", name, s);
        return -1;
    }
    if (v < min || v > max) {
        fprintf(stderr, "ERROR: %s must be between %lld and %lld (got %lld)\n",
                name, min, max, v);
        return -1;
    }
    *out = v;
    return 0;
}

/* Returns 0 to run, 1 to exit successfully (--help), -1 on a bad argument. */
int ds_parse_args(DsConfig *cfg, int argc, char **argv)
{
    long long v;
    int positional = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            ds_usage(argv[0], cfg->multi_mode);
            return 1;
        }
        if (strcmp(a, "--no-direct") == 0) {
            cfg->direct_io = 0;
            continue;
        }
        if (strcmp(a, "--no-verify") == 0) {
            cfg->verify = 0;
            continue;
        }
        if (strcmp(a, "--recreate") == 0) {
            cfg->recreate = 1;
            continue;
        }
        if (strncmp(a, "--", 2) == 0) {
            const char *val;

            if (i + 1 >= argc) {
                fprintf(stderr, "ERROR: %s needs a value\n", a);
                return -1;
            }
            val = argv[++i];
            if (strcmp(a, "--size-mb") == 0) {
                if (parse_ll(val, &v, 1, 1LL << 30, a) != 0) return -1;
                cfg->file_size = v * 1024 * 1024;
            } else if (strcmp(a, "--files") == 0) {
                if (parse_ll(val, &v, 1, 4096, a) != 0) return -1;
                cfg->num_files = (int)v;
            } else if (strcmp(a, "--path") == 0) {
                cfg->path = val;
            } else if (strcmp(a, "--seq-block") == 0) {
                if (parse_ll(val, &v, 1, 1024 * 1024, a) != 0) return -1;
                cfg->seq_block = (size_t)v * 1024;
            } else if (strcmp(a, "--rand-block") == 0) {
                if (parse_ll(val, &v, 1, 1024 * 1024, a) != 0) return -1;
                cfg->rand_block = (size_t)v * 1024;
            } else if (strcmp(a, "--read-pct") == 0) {
                if (parse_ll(val, &v, 0, 100, a) != 0) return -1;
                cfg->read_pct = (int)v;
            } else if (strcmp(a, "--mixed-sec") == 0) {
                if (parse_ll(val, &v, 0, 86400, a) != 0) return -1;
                cfg->mixed_sec = (int)v;
            } else if (strcmp(a, "--iterations") == 0) {
                if (parse_ll(val, &v, 0, 1LL << 40, a) != 0) return -1;
                cfg->iterations = v;
            } else if (strcmp(a, "--duration") == 0) {
                if (parse_ll(val, &v, 0, 1LL << 30, a) != 0) return -1;
                cfg->duration_sec = v;
            } else if (strcmp(a, "--seed") == 0) {
                if (parse_ll(val, &v, 1, (long long)((1ULL << 62) - 1), a) != 0) return -1;
                cfg->seed = (uint64_t)v;
            } else {
                fprintf(stderr, "ERROR: unknown option %s\n", a);
                return -1;
            }
            continue;
        }

        /* Positional arguments, kept for backwards compatibility. */
        positional++;
        if (positional == 1) {
            if (parse_ll(a, &v, 1, 1LL << 30, "<size_mb>") != 0) return -1;
            cfg->file_size = v * 1024 * 1024;
        } else if (positional == 2 && cfg->multi_mode) {
            if (parse_ll(a, &v, 1, 4096, "<num_files>") != 0) return -1;
            cfg->num_files = (int)v;
        } else if (positional == 2) {
            cfg->path = a;
        } else {
            fprintf(stderr, "ERROR: unexpected argument '%s'\n", a);
            return -1;
        }
    }

    /* Block sizes must be sector multiples, and seq must divide into rand. */
    if (cfg->rand_block % 512 != 0) {
        fprintf(stderr, "ERROR: --rand-block must be a multiple of 512 bytes\n");
        return -1;
    }
    if (cfg->seq_block % cfg->rand_block != 0) {
        fprintf(stderr, "ERROR: --seq-block (%zu KiB) must be a multiple of"
                " --rand-block (%zu KiB)\n",
                cfg->seq_block / 1024, cfg->rand_block / 1024);
        return -1;
    }
    if (cfg->file_size < (long long)cfg->seq_block) {
        fprintf(stderr, "ERROR: file size (%lld bytes) is smaller than one"
                " sequential block (%zu bytes)\n", cfg->file_size, cfg->seq_block);
        return -1;
    }
    if (cfg->direct_io && cfg->rand_block % DS_ALIGN != 0) {
        fprintf(stderr, "NOTE: --rand-block %zu KiB is not a multiple of %u bytes;"
                " O_DIRECT may be rejected.\n", cfg->rand_block / 1024, DS_ALIGN);
    }
    return 0;
}
