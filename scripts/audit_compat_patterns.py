#!/usr/bin/env python3
"""Offline compatibility audit for ZZZ native-touch locators.

Usage:
  python scripts/audit_compat_patterns.py UnityPlayer.dll GameAssembly.dll

The script does not modify files. It checks whether the relocatable runtime
patterns still resolve uniquely and prints the derived RVAs/field offsets.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Section:
    name: str
    va: int
    vsize: int
    raw: int
    raw_size: int


class PE:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        e_lfanew = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise ValueError(f"{path}: bad PE signature")
        nsects = struct.unpack_from("<H", self.data, e_lfanew + 6)[0]
        opt_size = struct.unpack_from("<H", self.data, e_lfanew + 20)[0]
        sec_off = e_lfanew + 24 + opt_size
        self.sections: dict[str, Section] = {}
        for i in range(nsects):
            off = sec_off + i * 40
            name = self.data[off:off + 8].split(b"\0", 1)[0].decode("ascii", "ignore")
            vsize, va, raw_size, raw = struct.unpack_from("<IIII", self.data, off + 8)
            self.sections[name] = Section(name, va, vsize, raw, raw_size)

    def section_bytes(self, name: str) -> tuple[bytes, int]:
        s = self.sections[name]
        return self.data[s.raw:s.raw + s.raw_size], s.va

    def rva_bytes(self, rva: int, size: int) -> bytes:
        for s in self.sections.values():
            if s.va <= rva < s.va + max(s.vsize, s.raw_size):
                off = s.raw + (rva - s.va)
                return self.data[off:off + size]
        raise ValueError(f"RVA 0x{rva:X} not mapped")


def compile_pattern(text: str) -> tuple[bytes, list[bool]]:
    bs = bytearray()
    mask: list[bool] = []
    for tok in text.split():
        if tok in {"?", "??"}:
            bs.append(0)
            mask.append(False)
        else:
            bs.append(int(tok, 16))
            mask.append(True)
    return bytes(bs), mask


def find_pattern(data: bytes, text: str) -> list[int]:
    pat, mask = compile_pattern(text)
    first = next(i for i, m in enumerate(mask) if m)
    needle = bytes([pat[first]])
    out: list[int] = []
    pos = 0
    while True:
        hit = data.find(needle, pos)
        if hit < 0:
            break
        start = hit - first
        if start >= 0 and start + len(pat) <= len(data):
            if all((not mask[i]) or data[start + i] == pat[i] for i in range(len(pat))):
                out.append(start)
        pos = hit + 1
    return out


def i32(b: bytes, off: int) -> int:
    return struct.unpack_from("<i", b, off)[0]


def u32(b: bytes, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def audit_unity(pe: PE) -> bool:
    pattern = (
        "45 33 FF 44 38 B8 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? "
        "B9 D8 00 00 00 E8 ?? ?? ?? ?? 48 85 C0"
    )
    data, base = pe.section_bytes(".text")
    hits = find_pattern(data, pattern)
    print(f"Unity input locator matches: {len(hits)}")
    if len(hits) != 1:
        return False
    hit = base + hits[0]
    target = hit + 3
    code = pe.rva_bytes(target, 13)
    field = u32(code, 3)
    branch = target + 13 + i32(code, 9)

    legacy_candidates: list[int] = []
    for rva in range(target - 0x60, target - 7):
        b = pe.rva_bytes(rva, 8)
        if b[:3] == b"\x48\x83\x3d" and b[7] == 0:
            legacy_candidates.append(rva + 8 + i32(b, 3))

    new_candidates: list[int] = []
    for rva in range(target + 13, branch - 6):
        b = pe.rva_bytes(rva, 7)
        if b[:3] == b"\x48\x89\x05":
            new_candidates.append(rva + 7 + i32(b, 3))

    callsite = target - 8
    call = pe.rva_bytes(callsite, 5)
    getter = callsite + 5 + i32(call, 1)
    g = pe.rva_bytes(getter, 7)
    inner = getter + 7 + i32(g, 3)
    lea = pe.rva_bytes(inner + 3, 7)
    settings_global = inner + 10 + i32(lea, 3)

    print(f"  patch RVA:             0x{target:X}")
    print(f"  PlayerSettings field:  0x{field:X} / 0x{field + 1:X}")
    print(f"  legacy branch:         0x{branch:X}")
    print(f"  legacy global:         {[hex(x) for x in legacy_candidates]}")
    print(f"  new global:            {[hex(x) for x in new_candidates]}")
    print(f"  PlayerSettings global: 0x{settings_global:X}")
    return len(legacy_candidates) == 1 and len(new_candidates) == 1


def audit_gameassembly(pe: PE) -> bool:
    data, base = pe.section_bytes("il2cpp")
    layout_pattern = (
        "E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? "
        "8B 40 ?? 85 C0 0F 4F F0 89 F0 48 83 C4 20 5B 5F 5E C3"
    )
    save_pattern = (
        "48 89 45 E8 E8 ?? ?? ?? ?? 48 8B 4D E8 48 8B 51 20 "
        "49 89 C0 E8 ?? ?? ?? ?? 48 8B 75 E8 C6 46 41 00"
    )

    layouts = find_pattern(data, layout_pattern)
    saves = find_pattern(data, save_pattern)
    print(f"GameAssembly layout locator matches: {len(layouts)}")
    print(f"GameAssembly save locator matches:   {len(saves)}")
    if len(layouts) != 1 or len(saves) != 1:
        return False

    layout_hit = base + layouts[0]
    layout_target = layout_hit + 14
    field = pe.rva_bytes(layout_target, 3)[2]

    save_context = base + saves[0]
    callsite = save_context + 4
    call = pe.rva_bytes(callsite, 5)
    serializer = callsite + 5 + i32(call, 1)

    print(f"  layout apply RVA:      0x{layout_target:X}")
    print(f"  layout field offset:   0x{field:X}")
    print(f"  save callsite RVA:     0x{callsite:X}")
    print(f"  serializer RVA:        0x{serializer:X}")
    print(f"  serializer return RVA: 0x{callsite + 5:X}")
    return True


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip())
        return 2
    unity = PE(Path(sys.argv[1]))
    ga = PE(Path(sys.argv[2]))
    ok_u = audit_unity(unity)
    ok_g = audit_gameassembly(ga)
    print("RESULT:", "COMPATIBLE_CANDIDATE" if ok_u and ok_g else "RE_AUDIT_REQUIRED")
    return 0 if ok_u and ok_g else 1


if __name__ == "__main__":
    raise SystemExit(main())
