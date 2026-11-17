# Calf design and results

This page has the detail behind the results in the [README](../README.md).

## Recovery keeps every complete record and truncates the rest

Each WAL record has this layout:

```
u8 type (1 = COMMIT) | u32 length | u32 crc32 | payload
```

The crc32 (from zlib) covers the type, the length, and the payload. The
payload holds the new series (id and name) and the samples (series id, ts_ms,
and value bits).

At start, replay decodes records until it finds a record that is not complete
and valid. Replay stops for one of these reasons:

- end of file
- short record
- bad type
- bad length
- bad crc

Replay treats each stop as a torn tail from a crash during a write. It keeps
the records before the stop, truncates the file at that offset, and syncs the
truncate before the first new append. Replay does not check that the bad
record is at the end of the file. See the third item in
[Known limits](../README.md#known-limits).

Two other cases stop the server. In the first case, a record has a good crc,
but its payload does not parse. The writer wrote that record, so the bad
payload is a bug, not a crash. In the second case, a record does not apply to
the store, for example because a timestamp goes back. In both cases, the
server refuses to start and does not truncate the file.

A crash during the truncate is safe. Replay does not change the file. Only the
truncate does. If the process crashes before the truncate reaches the disk,
the file still has the same bytes. The next start runs the same replay,
truncates at the same offset, and gets the same result.

## The tests cut every offset, flip every byte, and kill the server 50 times

- Truncation (`tests/record_test.cpp`). The test encodes 50 records and cuts
  them at every byte offset from 0 to the end. For each cut, the decoder
  returns exactly the complete records before the cut and stops at the
  correct offset.
- Corruption (`tests/record_test.cpp`). The test flips every byte of every
  record, one byte at a time. For each flip, the decoder keeps exactly the
  records before the damaged record. It never reads the damaged record as
  valid.
- Crash loop (`tests/crash_loop.sh`, seed 6430, Release build). The script
  runs the server 50 times on one WAL. In each run, 8 client threads send
  writes, and the script sends SIGKILL after a random delay of 0 to 300 ms.
  After each restart, the script compares the WAL with the client logs. The
  totals for the 50 runs are:
  - 13004 records in the WAL.
  - 12774 acked requests, all present in the WAL.
  - 230 records present but not acked. The kill came after the write and
    before the reply.
  - 170 requests sent but absent. None of them was acked.

  No acked request was lost.
- Torn and damaged WAL (`tests/crash_loop.sh`). The script cuts 5 bytes off
  the last record. In a second case, it flips one byte in the middle of a WAL
  of 200 records. In both cases, the server truncates the WAL at the bad
  record on restart. In the flip case, the 99 records before the damage stay.
  The server then accepts 100 new writes, and all 100 are acked and present
  in the WAL.

SIGKILL stops the process, but the kernel keeps its page cache and writes the
data to disk later. So the crash loop proves two things. Calf acks only
records that it wrote, and replay handles a torn tail. The crash loop does
not prove that the sync reaches the disk. A power-loss test needs a real
power cut or a device that drops unflushed writes, for example a dm-flakey or
dm-log-writes target on Linux. The test then does the same check of acked
requests against the WAL.

## Group commit gives 37 times the throughput at 64 writers

This benchmark compares group commit (`group`) with one sync per request
(`--max-batch 1`, shown as `batch1`). The numbers come from
`bench/results.md`. The environment is:

- OS: macOS 26.6.2, kernel 25.6.0 (arm64)
- CPU: Apple M3, 8 cores
- Disk: APPLE SSD AP0512Z
- Filesystem: apfs, with the WAL in `bench/` in the repo
- Compiler: Apple clang version 21.0.0 (clang-2100.1.1.101),
  `CMAKE_BUILD_TYPE=Release`
- Sync call: `fcntl(F_FULLFSYNC)` after each WAL append. On macOS, plain
  `fsync` does not flush the drive cache, so a benchmark with `fsync` does
  not measure durable writes.

W is the number of writer connections. Each writer keeps one request in
flight, and each request has 10 samples. Each configuration ran 3 times for
10 s, each time with a new WAL and a new server. Each row shows the run with
the median requests/s, so all columns in a row come from the same run. The
last column, W x R, is requests/s times the mean latency. Little's law says
that W x R must equal W, so the column checks the measurement.

| mode | W | requests/s | samples/s | p50 ms | p99 ms | mean ms | mean batch | W x R |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| group | 1 | 374 | 3745 | 2.29 | 5.17 | 2.67 | 1.00 | 1.00 |
| group | 4 | 864 | 8641 | 4.58 | 8.16 | 4.63 | 2.00 | 4.00 |
| group | 16 | 3579 | 35787 | 3.75 | 12.31 | 4.47 | 7.96 | 15.99 |
| group | 64 | 15656 | 156565 | 4.16 | 6.97 | 4.09 | 31.59 | 63.97 |
| batch1 | 1 | 388 | 3883 | 2.33 | 4.51 | 2.57 | 1.00 | 1.00 |
| batch1 | 4 | 450 | 4502 | 8.52 | 15.07 | 8.88 | 1.00 | 4.00 |
| batch1 | 16 | 442 | 4424 | 35.27 | 56.05 | 36.03 | 1.00 | 15.94 |
| batch1 | 64 | 423 | 4231 | 145.12 | 212.92 | 149.17 | 1.00 | 63.11 |

At W = 64, group commit does 15,656 requests/s and batch1 does 423. The
speedup is 37× (15,656 / 423).

The speedup comes from the batch size. Throughput is the batch size times
the sync rate. At W = 64, group mode syncs 496 times per second
(15,656 / 31.59), and batch1 syncs 423 times per second. The two sync rates
are close, so almost all of the gain comes from the mean batch of 31.59
requests.

The mean batch is about W/2, not W, because the writers split into two
groups that take turns. While batch A syncs, the writers outside A get the
replies for the batch before A and send again. Those requests form batch B.
When A finishes, its writers send again while B syncs.

PostgreSQL's `commit_delay` makes the writer wait a short time before the
sync, so more requests join the batch. The cost is higher latency. Calf does
not have this option.

In batch1 mode, latency grows with W. Each request waits for the syncs of all
the requests in front of it.

To run the benchmark, build in Release and run `scripts/bench.sh build`. The
script refuses a debug build, a sanitizer build, and a WAL on tmpfs. It
writes every run to `bench/results.csv`. It writes the medians and the
chunk_stats table to `bench/results.md`.

## A counter compresses to 3.4 bytes per sample

`build/chunk_stats` encodes 1,000,000 samples for each series type at a 15 s
interval. Counter and gauge timestamps have ±5 ms of jitter. Timestamps use
delta-of-delta encoding, and values use XOR encoding.

```
type          samples   chunks      bytes  bytes/smp
counter       1000000     8865    3397284      3.397
gauge         1000000     8865    8379004      8.379
constant      1000000     8334     391600      0.392
raw           1000000        -   16000000     16.000
```

The noisy gauge compresses badly. Each gauge value has 53 random mantissa
bits, so the XOR of two neighboring values has almost no leading or trailing
zero bits to skip.

## Queries run in parallel under a shared lock

A QUERY frame holds a series name and an inclusive range from start_ms to
end_ms. The server replies OK with the samples in the range. It replies ERROR
if start_ms is after end_ms or if the result has more than 1,000,000 samples.
Queries take a shared lock on the one `std::shared_mutex` in Head, so they run
in parallel. The writer takes the exclusive lock only while it applies a
synced batch.
