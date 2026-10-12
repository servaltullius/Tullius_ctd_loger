#!/usr/bin/env python3
"""Generate dump_tool/data/address_db/skyrimse_functions.json.

Names come from an MIT-licensed CommonLibSSE-NG revision (the RELOCATION_ID /
RelocationID / VariantID pairs and the function or constant they sit in).
Offsets come from the Address Library files of each game version. Only names
are taken from CommonLib; no Address Library file is shipped.

When the SkyrimSE.exe of a version is given (--exe), an ID is kept only if its
offset is the start of a primary .pdata entry there, so data addresses and
mid-function IDs drop out. IDs kept for that version are kept for the other
versions too (the same AE ID is the same function in every AE build).

Example:
  python scripts/generate_game_function_names.py \
    --commonlib path/to/CommonLibSSE-NG-e342462 \
    --bin 1.5.97.0=version-1-5-97-0.bin \
    --bin 1.6.640.0=versionlib-1-6-640-0.bin \
    --bin 1.6.1170.0=versionlib-1-6-1170-0.bin \
    --exe 1.6.1170.0=SkyrimSE.exe \
    --commonlib-ref alandtse/CommonLibSSE-NG@e34246283a75fdd7009108476f0c646038f71f3c
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import struct
import sys
from pathlib import Path

ID_PATTERN = re.compile(r"\b(?:RELOCATION_ID|RelocationID|VariantID)\s*\(\s*(\d+)\s*,\s*(\d+)")
SKIPPED_FILES = {"Offsets_RTTI.h", "Offsets_VTABLE.h", "Offsets_NiRTTI.h"}
DROPPED_SCOPES = {"RE", "Offset", "SKSE", "REL", "detail", "Impl"}
CONTROL_WORDS = {"if", "for", "while", "switch", "catch", "return", "else", "do", "sizeof", "alignof", "decltype"}
FUNC_HEADER = re.compile(
    r"([~A-Za-z_][\w:~]*(?:operator\s*\S+?)?)\s*\((?:[^()]|\([^()]*\))*\)"
    r"\s*(?:const)?\s*(?:noexcept(?:\([^)]*\))?)?\s*(?:override)?\s*(?:->\s*[^{;]+)?\s*$",
    re.S,
)
CONST_NAME = re.compile(r"\b(?:constexpr|inline|static|const)\b[^=;{}]*?\b([A-Za-z_]\w*)\s*(?:\{|=)\s*$", re.S)
TYPE_HEADER = re.compile(r"\b(namespace|class|struct|union)\s+(?:alignas\([^)]*\)\s*)?([A-Za-z_][\w:]*)?[^{;]*$", re.S)


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and string/char literals, keeping offsets and newlines."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * (j - i - 2) + c if j - i >= 2 else c)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_template_heads(header: str) -> str:
    """Drop 'template <...>' parameter lists, which name parameters like 'class T'."""
    out = []
    i = 0
    while True:
        m = re.search(r"\btemplate\s*<", header[i:])
        if not m:
            out.append(header[i:])
            return "".join(out)
        out.append(header[i:i + m.start()])
        j = i + m.end()
        depth = 1
        while j < len(header) and depth:
            depth += {"<": 1, ">": -1}.get(header[j], 0)
            j += 1
        i = j


def cut_initializer_list(header: str) -> str:
    """'X::X(args) : a_(b) ' -> 'X::X(args)' so the last member is not taken as the function."""
    depth = 0
    for i, ch in enumerate(header):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                m = re.match(r"\s*(?:const\s*)?(?:noexcept\s*)?:(?!:)", header[i + 1:])
                if m:
                    return header[: i + 1]
    return header


def scope_name(header: str) -> tuple[str, str]:
    """Classify the text before a '{' as ('func'|'type'|'other', name)."""
    header = cut_initializer_list(strip_template_heads(header)).strip()
    m = TYPE_HEADER.search(header)
    if m and not FUNC_HEADER.search(header):
        return ("type", m.group(2) or "")
    m = FUNC_HEADER.search(header)
    if m:
        name = m.group(1)
        last = name.split("::")[-1]
        if last not in CONTROL_WORDS and not header.rstrip().endswith("]"):
            return ("func", name)
    return ("other", "")


def qualified(scopes: list[tuple[str, str]], name: str) -> str:
    outer = [n for kind, n in scopes if kind == "type" and n and n not in DROPPED_SCOPES]
    inner = [p for p in name.split("::") if p and p not in DROPPED_SCOPES]
    # An inline member already sits in its class scope; an out-of-line one
    # ("Actor::AddSpell" inside "namespace RE") names it itself. Join where
    # the two overlap, which keeps constructors as "X::X".
    for k in range(min(len(outer), len(inner)), 0, -1):
        if outer[-k:] == inner[:k]:
            outer = outer[:-k]
            break
    return "::".join(outer + inner)


def is_template_parameter_name(name: str) -> bool:
    """'T::GetHandle' comes from a class template; its real class is unknown."""
    first = name.split("::")[0]
    return len(first) <= 2 and first[:1].isupper()


def extract_names(commonlib: Path) -> tuple[dict[int, collections.Counter], dict[int, collections.Counter]]:
    """AE ID -> Counter of candidate names, and AE ID -> Counter of paired SE IDs."""
    names: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
    se_for_ae: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
    roots = [commonlib / "include" / "RE", commonlib / "src" / "RE"]
    for root in roots:
        for path in sorted(root.rglob("*")):
            if path.suffix not in {".h", ".cpp", ".inl"} or path.name in SKIPPED_FILES:
                continue
            text = strip_comments_and_strings(path.read_text(encoding="utf-8", errors="replace"))
            occurrences = [(m.start(), int(m.group(1)), int(m.group(2))) for m in ID_PATTERN.finditer(text)]
            if not occurrences:
                continue
            scopes: list[tuple[str, str]] = []
            last_boundary = 0
            occ_index = 0
            for pos, ch in enumerate(text):
                while occ_index < len(occurrences) and occurrences[occ_index][0] == pos:
                    _, se_id, ae_id = occurrences[occ_index]
                    occ_index += 1
                    func = next((n for kind, n in reversed(scopes) if kind == "func"), None)
                    if func is not None:
                        func_depth = max(i for i, (kind, _) in enumerate(scopes) if kind == "func")
                        name = qualified(scopes[:func_depth], func)
                    else:
                        m = CONST_NAME.search(text[last_boundary:pos])
                        if not m:
                            continue
                        name = qualified(scopes, m.group(1))
                    if name and not is_template_parameter_name(name):
                        names[ae_id][name] += 1
                        se_for_ae[ae_id][se_id] += 1
                if ch == "{":
                    scopes.append(scope_name(text[last_boundary:pos]))
                    last_boundary = pos + 1
                elif ch == "}":
                    if scopes:
                        scopes.pop()
                    last_boundary = pos + 1
                elif ch == ";":
                    last_boundary = pos + 1
    return names, se_for_ae


def read_address_library(path: Path) -> dict[int, int]:
    data = path.read_bytes()
    pos = 0

    def take(fmt: str) -> int:
        nonlocal pos
        size = struct.calcsize(fmt)
        (value,) = struct.unpack_from(fmt, data, pos)
        pos += size
        return value

    fmt_version = take("<i")
    if fmt_version not in (1, 2):
        raise ValueError(f"{path}: unsupported Address Library format {fmt_version}")
    pos += 16  # version[4]
    name_len = take("<i")
    pos += name_len
    pointer_size = take("<i")
    count = take("<i")
    result: dict[int, int] = {}
    prev_id = prev_off = 0
    small = {2: "<B", 3: "<B", 4: "<H", 5: "<H"}
    for _ in range(count):
        tag = data[pos]
        pos += 1
        lo, hi = tag & 0xF, tag >> 4
        if lo == 0:
            id_ = take("<Q")
        elif lo == 1:
            id_ = prev_id + 1
        elif lo in small:
            delta = take(small[lo])
            id_ = prev_id + delta if lo in (2, 4) else prev_id - delta
        elif lo == 6:
            id_ = take("<H")
        elif lo == 7:
            id_ = take("<I")
        else:
            raise ValueError(f"{path}: bad id tag {tag:#x}")
        base = prev_off // pointer_size if hi & 8 else prev_off
        kind = hi & 7
        if kind == 0:
            off = take("<Q")
        elif kind == 1:
            off = base + 1
        elif kind in small:
            delta = take(small[kind])
            off = base + delta if kind in (2, 4) else base - delta
        elif kind == 6:
            off = take("<H")
        else:
            off = take("<I")
        if hi & 8:
            off *= pointer_size
        result[id_] = off
        prev_id, prev_off = id_, off
    if pos != len(data):
        raise ValueError(f"{path}: {len(data) - pos} trailing bytes")
    return result


def primary_function_starts(exe: Path) -> set[int]:
    """RVAs that start a primary (non-chained) .pdata entry of a PE32+ file."""
    data = exe.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    num_sections = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise ValueError(f"{exe}: not a PE32+ image")
    exc_rva, exc_size = struct.unpack_from("<II", data, opt + 112 + 3 * 8)
    sections = []
    for i in range(num_sections):
        s = pe + 24 + opt_size + i * 40
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((vaddr, max(vsize, raw_size), raw_ptr, raw_size))

    def file_offset(rva: int) -> int | None:
        for vaddr, size, raw_ptr, raw_size in sections:
            if vaddr <= rva < vaddr + size and rva - vaddr < raw_size:
                return raw_ptr + (rva - vaddr)
        return None

    table = file_offset(exc_rva)
    if table is None:
        raise ValueError(f"{exe}: no exception directory")
    starts: set[int] = set()
    for i in range(exc_size // 12):
        begin, _end, unwind = struct.unpack_from("<III", data, table + i * 12)
        if begin == 0 or unwind & 1:
            continue
        info = file_offset(unwind)
        if info is None:
            continue
        flags = data[info] >> 3
        if flags & 0x4:  # UNW_FLAG_CHAININFO: a fragment of another function
            continue
        starts.add(begin)
    return starts


def read_license(commonlib: Path) -> list[str]:
    """The name source's license text, kept with the names it covers."""
    for candidate in ("LICENSE", "LICENSE.txt", "LICENSE.md"):
        path = commonlib / candidate
        if path.is_file():
            text = path.read_text(encoding="utf-8")
            if "MIT License" not in text:
                raise SystemExit(f"{path}: names must come from an MIT-licensed revision")
            return text.strip().splitlines()
    raise SystemExit(f"{commonlib}: no LICENSE file")


