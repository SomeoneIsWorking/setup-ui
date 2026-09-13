#!/usr/bin/env python3
"""Convert a raw ARGB8888 frame capture into a PNG.

SDL's SDL_PIXELFORMAT_ARGB8888 stores bytes B,G,R,A on little-endian hosts,
while PNG wants R,G,B,A. Writing the raw bytes straight into a PNG silently
swaps red and blue, which makes a correct screen look colour-shifted.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


def chunk(tag: bytes, payload: bytes) -> bytes:
    body = tag + payload
    return (
        struct.pack(">I", len(payload))
        + body
        + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path, help="raw ARGB8888 frame")
    parser.add_argument("png", type=Path, help="destination PNG")
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    args = parser.parse_args()

    data = args.raw.read_bytes()
    expected = args.width * args.height * 4
    if len(data) != expected:
        raise SystemExit(f"frame is {len(data)} bytes, expected {expected}")

    rows = bytearray()
    for y in range(args.height):
        line = data[y * args.width * 4 : (y + 1) * args.width * 4]
        rows.append(0)
        for x in range(args.width):
            b, g, r, a = line[x * 4 : x * 4 + 4]
            rows += bytes((r, g, b, a))

    args.png.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(
            b"IHDR", struct.pack(">IIBBBBB", args.width, args.height, 8, 6, 0, 0, 0)
        )
        + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
        + chunk(b"IEND", b"")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
