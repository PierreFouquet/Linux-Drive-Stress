# Linux-Drive-Stress

Two small, dependency-free drive stress tests for Linux. They write real data
to a drive, read it back, verify every byte, and report throughput, IOPS and
latency percentiles.

```
git clone git@github.com:PierreFouquet/Linux-Drive-Stress.git
cd Linux-Drive-Stress/
make
```

`make` produces both binaries. To build by hand:

```
gcc -Wall -Wextra -O2 -std=c99 -o drive_stress_linux drive_stress_linux.c drive_stress.c -lpthread
gcc -Wall -Wextra -O2 -std=c99 -o stress_test_multi  stress_test_multi.c  drive_stress.c -lpthread
```

## What it actually does

Both binaries share the engine in `drive_stress.c`. Each worker thread owns one
file, and each iteration runs four phases in lockstep across all workers:

| Phase | What it measures |
| --- | --- |
| Sequential write | write bandwidth |
| Sequential read + verify | read bandwidth, data integrity |
| Random read/write mix | IOPS, latency distribution, FTL/queue behaviour |
| Full re-read + verify | integrity of everything the random phase touched |

Design points that make the numbers meaningful:

- **The drive is the bottleneck, not the CPU.** Block contents come from eight
  independent xorshift64 lanes seeded per block via splitmix64. Measured on one
  x86 core, generating into a buffer interleaved with real `O_DIRECT` writes:
  **8.9 GiB/s** on the sequential path and **4.1 GiB/s** on the 4 KiB random
  path (12 GiB/s against a cache-resident buffer, but the real write path is
  always cold because DMA invalidates the buffer). A naive `rand()`-per-byte
  loop caps out at **50 MiB/s** and measures the CPU instead of the drive.
  The lanes matter: a single xorshift chain is one long dependency chain and
  reaches only 3.6 GiB/s, which would itself cap a fast NVMe drive.
- **Reads reach the device.** Files are opened `O_DIRECT`, so reads bypass the
  page cache. Where the filesystem rejects `O_DIRECT` (tmpfs, some overlayfs)
  the engine warns and falls back to buffered I/O plus
  `posix_fadvise(POSIX_FADV_DONTNEED)`, which evicts the pages it just wrote.
  Without one of these, a read-back is served from RAM and tests nothing.
- **Real queue depth.** Each worker issues synchronous `pread`/`pwrite` against
  its own file, so `--files N` puts roughly N requests in flight at the device.
  Raising it until IOPS stops climbing is how you find a drive's saturation
  point.
- **Every byte is verified.** Block contents are a pure function of
  `(seed, file offset)`, so any block can be checked independently of the order
  it was written in — which is what makes the random-offset phase verifiable.
  A mismatch prints the file, offset and first differing byte.

## Single file - drive_stress_linux

```
./drive_stress_linux                  # 10 MiB, default filename
./drive_stress_linux 100              # 100 MiB
./drive_stress_linux 50 /mnt/my_drive/test_file.bin
```

## Multiple files - stress_test_multi

```
./stress_test_multi                   # 10 MiB x 2 files
./stress_test_multi 20 5              # 20 MiB x 5 files (queue depth ~5)
./stress_test_multi 256 8 --path /mnt/my_drive/stress_
```

## Options

Both binaries accept the same options; run either with `--help` for the list.

| Option | Meaning |
| --- | --- |
| `--size-mb N` | size of each file in MiB (default 10) |
| `--files N` | worker threads / files, i.e. queue depth |
| `--path P` | file to write, or filename prefix in multi mode |
| `--seq-block KB` | sequential block size (default 1024) |
| `--rand-block KB` | random block size (default 4) |
| `--read-pct N` | share of random ops that are reads (default 70) |
| `--mixed-sec N` | seconds of random I/O per iteration (default 10, `0` disables) |
| `--iterations N` | stop after N iterations |
| `--duration N` | stop after N seconds |
| `--seed N` | fixed base seed, for reproducible runs |
| `--recreate` | delete and recreate the files each iteration (filesystem metadata churn) |
| `--no-direct` | use buffered I/O instead of `O_DIRECT` |
| `--no-verify` | skip integrity checking (pure throughput measurement) |

