#!/usr/bin/env python3
# Sends valid, malformed, and split frames to a running server and checks each
# result. Usage: tests/bad_frames.py [port]. Exit code 0 if all cases pass.
import socket
import struct
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7070
OK = "ok"          # reply with length 1 and status 0
CLOSED = "closed"  # server closed the connection with no reply


def connect():
    s = socket.create_connection(("127.0.0.1", PORT))
    # A server that waits for bytes that never come shows as a timeout, not
    # as a hang of this script.
    s.settimeout(2)
    return s


def payload(msg_type=1, name=b"ab", samples=1, sample_count=None, extra=b""):
    count = samples if sample_count is None else sample_count
    p = struct.pack("<BHH", msg_type, 1, len(name)) + name + struct.pack("<I", count)
    p += b"".join(struct.pack("<qQ", i, 0) for i in range(samples))
    return p + extra


def frame(p):
    return struct.pack("<I", len(p)) + p


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
        return OK if struct.unpack("<IB", data) == (1, 0) else "bad reply %r" % data
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
    ("msg_type 2", CLOSED, lambda: send_and_check(frame(payload(msg_type=2)))),
    ("sample_count 0xFFFFFFFF", CLOSED,
     lambda: send_and_check(frame(payload(samples=0, sample_count=0xFFFFFFFF)))),
    ("sample_count 2, data for 1", CLOSED,
     lambda: send_and_check(frame(payload(samples=1, sample_count=2)))),
    ("name_len past end", CLOSED,
     lambda: send_and_check(frame(struct.pack("<BHH", 1, 1, 500) + b"ab"))),
    ("1 extra byte after last series", CLOSED,
     lambda: send_and_check(frame(payload(extra=b"\0")))),
    ("valid frame after 200 abrupt closes", OK, abrupt_closes_then_valid),
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
