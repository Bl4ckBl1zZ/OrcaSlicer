"""A minimal PNG reader for picture tests (standard library only).

Reads 8-bit RGB and RGBA, non-interlaced: what the server writes. Returns an Image whose pixel
(u, v) has u to the right and v down from the top-left corner, like the pixel coordinates the
`screenshot` tool reports.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass


@dataclass
class Image:
    width: int
    height: int
    channels: int
    rows: list  # one bytes object per row, top row first

    def pixel(self, u: int, v: int) -> tuple:
        """(r, g, b) at column u, row v."""
        offset = u * self.channels
        return tuple(self.rows[v][offset:offset + 3])

    def count(self, predicate, box: tuple | None = None, step: int = 1) -> int:
        """Number of pixels for which predicate((r, g, b)) holds, optionally inside box (u0, v0, u1, v1)."""
        u0, v0, u1, v1 = box or (0, 0, self.width, self.height)
        total = 0
        for v in range(max(0, v0), min(self.height, v1), step):
            row = self.rows[v]
            for u in range(max(0, u0), min(self.width, u1), step):
                offset = u * self.channels
                if predicate((row[offset], row[offset + 1], row[offset + 2])):
                    total += 1
        return total

    def bounding_box(self, predicate) -> tuple | None:
        """(u0, v0, u1, v1), inclusive, of the pixels for which predicate holds; None when there are none."""
        us, vs = [], []
        for v, row in enumerate(self.rows):
            hits = [u for u in range(self.width) if predicate(tuple(row[u * self.channels:u * self.channels + 3]))]
            if hits:
                vs.append(v)
                us.append(hits[0])
                us.append(hits[-1])
        if not vs:
            return None
        return min(us), vs[0], max(us), vs[-1]


def close_to(color: tuple, tolerance: int = 12):
    """A predicate: within `tolerance` of `color` on every channel."""
    return lambda p: all(abs(int(p[i]) - int(color[i])) <= tolerance for i in range(3))


def hex_color(text: str) -> tuple:
    """'#RRGGBB' -> (r, g, b)."""
    text = text.lstrip("#")
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


def read_png(data: bytes) -> Image:
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    position = 8
    header = None
    compressed = bytearray()
    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        body = data[position + 8:position + 8 + length]
        position += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
    width, height, depth, color_type, _, _, interlace = header
    assert depth == 8 and interlace == 0 and color_type in (2, 6), f"unsupported PNG: depth {depth}, type {color_type}"
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(bytes(compressed))
    rows = []
    previous = bytes(stride)
    offset = 0
    for _ in range(height):
        filter_type = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        if filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                up = previous[i]
                up_left = previous[i - channels] if i >= channels else 0
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                line[i] = (line[i] + predictor) & 0xFF
        previous = bytes(line)
        rows.append(previous)
    return Image(width, height, channels, rows)
