"""Checks that a built mod dll only imports UE4SS.dll symbols every supported UE4SS build exports.

The mod is compiled against one UE4SS source tree but has to load under two different UE4SS.dll
builds: the Steam Workshop item "UE4SS Experimental (Palworld)" and the GitHub/Nexus release of
Okaetsu's RE-UE4SS. A mod dll that imports a symbol one of them lacks fails to load there (the
loader reports a missing procedure, the mod simply never starts), so every release is checked here
against the export list of each supported build.

Usage:
    python tools/Check-Ue4ssAbi.py <mod dll> <exports.txt or UE4SS.dll>...
    python tools/Check-Ue4ssAbi.py --dump <UE4SS.dll> > tools/ue4ss-exports/<name>.txt

Exit code 0 when every import is present in every export list, 1 otherwise. Pure Python, no
third-party modules, so it runs from deploy.ps1 on any machine.
"""

import struct
import sys
from pathlib import Path


def _load(path: Path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories_offset = optional + (112 if magic == 0x20B else 96)
    directories = [struct.unpack_from("<II", data, directories_offset + 8 * i) for i in range(16)]
    sections = []
    sections_offset = optional + optional_size
    for i in range(section_count):
        header = sections_offset + 40 * i
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, header + 8)
        sections.append((virtual_address, virtual_size, raw_size, raw_offset))

    def to_offset(rva: int) -> int:
        for virtual_address, virtual_size, raw_size, raw_offset in sections:
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return rva - virtual_address + raw_offset
        raise ValueError(f"RVA {rva:#x} is outside every section")

    def c_string(offset: int) -> str:
        end = data.index(b"\0", offset)
        return data[offset:end].decode("latin1")

    return data, directories, to_offset, c_string


def imports_from(path: Path) -> dict[str, list[str]]:
    data, directories, to_offset, c_string = _load(path)
    rva, _ = directories[1]
    result: dict[str, list[str]] = {}
    offset = to_offset(rva)
    while True:
        lookup, _, _, name_rva, address_table = struct.unpack_from("<IIIII", data, offset)
        if name_rva == 0:
            break
        names = []
        entry = to_offset(lookup or address_table)
        while True:
            value = struct.unpack_from("<Q", data, entry)[0]
            if value == 0:
                break
            names.append(f"ordinal {value & 0xFFFF}" if value >> 63 else c_string(to_offset(value & 0x7FFFFFFF) + 2))
            entry += 8
        result[c_string(to_offset(name_rva)).lower()] = names
        offset += 20
    return result


def exports_from(path: Path) -> set[str]:
    data, directories, to_offset, c_string = _load(path)
    rva, _ = directories[0]
    offset = to_offset(rva)
    name_count = struct.unpack_from("<I", data, offset + 24)[0]
    names_rva = struct.unpack_from("<I", data, offset + 32)[0]
    table = to_offset(names_rva)
    return {c_string(to_offset(struct.unpack_from("<I", data, table + 4 * i)[0])) for i in range(name_count)}


def export_list(path: Path) -> set[str]:
    if path.suffix.lower() == ".dll":
        return exports_from(path)
    return {line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip() and not line.startswith("#")}


def main(argv: list[str]) -> int:
    if len(argv) >= 3 and argv[1] == "--dump":
        for name in sorted(exports_from(Path(argv[2]))):
            print(name)
        return 0
    if len(argv) < 3:
        print(__doc__)
        return 2
    mod = Path(argv[1])
    needed = imports_from(mod).get("ue4ss.dll", [])
    if not needed:
        print(f"{mod}: no imports from UE4SS.dll found; is this a UE4SS C++ mod?")
        return 1
    failed = False
    for target in argv[2:]:
        exports = export_list(Path(target))
        missing = [name for name in needed if name not in exports]
        state = "OK" if not missing else "MISSING"
        print(f"{state}: {Path(target).name}: {len(needed)} imports, {len(exports)} exports, {len(missing)} missing")
        for name in missing:
            print(f"    {name}")
        failed = failed or bool(missing)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
