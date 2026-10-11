#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Validates the Godot test project's screenshot (tests/godot/project/main.tscn).

usage: check_godot_screenshot.py <file.png>

Checks: not a single colour, the sky (top right) is bluish, the floor (bottom centre) is
grey and lit, the red box and the yellow/blue sphere are present. Exits 1 on failure.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_pixel import read_png  # noqa: E402


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    width, height, channels, rows = read_png(sys.argv[1])

    def px(fx, fy):
        x, y = min(int(fx * width), width - 1), min(int(fy * height), height - 1)
        o = x * channels
        return rows[y][o], rows[y][o + 1], rows[y][o + 2]

    colours = {tuple(r[i:i + 3]) for r in rows[::4] for i in range(0, width * channels, channels * 4)}
    counts = {"red box": 0, "sphere yellow": 0, "sphere blue": 0}
    for r in rows[::2]:
        for i in range(0, width * channels, channels * 2):
            red, green, blue = r[i], r[i + 1], r[i + 2]
            if red > 150 and green < 80 and blue < 80:
                counts["red box"] += 1
            elif red > 200 and green > 180 and blue < 90:
                counts["sphere yellow"] += 1
            elif blue > 120 and red < 70 and green < 90:
                counts["sphere blue"] += 1

    failures = []
    if len(colours) < 50:
        failures.append(f"only {len(colours)} distinct colours")
    sky = px(0.9, 0.12)
    if not (sky[2] > sky[0] + 30 and sky[2] > 150):
        failures.append(f"sky pixel {sky} is not bluish")
    floor = px(0.5, 0.95)
    if not (abs(floor[0] - floor[2]) < 25 and floor[0] > 90):
        failures.append(f"floor pixel {floor} is not lit grey")
    for name, n in counts.items():
        if n < 100:
            failures.append(f"{name}: {n} pixels")
    print(f"{width}x{height}, {len(colours)} colours, sky {sky}, floor {floor}, "
          + ", ".join(f"{k} {v}" for k, v in counts.items()))
    for f in failures:
        print("FAIL:", f)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
