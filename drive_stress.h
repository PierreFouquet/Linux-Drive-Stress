/*
 * drive_stress.h - shared engine for the Linux drive stress tests.
 *
 * The engine drives one file per worker thread. Each worker performs, per
 * iteration: a sequential write pass, a sequential read+verify pass, a timed
 * random read/write pass, and a final full verify. Buffer contents are a pure
 * function of (seed, file offset), so any block can be verified independently
 * of the order in which it was written.
 */
#ifndef DRIVE_STRESS_H
#define DRIVE_STRESS_H

#include <stddef.h>
#include <stdint.h>

/* Buffer/offset alignment used for O_DIRECT. */
#define DS_ALIGN 4096u

/* Log-linear latency histogram: 8 sub-buckets per power of two. */
#define DS_HIST_BUCKETS 512

typedef struct {
    long long   file_size;    /* bytes per file, rounded to a seq_block multiple */
    int         num_files;    /* worker threads; each owns exactly one file */
    size_t      seq_block;    /* sequential I/O size */
    size_t      rand_block;   /* random I/O size, also the verify granularity */
    int         direct_io;    /* attempt O_DIRECT */
    int         verify;       /* verify data integrity */
    int         read_pct;     /* share of random ops that are reads, 0-100 */
    int         mixed_sec;    /* seconds of random mixed I/O per iteration */
    int         recreate;     /* unlink + recreate files every iteration */
    long long   iterations;   /* 0 = until interrupted */
    long long   duration_sec; /* 0 = until interrupted */
    uint64_t    seed;         /* 0 = derive from clock */
    const char *path;         /* file path, or directory/prefix in multi mode */
    int         multi_mode;   /* path is a prefix; num_files is user-settable */
} DsConfig;

void ds_config_defaults(DsConfig *cfg);
int  ds_parse_args(DsConfig *cfg, int argc, char **argv);
void ds_usage(const char *prog, int multi_mode);

/* Returns 0 if every byte verified, non-zero on I/O error or data mismatch. */
int  ds_run(DsConfig *cfg);

#endif /* DRIVE_STRESS_H */
