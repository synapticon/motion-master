#!/usr/bin/env python3
"""Show what a SOMANET drive does when a second SPoE client connects.

The SPoE specification and the firmware disagree. The specification says that the drive
disconnects the first client and serves the second one. The firmware source keeps the first
client and never answers the second one, because `Sock_SaveHandle` has a single client slot.
This script asks the drive.

Every request is SERVER_INFO (0x20). It reads the protocol version and the PDO mode, and it
changes nothing on the drive. Disconnect every other SPoE client from the drive before you run it.
"""

import argparse
import socket
import struct
import sys

SERVER_INFO = 0x20
HEADER = struct.Struct("<BHHH")  # message type, sequence id, status, data length


def recv_exact(sock: socket.socket, count: int) -> bytes:
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise ConnectionError("the drive closed the connection")
        data += chunk
    return data


def server_info(sock: socket.socket, sequence_id: int) -> str:
    """Send SERVER_INFO and describe the reply, the silence, or the closed connection."""
    try:
        sock.sendall(HEADER.pack(SERVER_INFO, sequence_id, 0, 0))
        msg_type, reply_id, status, length = HEADER.unpack(recv_exact(sock, HEADER.size))
        data = recv_exact(sock, length)
    except socket.timeout:
        return "NO ANSWER (timeout)"
    except (ConnectionError, OSError) as error:
        return f"CONNECTION CLOSED ({error})"
    if msg_type != SERVER_INFO or reply_id != sequence_id:
        return f"UNEXPECTED REPLY (type 0x{msg_type:02X}, sequence id {reply_id})"
    if length < 3:
        return f"ANSWER with status {status} and {length} data bytes"
    version = data[0] | (data[1] << 8)
    return f"ANSWER: protocol version 0x{version:04X}, PDO mode {data[2]}"


def connect(host: str, port: int, timeout: float) -> socket.socket:
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host", help="IP address of the drive")
    parser.add_argument("--port", type=int, default=8080, help="SPoE port (default: 8080)")
    parser.add_argument("--timeout", type=float, default=2.0,
                        help="seconds to wait for each answer (default: 2)")
    args = parser.parse_args()

    a = connect(args.host, args.port, args.timeout)
    print(f"1. Client A connects, then asks:            {server_info(a, 1)}")

    try:
        b = connect(args.host, args.port, args.timeout)
    except OSError as error:
        print(f"2. Client B cannot connect: {error}")
        b = None
    if b is not None:
        print(f"2. Client B connects, then asks:            {server_info(b, 1)}")
    print(f"3. Client A asks again:                     {server_info(a, 2)}")

    a.close()
    if b is not None:
        print(f"4. Client A disconnects. Client B asks:     {server_info(b, 2)}")
        b.close()
    c = connect(args.host, args.port, args.timeout)
    print(f"5. Client C connects, then asks:            {server_info(c, 1)}")
    c.close()

    print()
    print("Firmware source predicts:  1 ANSWER, 2 NO ANSWER, 3 ANSWER, 4 NO ANSWER, 5 ANSWER")
    print("Specification predicts:    1 ANSWER, 2 ANSWER, 3 CONNECTION CLOSED, 4 ANSWER, 5 ANSWER")
    print("Step 5 checks that the drive accepts a client again after the test.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
