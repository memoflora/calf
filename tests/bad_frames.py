#!/usr/bin/env python3
# Sends valid, malformed, and split frames to a running server and checks each
# result. Usage: tests/bad_frames.py [port]. Exit code 0 if all cases pass.
import itertools
import socket
import struct
import sys
import threading

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7070
OK = "ok"              # reply with length 1 and status 0
REJECTED = "rejected"  # reply with length 1 and status 1
CLOSED = "closed"      # server closed the connection with no reply

# The server rejects a timestamp <= the last one in a series, so each valid
# case writes to its own series.
_names = itertools.count()


def new_name():
    return b"s%d" % next(_names)


def connect():
    s = socket.create_connection(("127.0.0.1", PORT))
    # A server that waits for bytes that never come shows as a timeout, not
    # as a hang of this script.
    s.settimeout(2)
    return s


def payload(msg_type=1, name=None, samples=1, sample_count=None, extra=b""):
    name = new_name() if name is None else name
    count = samples if sample_count is None else sample_count
    p = struct.pack("<BHH", msg_type, 1, len(name)) + name + struct.pack("<I", count)
    p += b"".join(struct.pack("<qQ", i, 0) for i in range(samples))
    return p + extra


def multi(series):
    """series is a list of (name, [ts_ms, ...])."""
    p = struct.pack("<BH", 1, len(series))
    for name, stamps in series:
        p += struct.pack("<H", len(name)) + name + struct.pack("<I", len(stamps))
        p += b"".join(struct.pack("<qQ", ts, 0) for ts in stamps)
    return p


def frame(p):
    return struct.pack("<I", len(p)) + p


def query_payload(name, start, end):
    return struct.pack("<BH", 2, len(name)) + name + struct.pack("<qq", start, end)


def result(s):
    data = b""
    try:
        while len(data) < 5:
            chunk = s.recv(5 - len(data))
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        return "timeout"
    except ConnectionResetError:
        return CLOSED
    if len(data) == 5:
        return {(1, 0): OK, (1, 1): REJECTED}.get(struct.unpack("<IB", data),
                                                  "bad reply %r" % data)
    return CLOSED if not data else "short reply %r" % data


def send_and_check(data, one_byte_per_send=False):
    s = connect()
    if one_byte_per_send:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for b in data:
            s.send(bytes([b]))
    else:
        s.sendall(data)
    r = result(s)
    s.close()
    return r


def two_requests():
    s = connect()
    results = []
    for _ in range(2):
        s.sendall(frame(payload()))
        results.append(result(s))
    s.close()
    return OK if results == [OK, OK] else str(results)


def sequence(payloads):
    s = connect()
    results = []
    for p in payloads:
        s.sendall(frame(p))
        results.append(result(s))
    s.close()
    return results


def last_of(payloads, want_before):
    # The earlier requests set up the series. They must give want_before.
    results = sequence(payloads)
    if results[:-1] != want_before:
        return "setup gave %s" % results[:-1]
    return results[-1]


def rejection_leaves_no_trace():
    a, b = new_name(), new_name()
    # b is bad, so the whole request is rejected, and a must stay unused.
    return last_of([multi([(a, [5]), (b, [2, 1])]), multi([(a, [5])])], [REJECTED])


