#!/usr/bin/env python3
"""Request TIFF images without operating the laser or any GPIO pins."""
import argparse
import socket
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--count', type=int, default=1)
    parser.add_argument('--interval', type=float, default=6.0)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.count < 1 or args.interval < 0:
        parser.error('Use a valid port, positive count and non-negative interval.')
    try:
        with socket.create_connection((args.host, args.port), timeout=10) as connection:
            connection.settimeout(10)
            with connection.makefile('rb') as replies:
                deadline = time.monotonic()
                for i in range(args.count):
                    if i:
                        time.sleep(max(0, deadline - time.monotonic()))
                    connection.sendall(b'store\n')
                    reply = replies.readline(4096)
                    if not reply.endswith(b'\n') or not reply.startswith(b'OK '):
                        raise RuntimeError(reply.decode('utf-8', 'replace').strip() or 'Receiver disconnected')
                    print(reply.decode('utf-8', 'replace').strip(), flush=True)
                    deadline += args.interval
    except (OSError, RuntimeError) as exc:
        print(f'Frame request failed: {exc}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == '__main__':
    sys.exit(main())
