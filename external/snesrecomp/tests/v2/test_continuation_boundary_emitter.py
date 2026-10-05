"""EB1-EB22: emitter integration for unresolved-M/X continuation boundaries.

Every fixture is a synthetic ROM in logical bank $8F with fictional PCs.  No
game address appears anywhere in this file (asserted by EB20's audit).

The contract under test, for a call site the solver declared a boundary for:

    proven exit modes  -> exact continuations, one per mode
    unproven residue   -> a runtime transfer keyed on the LIVE CPU M/X,
                          tiering down to the interpreter when no exact
                          static continuation exists for that width

and, at every point, never an exact symbol produced from a preserved,
assumed or nearest caller M/X.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "recompiler"), str(ROOT)]

from v2.continuation_boundary import (BoundarySpec, BoundaryTable,  # noqa: E402
                                      audit_rows, boundary_at, from_manifest,
                                      mx_index, set_continuation_boundaries)
from v2.emit_function import emit_function  # noqa: E402

BANK = 0x0F                       # logical bank $8F -- fictional
LOG = 0x0F0000

# ---------------------------------------------------------------- opcodes
def JSR(a):  return bytes([0x20, a & 0xFF, (a >> 8) & 0xFF])
def JSL(a):  return bytes([0x22, a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF])
def JMP(a):  return bytes([0x4C, a & 0xFF, (a >> 8) & 0xFF])
RTS, RTL, NOP = b"\x60", b"\x6B", b"\xEA"
SEP_M, REP_M = b"\xE2\x20", b"\xC2\x20"
SEP_X, REP_X = b"\xE2\x10", b"\xC2\x10"
SELFLOOP = b"\x80\xFE"            # BRA * -- provably no normal exit

TIER_DOWN = "interp_tier_dispatch_balanced"
MX_SWITCH = "switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1))"
EXACT_SYMBOL = re.compile(r"\b(?:CODE|bank)_[0-9A-Fa-f]{2,6}_?[0-9A-Fa-f]{0,4}_M\dX\d\b")


def rom_with(blobs) -> bytes:
    """A 1 MiB LoROM image; keys are logical bank-$8F addresses."""
    buf = bytearray(b"\xEA" * 0x100000)
    for pc, blob in blobs.items():
        assert 0x8000 <= pc <= 0xFFFF
        off = BANK * 0x8000 + (pc - 0x8000)
        buf[off:off + len(blob)] = blob
    return bytes(buf)


def spec(site16, cont16, callee16, *, modes=(), m=1, x=1, state="UNKNOWN",
         kind="direct-call", owner=None):
    return BoundarySpec(site_pc24=LOG | site16, continuation_pc24=LOG | cont16,
                        callee_pc24=LOG | callee16, callee_m=m, callee_x=x,
                        proven_modes=frozenset(modes), via_exit_state=state,
                        kind=kind, owner=owner)


def emit(rom, start, *, m=1, x=1, end=None, boundaries=(), **kw):
    """Emit one function with a boundary table installed, then uninstall."""
    set_continuation_boundaries(BoundaryTable(tuple(boundaries)))
    try:
        return emit_function(rom, bank=BANK, start=start, entry_m=m,
                             entry_x=x, end=end, **kw)
    finally:
        set_continuation_boundaries(None)


def cases(src):
    """The (index, statement) pairs of the boundary switch, in emitted order."""
    out = []
    for line in src.splitlines():
        hit = re.match(r"\s*case (\d+): (.*)", line)
        if hit and "proven exit" in line:
            out.append((int(hit.group(1)), hit.group(2).split("/*")[0].strip()))
    return out


def default_arm(src):
    for line in src.splitlines():
        if line.strip().startswith("default:") and "residue" in line:
            return line.strip()
    return None


# ================================================================= EB1
def test_eb1_exact_continuation_needs_no_boundary():
    """EXACT M1X0 -> exact target M1X0. A fully proven site is not a
    boundary at all, and the emitter's normal path is untouched."""
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS,
                    0x9100: SEP_M + RTS})
    src = emit(rom, 0x9000, m=1, x=0, end=0x9006,
               callee_exit_mx={(LOG | 0x9100, 1, 0): (1, 0)})
    assert boundary_at(LOG | 0x9000, LOG | 0x9100, 1, 0) is None
    assert "L_9003_M1X0:" in src
    assert TIER_DOWN not in src
    assert "continuation boundary" not in src


