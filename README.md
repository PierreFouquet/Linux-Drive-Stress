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

- **The drive is the bottleneck, not the CPU.** Block contents come from
  xorshift64 seeded per block, which fills memory at several GB/s. A naive
  `rand()`-per-byte loop caps out around 50 MB/s and would measure the CPU
  instead of the drive.
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
  base seed     0x0000117a6a99931a
Press Ctrl+C to stop.

Iteration 1
  seq write       1024.0 MiB in   1.08 s     949.4 MiB/s
  seq read        1024.0 MiB in   0.50 s    2031.3 MiB/s
  rand 4KiB        55670 IOPS     217.5 MiB/s  (408474 rd / 175394 wr, 70% read target)
               p50 0.053  p95 0.090  p99 0.262  p99.9 0.721  max 469.762  (ms)
  re-verify       1024.0 MiB in   0.45 s    2265.4 MiB/s
  verify          3643.6 MiB checked, 0 mismatches
```

Watch the latency tail and the read/write bandwidth across iterations. A drive
that is overheating, running out of SLC cache, or failing shows up as climbing
p99/p99.9 latency or falling bandwidth well before it returns bad data.

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
