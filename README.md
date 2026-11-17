```
                     __.-------.__
                    (__/ o   o \__)
    __________________|         |
   /  @@      @      .'---------'.
 ~|     @@     @@   (   @     @   )
  |  @         @@@   '._________.'
   \_________________/
    (__)(__)     (__)(__)
```

# Calf

Calf is a push-based time-series database in C++20. Agents send samples to
the server over TCP. The server replies OK only after it writes the samples to
the write-ahead log (WAL) and syncs the WAL to disk. So an acknowledged
(acked) write is durable: the samples are on the disk before the agent gets
OK. Calf uses [Gorilla](https://www.vldb.org/pvldb/vol8/p1816-teller.pdf) and
[Prometheus TSDB](https://github.com/prometheus/prometheus/tree/main/tsdb) as
references.

## Why a push agent needs durable acks

A push agent keeps a sample only until the server replies OK. Then the agent
deletes the sample. If the server loses an acked write, no copy is left to
send again.

## How a write reaches the disk before the OK

1. The agent sends a WRITE frame. The frame is a u32 length, then the series
   and the samples. `src/protocol.cpp` parses and validates the frame.
2. Each connection has its own thread. The thread reads the frame, calls
   `Writer::submit`, and waits on a future (`src/server.cpp`).
3. The writer thread takes all queued requests in one swap. It checks each
   request against the store, because timestamps must increase in each
   series. It encodes one WAL record for each accepted request
   (`src/writer.cpp`, `src/record.cpp`).
4. The writer appends the records and syncs the WAL once for the whole batch
   (`src/wal.cpp`). The sync call is `fdatasync` on Linux and
   `fcntl(F_FULLFSYNC)` on macOS.
5. After the sync, the writer applies the records to the in-memory Gorilla
   chunks (`src/head.cpp`, `src/chunk/`).
6. The writer sets each future. The connection thread then sends OK or ERROR
   to the agent.

## One sync covers every queued request (group commit)

A sync takes about the same time for one record as for many, because the time
goes to the flush, not to the bytes. While the writer syncs batch N, new
requests wait in the queue. When the sync ends, the writer takes all of them
as batch N + 1, and one sync covers them all.

Group commit needs no timer. The batch closes when the writer is free, so the
previous sync is the only wait. With one client, each batch has one request
and adds no delay. With many clients, the batches grow with no setting to
tune.

## Results

[docs/design.md](docs/design.md) has the full tests, benchmark, and design
detail for each result.

- No acked request was lost in 50 SIGKILL runs. All 12,774 acked requests were
  in the WAL after the restarts. See
  [the tests](docs/design.md#the-tests-cut-every-offset-flip-every-byte-and-kill-the-server-50-times).
- On restart, recovery truncates a torn WAL at the first bad record and keeps
  every record before it. See
  [recovery](docs/design.md#recovery-keeps-every-complete-record-and-truncates-the-rest).
- At 64 writers, group commit does 15,656 requests/s, and one sync per request
  does 423. That is 37 times the throughput. See
  [the benchmark](docs/design.md#group-commit-gives-37-times-the-throughput-at-64-writers).
- A counter compresses to 3.4 bytes per sample, against 16 bytes raw. See
  [compression](docs/design.md#a-counter-compresses-to-34-bytes-per-sample).
- Queries take a shared lock on Head, so they run in parallel with each other.
  See [queries](docs/design.md#queries-run-in-parallel-under-a-shared-lock).

## Known limits

- Each connection has its own thread, so many idle connections use many
  threads.
- Replay reads the whole WAL into memory before it decodes it.
- A bad record in the middle of the WAL stops replay there. The truncate then
  deletes the acked records after it.

## Next steps

- WAL segments and checkpoints. With them, the WAL does not grow forever, and
  replay does not read all of it.
- Memory-mapped head chunks.
- An out-of-order window. Today the server rejects a sample if its timestamp
  is at or below the last timestamp in its series.
- Four reply types. Today the agent gets only OK or ERROR, so it cannot tell
  whether to retry or to give up.

  | Reply | Meaning | Agent action |
  |---|---|---|
  | OK | The server saved all samples. | Deletes them. |
  | PARTIAL | The server rejected some samples and gives a reason for each. It saved the accepted samples as one commit. | Deletes the accepted samples. |
  | PERMANENT_ERROR | The request can never succeed, because it is malformed or its samples are too old. | Drops the request and does not retry. |
  | TEMPORARY_ERROR | The server cannot take the request now, because its queue is full. | Keeps the samples and retries later. |

## Build and run

Build Calf and run the tests:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
(cd build && ctest --output-on-failure)
```

Start a server, then send writes and queries to it:

```sh
build/server 7070 calf.wal                # usage: server [--max-batch N] [port] [wal_path]
build/client 1000 4 7070 --samples 10     # 1000 requests from 4 threads, 10 samples each
build/query <series> <start_ms> <end_ms> 7070
build/server --dump calf.wal              # print every sample in the WAL
scripts/bench.sh build                    # run the benchmark
```