def pick(counter: collections.Counter):
    """Most frequent; then a class-qualified name over a free helper; then by name."""
    return sorted(counter.items(), key=lambda kv: (-kv[1], "::" not in str(kv[0]), str(kv[0])))[0][0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--commonlib", type=Path, required=True)
    parser.add_argument("--commonlib-ref", required=True, help="repo@commit recorded in the output")
    parser.add_argument("--bin", action="append", default=[], help="GAMEVERSION=path (SE files take SE IDs)")
    parser.add_argument("--exe", action="append", default=[], help="GAMEVERSION=path to that SkyrimSE.exe")
    parser.add_argument(
        "--out", type=Path,
        default=Path(__file__).resolve().parents[1] / "dump_tool" / "data" / "address_db" / "skyrimse_functions.json")
    args = parser.parse_args()

    names, se_for_ae = extract_names(args.commonlib)
    # An ID that CommonLib uses under two different names is left out: no
    # name is better than a wrong one.
    conflicts = {ae: c for ae, c in names.items() if len(c) > 1}
    chosen = {ae: pick(c) for ae, c in names.items() if ae not in conflicts}
    se_ids = {ae: pick(c) for ae, c in se_for_ae.items()}

    libraries: dict[str, dict[int, int]] = {}
    for spec in args.bin:
        version, _, path = spec.partition("=")
        libraries[version] = read_address_library(Path(path))

    verified: set[int] | None = None
    for spec in args.exe:
        version, _, path = spec.partition("=")
        if version not in libraries:
            raise SystemExit(f"--exe {version} has no matching --bin")
        starts = primary_function_starts(Path(path))
        ok = {ae for ae in chosen if libraries[version].get(ae) in starts}
        verified = ok if verified is None else verified & ok
        print(f"{version}: {len(ok)} of {len(chosen)} named IDs are function starts", file=sys.stderr)
    kept = verified if verified is not None else set(chosen)

    game_versions: dict[str, dict] = {}
    for version, library in sorted(libraries.items()):
        is_se = version.startswith("1.5.")
        functions: dict[int, str] = {}
        for ae in sorted(kept):
            game_id = se_ids.get(ae) if is_se else ae
            if game_id is None or game_id not in library:
                continue
            rva = library[game_id]
            # Two IDs at one address: keep the first name deterministically.
            functions.setdefault(rva, chosen[ae])
        game_versions[version] = {
            "functions": {f"0x{rva:X}": name for rva, name in sorted(functions.items())}
        }
        print(f"{version}: {len(functions)} functions", file=sys.stderr)

    out = {
        "version": 2,
        "description": (
            "Function-start offsets of SkyrimSE.exe with names. Generated by "
            "scripts/generate_game_function_names.py; do not edit by hand. Names: "
            f"{args.commonlib_ref} (MIT, license text in name_license). Offsets: Address Library "
            "for SKSE Plugins by meh321 (Nexus mod 32444); only derived offsets are included. "
            "An entry matches only the exact start of the .pdata function containing a frame."
        ),
        "name_source": args.commonlib_ref,
        "name_license": read_license(args.commonlib),
        "game_versions": game_versions,
    }
    args.out.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(conflicts)} IDs left out for having more than one candidate name", file=sys.stderr)
    for ae, c in sorted(conflicts.items()):
        print(f"  {ae}: {dict(c)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