With no `--iterations` or `--duration`, the test runs until interrupted.
Ctrl+C finishes the current phase and deletes the test files.

## Example

```
$ ./stress_test_multi 256 4 --mixed-sec 10 --iterations 1
Linux drive stress test
  target        stress_test_file_<n>.dat
  files/threads 4  (effective queue depth ~4)
  size per file 256.0 MiB  (1024.0 MiB total)
  sequential    1024 KiB blocks
  random        4 KiB blocks, 70% reads, 10 s per iteration
  O_DIRECT      requested
  verify        on
  base seed     0x000013f96a9996ab
Press Ctrl+C to stop.

Iteration 1
  seq write       1024.0 MiB in   0.99 s    1039.3 MiB/s
  seq read        1024.0 MiB in   0.47 s    2193.3 MiB/s
  rand 4KiB        55596 IOPS     217.2 MiB/s  (410838 rd / 175936 wr, 70% read target)
               p50 0.057  p95 0.090  p99 0.262  p99.9 0.655  max 25.166  (ms)
  re-verify       1024.0 MiB in   0.52 s    1971.2 MiB/s
  verify          3652.8 MiB checked, 0 mismatches
```

Watch the latency tail and the read/write bandwidth across iterations. A drive
that is overheating, running out of SLC cache, or failing shows up as climbing
p99/p99.9 latency or falling bandwidth well before it returns bad data.

### Where the CPU actually goes

Measured for a 256 MiB single-worker iteration, so you can confirm the test is
drive-bound rather than CPU-bound on your own hardware:

| Phase | CPU time / wall time | Dominated by |
| --- | --- | --- |
| Sequential write + read + verify | ~15% | data generation and verification |
| Random 4 KiB mix | ~40%, of which 7/8 is *system* time | the kernel's own I/O path, one syscall per op |

The sequential phases are ~85% idle waiting on the drive, which is what you
want. In the random phase the tool's own work is only about 5% of wall time;
the rest is the kernel executing the I/O. That is inherent to synchronous
one-syscall-per-op I/O, and it means a drive capable of several hundred
thousand IOPS will become syscall-bound before it runs out of headroom. If you
need to push a drive that hard, use `fio` with `--ioengine=io_uring`, which
batches submissions into far fewer syscalls.

### Getting the most accurate bandwidth numbers

Data generation is serial with the write syscall inside each worker, so a
single worker's sequential write figure includes generation cost. At 8.9 GiB/s
generated that is a few percent against a SATA SSD, but around 35% against a
drive that can absorb 5 GiB/s. Two things remove it:

- **Use `--files 4` or more.** Generation is per-worker and scales across
  cores, while the drive is shared, so the drive saturates first: the same
  5 GiB/s drive is understated by about 35% at `--files 1` but only about 12%
  at `--files 4`. This is also the only way to measure a drive's real peak,
  since one thread at queue depth 1 cannot saturate an NVMe drive anyway.
- **Build with `-march=native`** for a local run, which measured about 55%
  more verification throughput here. It is deliberately not the default,
  because the resulting binary will not run on older CPUs:

  ```
  make clean && make CFLAGS="-Wall -Wextra -O3 -march=native -std=c99"
  ```

`--no-verify` also removes the verification pass if you want throughput only,
though then the test no longer checks that the drive returns what it stored.

## Exit status

| Code | Meaning |
| --- | --- |
| 0 | clean run, no corruption or I/O errors |
| 1 | setup error (bad arguments, no space, unwritable target) |
| 2 | data corruption or I/O errors were detected |

## Notes and limitations

- **Testing a specific drive:** point `--path` at a file on a mount of that
  drive. The tools deliberately refuse to write to device nodes such as
  `/dev/sda` — writing raw to a block device destroys the partition table and
  every filesystem on it.
- **Free space:** the run needs `size-mb x files` of free space, checked up
  front. Test files are deleted on exit.
- **SSD wear:** this writes continuously and will consume flash endurance.
  Bounding a run with `--duration` or `--iterations` is a good habit.
- **These tools are not a replacement for [`fio`](https://github.com/axboe/fio)**,
  which supports io_uring, more access patterns and far more reporting. If you
  are diagnosing a drive seriously, cross-check against `fio`. The value here is
  being small, dependency-free and integrity-checking by default.
