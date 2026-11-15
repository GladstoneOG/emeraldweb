#!/usr/bin/env python3
"""Convert an assembled ARM ELF32 relocatable object (.o) into portable C.

Handles `pokeemerald-expansion` assembly data units (`data/*.s`, `sound/songs/*.s`)
assembled with `arm-none-eabi-as`. Preserves section byte payloads, exported symbols,
internal aliases, and `R_ARM_ABS32` relocations.

Special handling for `pokeemerald-expansion`:
  - `callnative`, `gotonative`, `data/script_cmd_table.inc`, and `data/specials.inc`
    add `ROM_SIZE` (`0x02000000`) to mark `requests_effects=1` in GBA ROM mirror 1.
  - On WASM, shifting `call_indirect` table function pointers by `+0x02000000`
    would corrupt function calls. This translator strips exact `0x02000000` mirror
    tags from pointer relocations and registers the clean function pointer into
    `WasmScriptEffects_RegisterTable()` at startup.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path
from typing import Dict, List, Tuple

ELF_MAGIC = b"\x7fELF"
ELFCLASS32 = 1
ELFDATA2LSB = 1
ET_REL = 1
EM_ARM = 40

SHT_NULL = 0
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9

STB_LOCAL = 0
STB_GLOBAL = 1
STB_WEAK = 2

STT_NOTYPE = 0
STT_OBJECT = 1
STT_FUNC = 2
STT_SECTION = 3
STT_FILE = 4

SHN_UNDEF = 0
SHN_ABS = 0xFFF1
SHN_COMMON = 0xFFF2

R_ARM_NONE = 0
R_ARM_ABS32 = 2
R_ARM_REL32 = 3
R_ARM_ABS16 = 5
R_ARM_TARGET1 = 38

EXPANSION_ROM_SIZE_TAG = 0x02000000


def sanitize_ident(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def read_cstr(blob: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(blob):
        return ""
    end = blob.find(b"\x00", offset)
    if end == -1:
        end = len(blob)
    return blob[offset:end].decode("utf-8", errors="replace")


def parse_elf32(path: Path) -> Tuple[List[dict], List[dict], bytes]:
    raw = path.read_bytes()
    if len(raw) < 52 or raw[:4] != ELF_MAGIC:
        raise ValueError(f"{path}: Not a valid ELF binary")
    if raw[4] != ELFCLASS32 or raw[5] != ELFDATA2LSB:
        raise ValueError(f"{path}: Expected 32-bit little-endian ELF")

    e_type, e_machine, _e_ver, _e_entry, _e_phoff, e_shoff, _e_flags, _e_ehsize, _e_phentsize, _e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(
        "<HHIIIIIHHHHHH", raw, 16
    )
    if e_type != ET_REL or e_machine != EM_ARM:
        raise ValueError(f"{path}: Expected ARM relocatable ET_REL object (got type={e_type}, machine={e_machine})")

    sections: List[dict] = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack_from(
            "<IIIIIIIIII", raw, off
        )
        data = b"" if sh_type == SHT_NOBITS else raw[sh_offset : sh_offset + sh_size]
        sections.append(
            {
                "index": i,
                "name_off": sh_name,
                "name": "",
                "type": sh_type,
                "flags": sh_flags,
                "offset": sh_offset,
                "size": sh_size,
                "link": sh_link,
                "info": sh_info,
                "align": max(1, sh_addralign),
                "entsize": sh_entsize,
                "data": bytearray(data),
            }
        )

    shstr = bytes(sections[e_shstrndx]["data"]) if e_shstrndx < len(sections) else b""
    for sec in sections:
        sec["name"] = read_cstr(shstr, sec["name_off"])

    symbols: List[dict] = []
    for sec in sections:
        if sec["type"] == SHT_SYMTAB:
            strtab = bytes(sections[sec["link"]]["data"])
            entsize = sec["entsize"] or 16
            count = sec["size"] // entsize
            for idx in range(count):
                st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                    "<IIIBBH", sec["data"], idx * entsize
                )
                symbols.append(
                    {
                        "index": idx,
                        "name": read_cstr(strtab, st_name),
                        "value": st_value,
                        "size": st_size,
                        "bind": st_info >> 4,
                        "type": st_info & 0xF,
                        "shndx": st_shndx,
                    }
                )
            break

    return sections, symbols, raw


def collect_abs_symbols(elf_paths: List[Path]) -> Dict[str, int]:
    abs_map: Dict[str, int] = {}
    for p in elf_paths:
        _, symbols, _ = parse_elf32(p)
        for sym in symbols:
            if sym["bind"] in (STB_GLOBAL, STB_WEAK) and sym["shndx"] == SHN_ABS and sym["name"]:
                abs_map.setdefault(sym["name"], sym["value"])
    return abs_map


def convert_elf_to_c(
    elf_path: Path,
    unit_name: str,
    abs_symbols: Dict[str, int] | None = None,
    func_symbols: set[str] | None = None,
) -> str:
    sections, symbols, _ = parse_elf32(elf_path)
    abs_symbols = abs_symbols or {}
    func_symbols = func_symbols or set()

    # Select non-empty PROGBITS/NOBITS sections that hold data or targets
    kept_secs: Dict[int, dict] = {}
    for sec in sections:
        if sec["type"] in (SHT_PROGBITS, SHT_NOBITS) and sec["size"] > 0:
            if sec["name"].startswith((".debug", ".ARM.", ".comment", ".note")):
                continue
            kept_secs[sec["index"]] = sec

    # Assign C buffer identifier to each kept section
    sec_cnames: Dict[int, str] = {}
    for idx, sec in kept_secs.items():
        sec_cnames[idx] = f"sElfSec_{sanitize_ident(unit_name)}_{idx}"

    # Collect relocations referencing kept sections
    relocs: List[dict] = []
    extern_syms: set[str] = set()
    effect_funcs: List[str] = []

    for sec in sections:
        if sec["type"] not in (SHT_REL, SHT_RELA):
            continue
        target_idx = sec["info"]
        if target_idx not in kept_secs:
            continue
        target_sec = kept_secs[target_idx]
        is_rela = sec["type"] == SHT_RELA
        entsize = sec["entsize"] or (12 if is_rela else 8)
        count = sec["size"] // entsize
        for i in range(count):
            if is_rela:
                r_offset, r_info, r_addend = struct.unpack_from("<IIi", sec["data"], i * entsize)
            else:
                r_offset, r_info = struct.unpack_from("<II", sec["data"], i * entsize)
                if (r_info & 0xFF) == R_ARM_ABS16 and r_offset + 2 <= len(target_sec["data"]):
                    r_addend = struct.unpack_from("<h", target_sec["data"], r_offset)[0]
                elif r_offset + 4 <= len(target_sec["data"]):
                    r_addend = struct.unpack_from("<i", target_sec["data"], r_offset)[0]
                else:
                    r_addend = 0

            r_type = r_info & 0xFF
            r_sym = r_info >> 8
            if r_type == R_ARM_NONE:
                continue
            if r_type not in (R_ARM_ABS32, R_ARM_TARGET1, R_ARM_REL32, R_ARM_ABS16):
                raise ValueError(f"{elf_path}: Unsupported ARM relocation type {r_type} in {sec['name']}")

            sym = symbols[r_sym]
            is_effect_tagged = False
            if r_addend >= EXPANSION_ROM_SIZE_TAG and r_addend < (EXPANSION_ROM_SIZE_TAG * 2):
                r_addend -= EXPANSION_ROM_SIZE_TAG
                is_effect_tagged = True

            if sym["shndx"] == SHN_UNDEF:
                sym_name = sym["name"]
                if sym_name in abs_symbols:
                    target_expr = f"((uintptr_t)0x{abs_symbols[sym_name]:x}u)"
                else:
                    extern_syms.add(sym_name)
                    target_expr = f"((uintptr_t)&{sym_name})"
                    if is_effect_tagged and sym_name not in effect_funcs and r_addend == 0:
                        effect_funcs.append(sym_name)
            elif sym["shndx"] in kept_secs:
                sym_sec_name = sec_cnames[sym["shndx"]]
                base_off = sym["value"]
                target_expr = f"((uintptr_t)&{sym_sec_name}[{base_off}])"
            elif sym["shndx"] == SHN_ABS:
                target_expr = f"((uintptr_t)0x{sym['value']:x}u)"
            else:
                raise ValueError(f"{elf_path}: Relocation targets unhandled section index {sym['shndx']} ({sym['name']})")

            # Zero inline addend in emitted array bytes when SHT_REL patched cleanly at init
            if r_type == R_ARM_ABS16:
                if r_offset + 2 <= len(target_sec["data"]):
                    struct.pack_into("<H", target_sec["data"], r_offset, 0)
            elif r_offset + 4 <= len(target_sec["data"]):
                struct.pack_into("<I", target_sec["data"], r_offset, 0)

            relocs.append(
                {
                    "sec_idx": target_idx,
                    "offset": r_offset,
                    "type": r_type,
                    "expr": target_expr,
                    "addend": r_addend,
                }
            )

    lines: List[str] = []
    lines.append(f"/* Auto-generated by engine/tools/wasm_elf_to_c.py from {elf_path.name} */")
    lines.append("#include <stdint.h>")
    lines.append("#include <stddef.h>")
    lines.append("")
    if effect_funcs:
        lines.append("void WasmScriptEffects_RegisterTable(const void *const *funcs, size_t count);")
        lines.append("")

    for ext in sorted(extern_syms):
        if ext in func_symbols:
            lines.append(f"extern void {ext}(void);")
        else:
            lines.append(f"extern uint8_t {ext}[];")
    if extern_syms:
        lines.append("")

    # Emit mutable buffers (mutable so runtime constructor can apply relocations without .rodata trap)
    for idx, sec in kept_secs.items():
        cname = sec_cnames[idx]
        align = max(4, sec["align"])
        lines.append(f"__attribute__((aligned({align}), used)) uint8_t {cname}[{sec['size']}] = {{")
        if sec["type"] != SHT_NOBITS and len(sec["data"]) > 0:
            data = sec["data"]
            for row_start in range(0, len(data), 16):
                chunk = data[row_start : row_start + 16]
                hex_bytes = ", ".join(f"0x{b:02x}" for b in chunk)
                lines.append(f"    {hex_bytes},")
        else:
            lines.append("    0")
        lines.append("};")
        lines.append("")

    # Emit global symbol aliases pointing into kept sections (supports offset 0 AND non-zero offsets)
    emitted_aliases: set[str] = set()
    for sym in symbols:
        if sym["bind"] in (STB_GLOBAL, STB_WEAK) and sym["shndx"] in kept_secs:
            name = sym["name"]
            if not name or name in emitted_aliases:
                continue
            emitted_aliases.add(name)
            sec_cname = sec_cnames[sym["shndx"]]
            off = sym["value"]
            bind_dir = ".weak" if sym["bind"] == STB_WEAK else ".globl"
            if off == 0:
                lines.append(f"__asm__(\"{bind_dir} {name}\\n.set {name}, {sec_cname}\\n\");")
            else:
                lines.append(f"__asm__(\"{bind_dir} {name}\\n.set {name}, {sec_cname} + {off}\\n\");")

    # Emit symbol+offset wrapper pointers cleanly for external C references (`extern const u8 gEventScript_...[]`)
    # NOTE: In GNU C / Clang, global array aliases only bind offset 0 unless emitted as individual arrays or
    # split via -fdata-sections / sub-arrays. Splitting per-global-symbol slice in phase 1 ensures
    # `extern const u8 Foo[];` resolves cleanly at link time for non-zero offsets!
    lines.append("")
    if effect_funcs:
        lines.append(f"static const void *const sEffectTable_{sanitize_ident(unit_name)}[] = {{")
        for fn in effect_funcs:
            lines.append(f"    (const void *)&{fn},")
        lines.append("};")
        lines.append("")

    init_fn = f"WasmInitDataRelocs_{sanitize_ident(unit_name)}"
    lines.append(f"__attribute__((constructor)) void {init_fn}(void)")
    lines.append("{")
    for rel in relocs:
        sec_cname = sec_cnames[rel["sec_idx"]]
        off = rel["offset"]
        add = rel["addend"]
        add_str = f" + (intptr_t)({add})" if add != 0 else ""
        if rel["type"] == R_ARM_REL32:
            lines.append(
                f"    {{ uint32_t val = (uint32_t)({rel['expr']}{add_str} - (uintptr_t)&{sec_cname}[{off}]); "
                f"__builtin_memcpy(&{sec_cname}[{off}], &val, 4); }}"
            )
        elif rel["type"] == R_ARM_ABS16:
            lines.append(
                f"    {{ uint16_t val = (uint16_t)({rel['expr']}{add_str}); "
                f"__builtin_memcpy(&{sec_cname}[{off}], &val, 2); }}"
            )
        else:
            lines.append(
                f"    {{ uint32_t val = (uint32_t)({rel['expr']}{add_str}); "
                f"__builtin_memcpy(&{sec_cname}[{off}], &val, 4); }}"
            )
    if effect_funcs:
        tbl = f"sEffectTable_{sanitize_ident(unit_name)}"
        lines.append(f"    WasmScriptEffects_RegisterTable({tbl}, sizeof({tbl}) / sizeof({tbl}[0]));")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Translate ARM ELF32 .o data units to portable WASM C.")
    parser.add_argument("input", help="Input ARM ELF32 .o file")
    parser.add_argument("output", help="Output .c file")
    parser.add_argument("--unit", default="", help="Optional C identifier suffix for generated unit")
    parser.add_argument("--abs-from", nargs="*", default=[], help="Optional ARM ELF .o files providing SHN_ABS constants")
    parser.add_argument("--funcs-file", default="", help="Optional newline-separated file of C function symbol names")
    args = parser.parse_args(argv)

    in_path = Path(args.input)
    out_path = Path(args.output)
    unit = args.unit or in_path.stem
    abs_map = collect_abs_symbols([Path(p) for p in args.abs_from]) if args.abs_from else {}
    func_set: set[str] = set()
    if args.funcs_file:
        func_set = {line.strip() for line in Path(args.funcs_file).read_text(encoding="utf-8").splitlines() if line.strip()}
    c_src = convert_elf_to_c(in_path, unit, abs_map, func_set)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(c_src, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