# ================================================================= EB2
def test_eb2_exact_changed_mode_follows_the_callee_not_the_caller():
    """Caller enters M1X1; the callee exits M1X0.  The continuation is the
    callee's mode -- the caller's own width is never preserved onto it."""
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS,
                    0x9100: SEP_M + REP_X + RTS})
    src = emit(rom, 0x9000, m=1, x=1, end=0x9006,
               callee_exit_mx={(LOG | 0x9100, 1, 1): (1, 0)})
    assert "L_9003_M1X0:" in src
    assert "L_9003_M1X1:" not in src


# ================================================================= EB3
def test_eb3_set_emits_one_exact_path_per_proven_mode():
    """A fully proven two-mode exit gets two exact continuations and no
    boundary: there is no residue to transfer dynamically."""
    rom = rom_with({0x9000: JSR(0x9100) + b"\xF0\x01" + NOP + RTS,
                    0x9100: RTS})
    src = emit(rom, 0x9000, m=1, x=1, end=0x9007,
               callee_exit_mx_modes={(LOG | 0x9100, 1, 1): frozenset({(1, 0), (1, 1)})})
    assert "L_9003_M1X0:" in src and "L_9003_M1X1:" in src
    assert "continuation boundary" not in src
    assert TIER_DOWN not in src


# ================================================================= EB4
def test_eb4_unknown_with_zero_modes_emits_no_exact_target():
    """The 808122 class in miniature: no proven mode, so no exact target may
    be named at any width -- only the dynamic fallback transfer."""
    rom = rom_with({0x9000: JSR(0x9100) + NOP + JMP(0x9500),
                    0x9100: RTS, 0x9500: RTS})
    src = emit(rom, 0x9000, end=0x9007,
               boundaries=[spec(0x9000, 0x9003, 0x9100)])
    assert "continuation boundary at $0F9003" in src
    assert TIER_DOWN in src
    assert "residual only" in src
    # No continuation block was emitted, at any width, and the JMP past
    # `end:` beyond it was never reached by the emitter.
    assert "L_9003_" not in src
    assert MX_SWITCH not in src.split("continuation boundary")[1]
    assert not EXACT_SYMBOL.search(src.split("continuation boundary")[1])


# ================================================================= EB5
def test_eb5_unknown_plus_one_proven_mode_keeps_the_exact_path():
    """One proven mode stays exact; the residue stays dynamic.  Never
    EXACT+UNKNOWN -> UNKNOWN only, and never EXACT+UNKNOWN -> assume."""
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS, 0x9100: RTS})
    src = emit(rom, 0x9000, end=0x9006,
               boundaries=[spec(0x9000, 0x9003, 0x9100, modes=[(1, 0)])])
    assert cases(src) == [(mx_index(1, 0), "goto L_9003_M1X0;")]
    assert "L_9003_M1X0:" in src
    assert TIER_DOWN in default_arm(src)


# ================================================================= EB6
def test_eb6_unknown_plus_two_proven_modes_keeps_both_exact_paths():
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS, 0x9100: RTS})
    src = emit(rom, 0x9000, end=0x9006,
               boundaries=[spec(0x9000, 0x9003, 0x9100, modes=[(1, 0), (1, 1)])])
    assert cases(src) == [(mx_index(1, 0), "goto L_9003_M1X0;"),
                          (mx_index(1, 1), "goto L_9003_M1X1;")]
    assert "L_9003_M1X0:" in src and "L_9003_M1X1:" in src
    assert TIER_DOWN in default_arm(src)


