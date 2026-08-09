#!/usr/bin/env python3
"""Prove the sketch's hand-maintained PIO encodings still match the .pio sources.

arduino-cli does not run pioasm during a sketch build, so GatedPulsePico.ino
carries the assembled words as C arrays. This re-assembles every .pio file in
the sketch directory and diffs the result against the matching
`<name>_insns[]` array, so the two cannot silently drift.

    python3 tools/pioverify.py <path-to-pioasm> <sketch-dir>
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


def assembled(pioasm: str, pio_file: pathlib.Path) -> list[str]:
    out = subprocess.run([pioasm, "-o", "c-sdk", str(pio_file)],
                         capture_output=True, text=True, check=True).stdout
    return re.findall(r"^\s+(0x[0-9a-f]{4}),", out, re.M)


def in_sketch(src: str, name: str) -> list[str] | None:
    m = re.search(name + r"_insns\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        return None
    return re.findall(r"0x[0-9a-f]{4}", m.group(1))


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 2
    pioasm, sketch_dir = argv[0], pathlib.Path(argv[1])
    ino = next(sketch_dir.glob("*.ino"), None)
    if ino is None:
        print(f"no .ino in {sketch_dir}")
        return 2
    src = ino.read_text()

    pio_files = sorted(sketch_dir.glob("*.pio"))
    if not pio_files:
        print(f"no .pio files in {sketch_dir}")
        return 2

    failures = 0
    for pf in pio_files:
        name = pf.stem
        ref = assembled(pioasm, pf)
        mine = in_sketch(src, name)
        if mine is None:
            print(f"  {name:14} SKIP   no {name}_insns[] in {ino.name}")
            continue
        if mine == ref:
            print(f"  {name:14} OK     {len(ref)} instructions")
            continue
        failures += 1
        print(f"  {name:14} DIFF   sketch={len(mine)} pioasm={len(ref)}")
        for i in range(max(len(mine), len(ref))):
            a = mine[i] if i < len(mine) else "----"
            b = ref[i] if i < len(ref) else "----"
            if a != b:
                print(f"      [{i:>2}] sketch {a}  !=  pioasm {b}")

    print("PIO verification:", "FAILED" if failures else "all programs match")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