def same_ts_from_many_connections():
    # Many connections in flight usually land in one batch, so this checks the
    # rule against earlier requests in the same batch, not only the store.
    name = new_name()
    results = []
    lock = threading.Lock()

    def one():
        r = send_and_check(frame(multi([(name, [7])])))
        with lock:
            results.append(r)

    threads = [threading.Thread(target=one) for _ in range(20)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if results.count(OK) == 1 and results.count(REJECTED) == 19:
        return OK
    return "ok=%d rejected=%d other=%s" % (
        results.count(OK), results.count(REJECTED),
        [r for r in results if r not in (OK, REJECTED)])


def recv_exact(s, n):
    data = b""
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            break
        data += chunk
    return data


def write_then_query():
    # A WRITE, then a QUERY for the same series: the OK reply must already
    # make the samples visible.
    name = new_name()
    s = connect()
    s.sendall(frame(multi([(name, [5, 6])])))
    r = result(s)
    if r != OK:
        return "write gave %s" % r
    s.sendall(frame(query_payload(name, 0, 100)))
    head = recv_exact(s, 9)
    want = struct.pack("<IBI", 1 + 4 + 32, 0, 2)
    if head != want:
        return "query reply header %r" % head
    body = recv_exact(s, 32)
    s.close()
    if body != struct.pack("<qQqQ", 5, 0, 6, 0):
        return "query samples %r" % body
    return OK


def abrupt_closes_then_valid():
    # SO_LINGER 0 makes close() send RST. The server's reply then hits a dead
    # peer, which raises SIGPIPE if the server does not ignore it.
    for _ in range(200):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.connect(("127.0.0.1", PORT))
        s.sendall(frame(payload()))
        s.close()
    return send_and_check(frame(payload()))


CASES = [
    ("valid frame", OK, lambda: send_and_check(frame(payload()))),
    ("valid frame, 1 byte per send", OK,
     lambda: send_and_check(frame(payload()), one_byte_per_send=True)),
    ("two requests on one connection", OK, two_requests),
    ("series_count 0", OK, lambda: send_and_check(frame(struct.pack("<BH", 1, 0)))),
    ("name_len 0, sample_count 0", OK,
     lambda: send_and_check(frame(struct.pack("<BHHI", 1, 1, 0, 0)))),
    # No payload follows. A server that allocated and waited for it would time out.
    ("length 16 MiB + 1", CLOSED,
     lambda: send_and_check(struct.pack("<I", 16 * 1024 * 1024 + 1))),
    ("length 0xFFFFFFFF", CLOSED, lambda: send_and_check(struct.pack("<I", 0xFFFFFFFF))),
    ("empty payload", CLOSED, lambda: send_and_check(frame(b""))),
    ("msg_type 3", CLOSED, lambda: send_and_check(frame(payload(msg_type=3)))),
    ("QUERY name_len past end", CLOSED,
     lambda: send_and_check(frame(struct.pack("<BH", 2, 500) + b"ab"))),
    ("QUERY ends before end_ms", CLOSED,
     lambda: send_and_check(frame(query_payload(b"a", 0, 9)[:-3]))),
    ("QUERY with 1 extra byte", CLOSED,
     lambda: send_and_check(frame(query_payload(b"a", 0, 9) + b"\0"))),
    ("QUERY start > end", REJECTED,
     lambda: send_and_check(frame(query_payload(b"a", 9, 0)))),
    ("WRITE then QUERY on one connection", OK, write_then_query),
    ("sample_count 0xFFFFFFFF", CLOSED,
     lambda: send_and_check(frame(payload(samples=0, sample_count=0xFFFFFFFF)))),
    ("sample_count 2, data for 1", CLOSED,
     lambda: send_and_check(frame(payload(samples=1, sample_count=2)))),
    ("name_len past end", CLOSED,
     lambda: send_and_check(frame(struct.pack("<BHH", 1, 1, 500) + b"ab"))),
    ("1 extra byte after last series", CLOSED,
     lambda: send_and_check(frame(payload(extra=b"\0")))),
    ("valid frame after 200 abrupt closes", OK, abrupt_closes_then_valid),
    ("timestamp equal to last", REJECTED,
     lambda: (lambda n: last_of([multi([(n, [5])]), multi([(n, [5])])], [OK]))(new_name())),
    ("timestamp lower than last", REJECTED,
     lambda: (lambda n: last_of([multi([(n, [5])]), multi([(n, [4])])], [OK]))(new_name())),
    ("timestamp higher than last", OK,
     lambda: (lambda n: last_of([multi([(n, [5])]), multi([(n, [6])])], [OK]))(new_name())),
    ("decreasing timestamps in one request", REJECTED,
     lambda: send_and_check(frame(multi([(new_name(), [2, 1])])))),
    ("same name twice, second lower", REJECTED,
     lambda: (lambda n: send_and_check(frame(multi([(n, [5]), (n, [4])]))))(new_name())),
    ("same name twice, second higher", OK,
     lambda: (lambda n: send_and_check(frame(multi([(n, [5]), (n, [6])]))))(new_name())),
    ("empty name with samples", REJECTED,
     lambda: send_and_check(frame(multi([(b"", [1])])))),
    ("rejected request leaves no trace", OK, rejection_leaves_no_trace),
    ("same ts from 20 connections", OK, same_ts_from_many_connections),
]


def main():
    failed = 0
    for name, want, run in CASES:
        got = run()
        status = "PASS" if got == want else "FAIL"
        failed += got != want
        print(f"{status}  {name:38s} want {want:6s} got {got}")
    print(f"{len(CASES) - failed}/{len(CASES)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