# ================================================================= EB7
def test_eb7_no_exit_has_no_normal_continuation():
    """A provably non-returning callee produces no boundary, so the emitter
    emits no continuation transfer of any kind."""
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS, 0x9100: SELFLOOP})
    src = emit(rom, 0x9000, end=0x9006)
    assert boundary_at(LOG | 0x9000, LOG | 0x9100, 1, 1) is None
    assert "continuation boundary" not in src
    assert TIER_DOWN not in src


# ================================================================= EB8
def test_eb8_poison_is_may_return_and_takes_the_same_boundary_policy():
    """POISON dominates UNKNOWN in the exit lattice but agrees with it on
    returnability, so it takes the identical emitter treatment: reachability
    asserted, zero modes invented, residue dynamic."""
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS, 0x9100: RTS})
    poisoned = emit(rom, 0x9000, end=0x9006,
                    boundaries=[spec(0x9000, 0x9003, 0x9100, state="POISON")])
    unknown = emit(rom, 0x9000, end=0x9006,
                   boundaries=[spec(0x9000, 0x9003, 0x9100, state="UNKNOWN")])
    assert TIER_DOWN in poisoned and "L_9003_" not in poisoned
    assert poisoned.replace("POISON", "UNKNOWN") == unknown


# ================================================================= EB9
def test_eb9_direct_jsr_residue_resumes_three_bytes_past_the_site():
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS, 0x9100: RTS})
    src = emit(rom, 0x9000, end=0x9006,
               boundaries=[spec(0x9000, 0x9003, 0x9100)])
    assert f"0x{LOG | 0x9003:06x}u, 0x{LOG | 0x9000:06x}u" in src


# ================================================================= EB10
def test_eb10_direct_jsl_residue_resumes_four_bytes_past_the_site():
    rom = rom_with({0x9000: JSL(LOG | 0x9100) + NOP + RTS, 0x9100: RTL})
    src = emit(rom, 0x9000, end=0x9006,
               boundaries=[spec(0x9000, 0x9004, 0x9100)])
    assert f"0x{LOG | 0x9004:06x}u, 0x{LOG | 0x9000:06x}u" in src
    assert TIER_DOWN in src


# ================================================================= EB11
def test_eb11_tail_call_past_end_stops_at_the_boundary():
    """The exact shape of the four real symptoms.  Without the boundary the
    emitter walks the preserved-M/X continuation to a JMP past `end:` and
    names an exact sibling symbol; with it, the walk stops at the boundary
    and that symbol is never produced."""
    rom = rom_with({0x9000: JSR(0x9100) + NOP + JMP(0x9500),
                    0x9100: RTS,
                    0x9500: RTS})
    leaking = emit(rom, 0x9000, end=0x9007, sibling_entry_pcs={0x9500},
                   tail_call_pc16=0x9500, tail_call_target_name="SIBLING")
    assert "tail_call into sibling fn" in leaking
    assert "SIBLING_M1X1" in leaking          # the leak, reproduced

    fixed = emit(rom, 0x9000, end=0x9007, sibling_entry_pcs={0x9500},
                 tail_call_pc16=0x9500, tail_call_target_name="SIBLING",
                 boundaries=[spec(0x9000, 0x9003, 0x9100)])
    assert "SIBLING" not in fixed
    assert TIER_DOWN in fixed


# ================================================================= EB12
def test_eb12_cross_owner_residue_invents_no_owner_specific_symbol():
    """The dynamic arm names a guest PC, never another owner's C symbol."""
    rom = rom_with({0x9000: JSR(0x9100) + NOP + JMP(0x9500),
                    0x9100: RTS, 0x9500: RTS})
    src = emit(rom, 0x9000, end=0x9007, sibling_entry_pcs={0x9500},
               tail_call_pc16=0x9500, tail_call_target_name="OTHER_OWNER",
               boundaries=[spec(0x9000, 0x9003, 0x9100, owner="OTHER_OWNER")])
    assert "OTHER_OWNER" not in src
    arm = default_arm(src) or [l for l in src.splitlines() if TIER_DOWN in l][0]
    assert f"0x{LOG | 0x9003:06x}u" in arm and "(" in arm


