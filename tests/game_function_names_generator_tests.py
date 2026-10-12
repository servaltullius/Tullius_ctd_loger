"""Tests for scripts/generate_game_function_names.py.

The generated address DB is only as good as the names it pulls out of
CommonLib and the offsets it reads from Address Library files, so both are
checked on small synthetic inputs: names must come from the function (or
constant) that holds the RELOCATION_ID, IDs that CommonLib uses under two names
must be left out, and the compressed .bin format must decode exactly.
"""

import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "generate_game_function_names.py"

spec = importlib.util.spec_from_file_location("generate_game_function_names", SCRIPT)
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_commonlib(root: Path) -> None:
    write(root / "LICENSE", "MIT License\n\nCopyright (c) 2018 Example\n")
    write(root / "include" / "RE" / "Offsets.h", """
namespace RE
{
	namespace Offset
	{
		namespace BSReadWriteLock
		{
			constexpr auto LockForRead = RELOCATION_ID(66976, 68233);  // a comment
		}
	}
}
""")
    write(root / "include" / "RE" / "Offsets_RTTI.h", """
namespace RE { inline constexpr REL::VariantID RTTI_Actor(1, 2, 3); constexpr auto Skipped = RELOCATION_ID(5, 6); }
""")
    write(root / "include" / "RE" / "H" / "HUDMenu.h", """
namespace RE
{
	class HUDMenu : public IMenu
	{
	public:
		static void FlashMeter(ActorValue a_value)
		{
			using func_t = decltype(&HUDMenu::FlashMeter);
			static REL::Relocation<func_t> func{ RELOCATION_ID(51907, 52845) };
			return func(a_value);
		}
	};

	template <class T, class Manager = void>
	class BSPointerHandleManagerInterface
	{
	public:
		static bool GetSmartPointer(const T& a_handle)
		{
			static REL::Relocation<void*> func{ RELOCATION_ID(12204, 12332) };
			return func(a_handle);
		}
	};
}
""")
    write(root / "src" / "RE" / "T" / "TESObjectREFR.cpp", """
#include "RE/T/TESObjectREFR.h"
namespace RE
{
	const char* TESObjectREFR::GetDisplayFullName()
	{
		const char* text = "{ not a scope }";
		static REL::Relocation<func_t> func{ RELOCATION_ID(19354, 19781) };
		return func(this);
	}

	hkStringPtr::hkStringPtr(const char* a_data) :
		_data(nullptr)
	{
		static REL::Relocation<func_t> func{ RELOCATION_ID(56801, 57231) };
		func(*this, a_data);
	}

	void FlashHUDMeter(ActorValue a_value)
	{
		static REL::Relocation<func_t> func{ RELOCATION_ID(51907, 52845) };
		return func(a_value);
	}
}
""")


def make_bin(path: Path, fmt_version: int, mapping: dict[int, int]) -> None:
    name = b"SkyrimSE.exe"
    out = bytearray(struct.pack("<i4ii", fmt_version, 1, 6, 1170, 0, len(name)))
    out += name
    out += struct.pack("<ii", 8, len(mapping))
    prev_id = prev_off = 0
    for id_, off in sorted(mapping.items()):
        if id_ == prev_id + 1 and off == prev_off + 0x10:
            out += bytes([0x21]) + struct.pack("<B", 0x10)  # id +1, offset +u8
        else:
            out += bytes([0x00]) + struct.pack("<QQ", id_, off)  # both absolute
        prev_id, prev_off = id_, off
    path.write_bytes(bytes(out))


def test_names_come_from_the_enclosing_function_or_constant():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        make_commonlib(root)
        names, se_for_ae = gen.extract_names(root)
        picked = {ae: dict(c) for ae, c in names.items()}
        assert picked[68233] == {"BSReadWriteLock::LockForRead": 1}, picked
        assert picked[19781] == {"TESObjectREFR::GetDisplayFullName": 1}, picked
        # A constructor keeps its doubled name; its initializer list is not the function.
        assert picked[57231] == {"hkStringPtr::hkStringPtr": 1}, picked
        # A class template's own name, not its parameter "T".
        assert picked[12332] == {"BSPointerHandleManagerInterface::GetSmartPointer": 1}, picked
        assert picked[52845] == {"HUDMenu::FlashMeter": 1, "FlashHUDMeter": 1}, picked
        assert 6 not in picked, "RTTI/VTABLE offset files are skipped"
        assert dict(se_for_ae[68233]) == {66976: 1}


def test_address_library_decoding():
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "versionlib.bin"
        mapping = {68233: 0xCC90C0, 68234: 0xCC90D0, 19781: 0x2EA340, 400000: 0x3000000}
        make_bin(path, 2, mapping)
        assert gen.read_address_library(path) == mapping


def test_generated_table_leaves_out_ambiguous_ids():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        make_commonlib(root / "commonlib")
        make_bin(root / "ae.bin", 2, {68233: 0xCC90C0, 19781: 0x2EA340, 52845: 0x900000, 57231: 0x910000, 12332: 0x920000})
        make_bin(root / "se.bin", 1, {66976: 0xC077B0, 19354: 0x2D0000, 51907: 0x880000, 56801: 0x890000, 12204: 0x8A0000})
        out = root / "out.json"
        subprocess.run(
            [sys.executable, str(SCRIPT), "--commonlib", str(root / "commonlib"),
             "--commonlib-ref", "example/CommonLib@abc",
             "--bin", f"1.6.1170.0={root / 'ae.bin'}", "--bin", f"1.5.97.0={root / 'se.bin'}",
             "--out", str(out)],
            check=True, capture_output=True, text=True)
        data = json.loads(out.read_text(encoding="utf-8"))
        ae = data["game_versions"]["1.6.1170.0"]["functions"]
        se = data["game_versions"]["1.5.97.0"]["functions"]
        assert ae["0xCC90C0"] == "BSReadWriteLock::LockForRead", ae
        assert se["0xC077B0"] == "BSReadWriteLock::LockForRead", se
        assert "0x900000" not in ae and "0x880000" not in se, "an ID with two names gets none"
        assert data["name_source"] == "example/CommonLib@abc"


def test_checked_in_table_matches_its_source():
    data = json.loads((REPO_ROOT / "dump_tool" / "data" / "address_db" / "skyrimse_functions.json").read_text(encoding="utf-8"))
    assert data["name_source"] == "alandtse/CommonLibSSE-NG@e34246283a75fdd7009108476f0c646038f71f3c"
    # MIT asks for its notice wherever the names go; they ship in this file.
    assert data["name_license"][0] == "MIT License"
    assert any("Ryan-rsm-McKenzie" in line for line in data["name_license"])
    for version in ("1.5.97.0", "1.6.640.0", "1.6.1170.0"):
        functions = data["game_versions"][version]["functions"]
        assert len(functions) > 500, (version, len(functions))
        for key in functions:
            int(key, 16)


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for test in tests:
        test()
    print(f"{len(tests)} tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
