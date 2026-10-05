"""Generic generated AOT registry contracts (no game-specific addresses)."""

from pathlib import Path
import tempfile

from v2.emit_bank import BankEntry
from tools.v2_regen import _emit_aot_entry_registry


class Cfg:
    def __init__(self, entries):
        self.entries = entries


def emit(entries, variants):
    with tempfile.TemporaryDirectory() as directory:
        out = Path(directory)
        _emit_aot_entry_registry(out, [(0x2A, Path("artificial.cfg"), Cfg(entries))], variants)
        return (out / "aot_entries_v2.c").read_text(encoding="utf-8")


def fails(fragment, entries, variants):
    try:
        emit(entries, variants)
    except ValueError as exc:
        assert fragment in str(exc), str(exc)
    else:
        raise AssertionError(f"expected fail-closed error containing {fragment!r}")


def test_artificial_function_and_continuation_registry():
    entries = [
        BankEntry("Alpha", 0xA123, entry_m=1, entry_x=0,
                  aot_entry_pc=0x9AA123),
        BankEntry("ResumeBeta", 0xB456, entry_m=0, entry_x=1,
                  entry_kind="continuation", aot_entry_pc=0x8BB456),
    ]
    variants = {0x2AA123: {(1, 0)}, 0x2AB456: {(0, 1)}}
    src = emit(entries, variants)
    assert "0x8BB456u, ResumeBeta_M0X1, INTERP_AOT_ENTRY_CONTINUATION" in src
    assert "0x9AA123u, Alpha_M1X0, INTERP_AOT_ENTRY_FUNCTION" in src
    assert "g_aot_entry_registry_count = 2u" in src


def test_empty_registry_is_supported():
    src = emit([BankEntry("Ordinary", 0xA123)], {0x2AA123: {(1, 1)}})
    assert "g_aot_entry_registry_count = 0u" in src


def test_registry_fail_closed_cases():
    missing_name = BankEntry(None, 0xA123, aot_entry_pc=0x9AA123)
    fails("canonical symbol was not emitted", [missing_name], {})

    invalid = BankEntry("Invalid", 0xA123, entry_kind="alien",
                        aot_entry_pc=0x9AA123)
    fails("invalid AOT entry kind", [invalid], {0x2AA123: {(1, 1)}})

    duplicates = [
        BankEntry("One", 0xA123, aot_entry_pc=0x9AA123),
        BankEntry("Two", 0xB456, aot_entry_pc=0x9AA123),
    ]
    fails("duplicate AOT registry logical PC", duplicates,
          {0x2AA123: {(1, 1)}, 0x2AB456: {(1, 1)}})

    diverged = BankEntry("Diverged", 0xA123, entry_m=0, entry_x=1,
                         aot_entry_pc=0x9AA123)
    fails("canonical symbol was not emitted", [diverged],
          {0x2AA123: {(1, 1)}})

    continuation = BankEntry("Resume", 0xB456, entry_m=0, entry_x=0,
                             entry_kind="continuation",
                             aot_entry_pc=0x8BB456)
    fails("canonical symbol was not emitted", [continuation],
          {0x2AB456: {(1, 0)}})


if __name__ == "__main__":
    test_artificial_function_and_continuation_registry()
    test_empty_registry_is_supported()
    test_registry_fail_closed_cases()
    print("AOT entry registry: generic + empty + 5 fail-closed cases PASS")