# ================================================================= EB13
def test_eb13_proven_mode_without_a_static_body_tiers_down():
    """A proven mode whose continuation block is not in this body still gets
    its own arm -- an exact runtime lookup -- not a sibling's body."""
    rom = rom_with({0x9000: JSR(0x9100) + RTS, 0x9100: RTS})
    src = emit(rom, 0x9000, end=0x9004,
               boundaries=[spec(0x9000, 0x9800, 0x9100, modes=[(0, 1)])])
    arms = [l for l in src.splitlines() if "case " in l and "proven" in l]
    assert len(arms) == 1 and TIER_DOWN in arms[0]
    assert "no local continuation block" in arms[0]
    assert not EXACT_SYMBOL.search(arms[0])


# ================================================================= EB14
def test_eb14_proven_mode_with_a_static_body_takes_the_aot_path():
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS, 0x9100: RTS})
    src = emit(rom, 0x9000, end=0x9006,
               boundaries=[spec(0x9000, 0x9003, 0x9100, modes=[(1, 1)])])
    (arm,) = cases(src)
    assert arm == (mx_index(1, 1), "goto L_9003_M1X1;")
    assert TIER_DOWN not in arm[1]


# ============================================================ EB15-EB18
def _selection_source(mode):
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS, 0x9100: RTS})
    return emit(rom, 0x9000, end=0x9006,
                boundaries=[spec(0x9000, 0x9003, 0x9100, modes=[mode])])


def test_eb15_runtime_selects_m0x0():
    src = _selection_source((0, 0))
    assert cases(src) == [(0, "goto L_9003_M0X0;")]


def test_eb16_runtime_selects_m0x1():
    src = _selection_source((0, 1))
    assert cases(src) == [(1, "goto L_9003_M0X1;")]


def test_eb17_runtime_selects_m1x0():
    src = _selection_source((1, 0))
    assert cases(src) == [(2, "goto L_9003_M1X0;")]


def test_eb18_runtime_selects_m1x1():
    src = _selection_source((1, 1))
    assert cases(src) == [(3, "goto L_9003_M1X1;")]


def test_eb15_18_index_matches_the_runtime_dispatch_encoding():
    """The switch index must be the engine's own ((m<<1)|x), so a generated
    case agrees with `_cpu_dispatch_lookup` in cpu_state.c."""
    assert [mx_index(m, x) for m in (0, 1) for x in (0, 1)] == [0, 1, 2, 3]


# ================================================================= EB19
def test_eb19_no_nearest_survivor_anywhere_on_a_boundary():
    """Every width outside the proven set reaches the interpreter, never a
    proven sibling's body.  Three of four widths are unproven here."""
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS, 0x9100: RTS})
    src = emit(rom, 0x9000, end=0x9006,
               boundaries=[spec(0x9000, 0x9003, 0x9100, modes=[(1, 1)])])
    assert "nearest survivor" not in src
    assert [i for i, _ in cases(src)] == [mx_index(1, 1)]
    assert TIER_DOWN in default_arm(src)
    # The default arm names no static body of any width.
    assert not EXACT_SYMBOL.search(default_arm(src))


# ================================================================= EB20
def test_eb20_no_dangling_exact_symbol_and_no_real_game_address():
    rom = rom_with({0x9000: JSR(0x9100) + NOP + JMP(0x9500),
                    0x9100: RTS, 0x9500: RTS})
    src = emit(rom, 0x9000, end=0x9007, sibling_entry_pcs={0x9500},
               tail_call_pc16=0x9500, tail_call_target_name="SIBLING",
               boundaries=[spec(0x9000, 0x9003, 0x9100)])
    # The callee's own JSR dispatch legitimately names the four callee
    # variants -- those are ordinary call demands with declared targets.  What
    # must not exist is any exact symbol produced BY the boundary: neither the
    # past-end sibling nor a continuation body at any width.
    residue = src.split("continuation boundary")[1]
    assert not EXACT_SYMBOL.search(residue), EXACT_SYMBOL.findall(residue)
    assert "SIBLING" not in src
    assert not re.search(r"9003_M\dX\d", src)
    # Hardcode audit: this file mentions no address outside the synthetic bank.
    text = Path(__file__).read_text(encoding="utf-8")
    assert not re.search(r"0x8[012][0-9A-Fa-f]{4}\b", text)


