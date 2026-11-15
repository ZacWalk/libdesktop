# This file is part of libdesktop, a C++20 platform layer for desktop applications
# Copyright 2026 Zac Walker
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Headless tests of the vendored icon rasteriser and its actual file formats."""

import struct
import sys
import unittest
import zlib

sys.dont_write_bytecode = True
from iconlib import Canvas, ICON_SIZES, bgra, bmp, ico, png


class IconTests(unittest.TestCase):
    def test_sizes_are_bounded(self) -> None:
        for size in (0, -1, 2049):
            with self.subTest(size=size), self.assertRaises(ValueError):
                Canvas(size)
        with self.assertRaises(ValueError):
            Canvas(16, 0)

    def test_empty_canvas_is_transparent(self) -> None:
        self.assertEqual(bytes(16 * 16 * 4), Canvas(16).rgba())

    def test_rect_fills_without_losing_colour(self) -> None:
        canvas = Canvas(2)
        canvas.rect(0, 0, 100, 100, (17, 34, 51))
        self.assertEqual(bytes((17, 34, 51, 255)) * 4, canvas.rgba())

    def test_half_coverage_uses_straight_alpha(self) -> None:
        canvas = Canvas(1)
        canvas.rect(0, 0, 50, 100, (200, 80, 40))
        self.assertEqual(bytes((200, 80, 40, 128)), canvas.rgba())

    def test_painters_order(self) -> None:
        canvas = Canvas(2)
        canvas.rect(0, 0, 100, 100, (255, 0, 0))
        canvas.rect(50, 0, 50, 100, (0, 0, 255))
        self.assertEqual(bytes((255, 0, 0, 255, 0, 0, 255, 255)) * 2, canvas.rgba())

    def test_rect_clips_to_canvas(self) -> None:
        canvas = Canvas(2)
        canvas.rect(-50, -50, 100, 100, (1, 2, 3))
        canvas.rect(150, 150, 10, 10, (4, 5, 6))
        self.assertEqual(bytes((1, 2, 3, 255)) + bytes(12), canvas.rgba())

    def test_circle_is_symmetric_and_antialiased(self) -> None:
        canvas = Canvas(24)
        canvas.circle(50, 50, 42, (80, 160, 240))
        rgba = canvas.rgba()
        alphas = rgba[3::4]
        self.assertTrue(any(0 < alpha < 255 for alpha in alphas))
        for y in range(24):
            for x in range(24):
                self.assertEqual(alphas[y * 24 + x], alphas[y * 24 + 23 - x])
                self.assertEqual(alphas[y * 24 + x], alphas[(23 - y) * 24 + x])

    def test_concave_polygon(self) -> None:
        canvas = Canvas(2)
        canvas.polygon(((0, 0), (100, 0), (100, 50), (50, 50), (50, 100), (0, 100)),
                       (3, 4, 5))
        self.assertEqual(bytes((3, 4, 5, 255)) * 3 + bytes(4), canvas.rgba())

    def test_round_line_and_zero_length_segment(self) -> None:
        canvas = Canvas(20)
        canvas.line(((25, 50), (25, 50), (75, 50)), 10, (1, 2, 3))
        self.assertEqual(bytes((1, 2, 3, 255)), canvas.rgba()[(10 * 20 + 10) * 4:(10 * 20 + 11) * 4])
        self.assertEqual(bytes(4), canvas.rgba()[:4])

    def test_gradient_receives_vector_coordinates(self) -> None:
        canvas = Canvas(1)
        canvas.rect(0, 0, 100, 100, lambda x, y: (round(x), round(y), 100))
        self.assertEqual(bytes((50, 50, 100, 255)), canvas.rgba())

    def test_bgra_preserves_alpha(self) -> None:
        self.assertEqual(bytes((3, 2, 1, 4, 7, 6, 5, 8)), bgra(bytes(range(1, 9))))

    def test_png_chunks_crc_and_pixels(self) -> None:
        rgba = bytes((1, 2, 3, 128)) * 4
        encoded = png(2, rgba)
        self.assertEqual(b"\x89PNG\r\n\x1a\n", encoded[:8])
        offset, kinds, compressed = 8, [], b""
        while offset < len(encoded):
            length = struct.unpack_from(">I", encoded, offset)[0]
            kind = encoded[offset + 4:offset + 8]
            data = encoded[offset + 8:offset + 8 + length]
            crc = struct.unpack_from(">I", encoded, offset + 8 + length)[0]
            self.assertEqual(zlib.crc32(kind + data), crc)
            kinds.append(kind)
            if kind == b"IDAT":
                compressed += data
            offset += 12 + length
        self.assertEqual([b"IHDR", b"IDAT", b"IEND"], kinds)
        self.assertEqual(b"\0" + rgba[:8] + b"\0" + rgba[8:], zlib.decompress(compressed))
        self.assertEqual(encoded, png(2, rgba))

    def test_bmp_has_explicit_alpha_and_top_down_rows(self) -> None:
        rgba = bytes((1, 2, 3, 128))
        encoded = bmp(1, rgba)
        self.assertEqual(b"BM", encoded[:2])
        self.assertEqual(len(encoded), struct.unpack_from("<I", encoded, 2)[0])
        self.assertEqual((108, 1, -1), struct.unpack_from("<Iii", encoded, 14))
        self.assertEqual((0xFF0000, 0xFF00, 0xFF, 0xFF000000),
                         struct.unpack_from("<4I", encoded, 54))
        self.assertEqual(bytes((3, 2, 1, 128)), encoded[122:])

    def test_rectangular_png_preserves_dimensions(self) -> None:
        rgba = bytes((12, 34, 56, 255)) * 6
        encoded = png(3, rgba, 2)
        self.assertEqual((3, 2), struct.unpack_from(">2I", encoded, 16))
        length = struct.unpack_from(">I", encoded, 33)[0]
        self.assertEqual(b"\0" + rgba[:12] + b"\0" + rgba[12:],
                         zlib.decompress(encoded[41:41 + length]))

    def test_ico_directory_frames_and_masks(self) -> None:
        images = {size: bytes((12, 34, 56, 255)) * size * size for size in ICON_SIZES}
        images[16] = bytes(4) + images[16][4:]
        encoded = ico(images)
        self.assertEqual((0, 1, len(ICON_SIZES)), struct.unpack_from("<3H", encoded))
        end = 6 + 16 * len(ICON_SIZES)
        for i, size in enumerate(ICON_SIZES):
            with self.subTest(size=size):
                width, height, _, _, planes, bits, length, offset = struct.unpack_from(
                    "<4B2H2I", encoded, 6 + 16 * i)
                self.assertEqual((size % 256, size % 256, 1, 32), (width, height, planes, bits))
                self.assertEqual(end, offset)
                end = offset + length
                frame = encoded[offset:end]
                if size == 256:
                    self.assertEqual(png(size, images[size]), frame)
                else:
                    self.assertEqual((40, size, size * 2), struct.unpack_from("<Iii", frame))
                    self.assertEqual(bgra(images[size][(size - 1) * size * 4:]),
                                     frame[40:40 + size * 4])
                    stride = ((size + 31) // 32) * 4
                    mask = frame[40 + size * size * 4:]
                    self.assertEqual(stride * size, len(mask))
                    self.assertEqual(0x80 if size == 16 else 0, mask[(size - 1) * stride])
        self.assertEqual(len(encoded), end)
        self.assertEqual(encoded, ico(images))

    def test_ico_rejects_oversized_frame(self) -> None:
        with self.assertRaises(ValueError):
            ico({257: bytes(257 * 257 * 4)})


if __name__ == "__main__":
    unittest.main()