# ================================================================= EB21
def test_eb21_generated_output_is_deterministic():
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS, 0x9100: RTS})
    rows = [spec(0x9000, 0x9003, 0x9100, modes=[(1, 1), (0, 0)])]
    first = emit(rom, 0x9000, end=0x9006, boundaries=rows)
    # Reversed insertion order, duplicated rows, reversed mode iteration:
    # the table normalises all of it.
    shuffled = list(reversed(rows)) + rows
    second = emit(rom, 0x9000, end=0x9006, boundaries=shuffled)
    assert first == second
    assert [i for i, _ in cases(first)] == [mx_index(0, 0), mx_index(1, 1)]


# ================================================================= EB22
def test_eb22_proven_modes_are_not_duplicated_across_exact_and_dynamic():
    """A proven mode appears exactly once, as an exact arm.  It must not also
    be reachable through the residual arm."""
    rom = rom_with({0x9000: JSR(0x9100) + NOP + RTS, 0x9100: RTS})
    duplicated = [spec(0x9000, 0x9003, 0x9100, modes=[(1, 1)]),
                  spec(0x9000, 0x9003, 0x9100, modes=[(1, 1)])]
    src = emit(rom, 0x9000, end=0x9006, boundaries=duplicated)
    indices = [i for i, _ in cases(src)]
    assert indices == sorted(set(indices))
    assert src.count("default:") == src.count(MX_SWITCH)
    assert src.count("L_9003_M1X1:") == 1


# ================================================================ table
def test_table_merges_rows_and_survives_the_lorom_mirror():
    """Identity is (site, callee, callee entry M/X); the LoROM alias of each
    address resolves to the same row."""
    table = BoundaryTable((
        spec(0x9000, 0x9003, 0x9100, modes=[(1, 0)]),
        spec(0x9000, 0x9003, 0x9100, modes=[(1, 1)]),
        spec(0x9000, 0x9003, 0x9100, modes=[(0, 0)], m=1, x=0),
    ))
    assert len(table) == 2
    merged = table.at(LOG | 0x9000, LOG | 0x9100, 1, 1)
    assert merged.proven_modes == frozenset({(1, 0), (1, 1)})
    mirror = 0x8F0000
    assert table.at(mirror | 0x9000, mirror | 0x9100, 1, 1) is merged
    # A different callee entry width is a different boundary, not this one.
    other = table.at(LOG | 0x9000, LOG | 0x9100, 1, 0)
    assert other is not merged and other.proven_modes == frozenset({(0, 0)})


def test_table_round_trips_through_the_manifest_section():
    rows = {"unresolved_mx_continuations": [{
        "pc24": "8F9003", "site_pc24": "8F9000", "source_variant": "8F9000_M1X1",
        "via_target": "8F9100_M1X0", "via_exit_state": "UNKNOWN",
        "kind": "direct-call", "proven_modes": ["M1X1"], "owner": "OWNER"}]}
    table = from_manifest(rows)
    (only,) = table.specs
    assert only.continuation_pc24 == 0x8F9003 and only.site_pc24 == 0x8F9000
    assert (only.callee_pc24, only.callee_m, only.callee_x) == (0x8F9100, 1, 0)
    assert only.proven_modes == frozenset({(1, 1)})
    assert only.classification == "exact+residual"
    assert audit_rows(table)[0]["proven_modes"] == ["M1X1"]
    assert from_manifest({"unresolved_mx_continuations": []}).specs == ()


def test_residual_only_classification():
    assert spec(0x9000, 0x9003, 0x9100).classification == "residual-only"
    assert spec(0x9000, 0x9003, 0x9100,
                modes=[(1, 1)]).classification == "exact+residual"


if __name__ == "__main__":
    for name, value in sorted(dict(globals()).items()):
        if name.startswith("test_") and callable(value):
            value()
            print("PASS", name)
