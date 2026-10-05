"""Progressive V2 reproducers for continuation cross-function control flow."""

from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_bank import BankEntry, emit_bank  # noqa: E402
from v2 import codegen as v2_codegen  # noqa: E402


def _emit(blobs, entries, valid_variants=None, **kwargs):
    saved = dict(v2_codegen._NAME_RESOLVER)
    saved_variants = dict(v2_codegen._VALID_VARIANTS)
    try:
        v2_codegen.set_name_resolver({e.start: e.name for e in entries})
        if valid_variants is not None:
            v2_codegen.set_valid_variants(valid_variants)
        return emit_bank(make_lorom_bank0(blobs), bank=0,
                         entries=entries, **kwargs)
    finally:
        v2_codegen.set_name_resolver(saved)
        v2_codegen.set_valid_variants(saved_variants)


def test_case_a_continuation_branch_stays_local_until_return():
    """continuation_entry -> local branch -> RTS."""
    src = _emit(
        {
            0x9000: bytes([0x80, 0x02]),       # BRA $9004
            0x9004: bytes([0xE6, 0x10, 0x60]), # INC $10; RTS
        },
        [BankEntry(name="ResumeLocal", start=0x9000, end=0x9007,
                   entry_kind="continuation")],
    )
    assert "goto L_9004_M1X1;" in src, src
    assert "cpu_trace_unresolved_goto_trap" not in src, src
    assert "RTS dispatch" in src, src


def test_case_b_continuation_branch_to_sibling_is_guest_tail_transfer():
    """continuation_entry -> conditional branch -> sibling -> RTS."""
    src = _emit(
        {
            0x9010: bytes([0xB0, 0x0E, 0x60]), # BCS $9020; RTS
            0x9020: bytes([0xE6, 0x11, 0x60]), # sibling: INC $11; RTS
        },
        [
            BankEntry(name="ResumeBranch", start=0x9010, end=0x9013,
                      entry_kind="continuation"),
            BankEntry(name="Neighbor", start=0x9020, end=0x9023),
        ],
    )
    assert "Neighbor_M1X1(cpu)" in src, src
    assert "tail-call past end:" in src, src
    assert "cpu_tailcall_inherit_return_context(_entry_s, _hrv);" in src, src
    assert "cpu_trace_unresolved_goto_trap" not in src, src


def test_case_c_continuation_sibling_direct_jsr_then_return():
    """continuation_entry -> sibling -> direct JSR/RTS -> sibling RTS."""
    src = _emit(
        {
            0x9100: bytes([0x80, 0x0E]),             # BRA $9110
            0x9110: bytes([0x20, 0x20, 0x91, 0x60]), # JSR $9120; RTS
            0x9120: bytes([0xE6, 0x12, 0x60]),       # INC $12; RTS
        },
        [
            BankEntry(name="ResumeDirect", start=0x9100, end=0x9102,
                      entry_kind="continuation"),
            BankEntry(name="DirectNeighbor", start=0x9110, end=0x9114),
            BankEntry(name="DirectHelper", start=0x9120, end=0x9123),
        ],
    )
    assert "DirectNeighbor_M1X1(cpu)" in src, src
    assert "DirectHelper_M1X1(cpu)" in src, src
    assert "cpu->host_return_valid = 2" in src, src
    assert "if (_r != RECOMP_RETURN_NORMAL)" in src, src
    assert "RTS dispatch" in src, src


def test_case_d_continuation_sibling_indirect_jsr_then_return():
    """continuation_entry -> sibling -> JSR (abs,X) -> return -> RTS."""
    src = _emit(
        {
            0x9200: bytes([0x80, 0x0E]),             # BRA $9210
            0x9210: bytes([0xFC, 0x00, 0x93, 0x60]), # JSR ($9300,X); RTS
            0x9300: bytes([0x00, 0x94]),             # -> $9400
            0x9400: bytes([0xE6, 0x13, 0x60]),       # INC $13; RTS
        },
        [
            BankEntry(name="ResumeIndirect", start=0x9200, end=0x9202,
                      entry_kind="continuation"),
            BankEntry(name="IndirectNeighbor", start=0x9210, end=0x9214),
            BankEntry(name="IndirectHelper", start=0x9400, end=0x9403),
        ],
        indirect_dispatch={0x009210: {"count": 1, "idx_reg": "X"}},
    )
    assert "IndirectNeighbor_M1X1(cpu)" in src, src
    assert "indirect dispatch call: cfg-resolved target list" in src, src
    assert "IndirectHelper_M1X1(cpu)" in src, src
    assert "break;" in src, src
    assert "RTS dispatch" in src, src


def test_case_e_cross_function_and_return_entries_use_live_mx_variants():
    """Both a branch target and later RTS target use their live M/X state."""
    src = _emit(
        {
            0x9500: bytes([0xC2, 0x21, 0xE2, 0x20,
                           0x80, 0x0A]),              # M1X0; BRA $9510
            0x9510: bytes([0xC2, 0x20, 0xE2, 0x10,
                           0x60]),                    # RTS in M0X1
            0x9600: bytes([0xE6, 0x14, 0x60]),       # return continuation
        },
        [
            BankEntry(name="ResumeBeforeReturn", start=0x9500, end=0x9506,
                      entry_kind="continuation", entry_m=1, entry_x=0),
            BankEntry(name="WidthChangingNeighbor", start=0x9510,
                      end=0x9515, entry_m=1, entry_x=0),
            BankEntry(name="ResumeAfterReturn", start=0x9600, end=0x9603,
                      entry_kind="continuation", entry_m=0, entry_x=1),
        ],
    )
    assert "RecompReturn ResumeAfterReturn_M0X1(CpuState *cpu);" in src, src
    assert "WidthChangingNeighbor_M1X0(cpu)" in src, src
    assert "cpu_dispatch_pc_from" in src, src


def test_case_f_strict_sibling_currently_canonicalizes_proven_edge_mx():
    """Characterize the first loss: strict survivor lookup wins over edge M/X."""
    v2_codegen.take_unresolved_call_targets()
    src = _emit(
        {
            0x9700: bytes([0x80, 0x0E]),       # BRA $9710 in M1X0
            0x9710: bytes([0xE6, 0x15, 0x60]), # sibling
        },
        [
            BankEntry(name="StrictSource", start=0x9700, end=0x9702,
                      entry_m=1, entry_x=0),
            BankEntry(name="StrictSibling", start=0x9710, end=0x9713),
        ],
        valid_variants={0x009710: frozenset({(1, 1)})},
    )
    demands = v2_codegen.take_unresolved_call_targets()
    assert "StrictSibling_M1X1(cpu)" in src, src
    assert (0x009710, 1, 1) in demands, demands
    assert (0x009710, 1, 0) not in demands, demands


def test_case_g_continuation_to_strict_sibling_exposes_same_loss():
    """Minimal continuation_entry -> sibling ownership characterization."""
    src = _emit(
        {
            0x9800: bytes([0x80, 0x0E]),
            0x9810: bytes([0xE6, 0x16, 0x60]),
        },
        [
            BankEntry(name="SyntheticResume", start=0x9800, end=0x9802,
                      entry_kind="continuation", entry_m=1, entry_x=0),
            BankEntry(name="SyntheticSibling", start=0x9810, end=0x9813),
        ],
        valid_variants={0x009810: frozenset({(1, 1)})},
    )
    assert "SyntheticSibling_M1X1(cpu)" in src, src


def test_case_h_rep_sep_state_reaches_sibling_exactly():
    """REP/SEP independently change both width axes before transfer."""
    src = _emit(
        {
            0x9900: bytes([0xC2, 0x30, 0x80, 0x0C]), # M0X0; BRA $9910
            0x9910: bytes([0xE2, 0x10, 0x80, 0x0C]), # M0X1; BRA $9920
            0x9920: bytes([0xE2, 0x20, 0x60]),       # M1X1; RTS
        },
        [
            BankEntry(name="RepSource", start=0x9900, end=0x9904),
            BankEntry(name="SepXSibling", start=0x9910, end=0x9914,
                      entry_m=0, entry_x=0),
            BankEntry(name="SepMSibling", start=0x9920, end=0x9923,
                      entry_m=0, entry_x=1),
        ],
    )
    assert "SepXSibling_M0X0(cpu)" in src, src
    assert "SepMSibling_M0X1(cpu)" in src, src


def test_case_i_j_compatible_and_incompatible_joins_are_variant_sets():
    """Equal evidence deduplicates; unequal evidence coexists."""
    v2_codegen.take_unresolved_call_targets()
    entries = [
        BankEntry(name="PredA", start=0x9A00, end=0x9A02,
                  entry_m=1, entry_x=0),
        BankEntry(name="PredB", start=0x9A10, end=0x9A12,
                  entry_m=1, entry_x=0),
        BankEntry(name="PredC", start=0x9A20, end=0x9A22,
                  entry_m=1, entry_x=1),
        BankEntry(name="Join", start=0x9A30, end=0x9A33),
    ]
    _emit({
        0x9A00: bytes([0x80, 0x2E]), # -> $9A30 M1X0
        0x9A10: bytes([0x80, 0x1E]), # -> $9A30 M1X0
        0x9A20: bytes([0x80, 0x0E]), # -> $9A30 M1X1
        0x9A30: bytes([0xE6, 0x17, 0x60]),
    }, entries, valid_variants={0x009A30: frozenset({(1, 0), (1, 1)})})
    demands = v2_codegen.take_unresolved_call_targets()
    assert {(0x009A30, 1, 0), (0x009A30, 1, 1)} <= demands, demands
    assert len([d for d in demands if d == (0x009A30, 1, 0)]) == 1


def test_case_k_all_four_combinations_are_distinct_symbols():
    entries = [
        BankEntry(name="FourModes", start=0x9B00, end=0x9B01,
                  entry_m=m, entry_x=x)
        for m, x in ((0, 0), (0, 1), (1, 0), (1, 1))
    ]
    src = _emit({0x9B00: bytes([0x60])}, entries)
    for m, x in ((0, 0), (0, 1), (1, 0), (1, 1)):
        assert f"FourModes_M{m}X{x}(CpuState *cpu)" in src, src


def test_case_l_missing_state_is_not_represented_as_unknown():
    saved_valid = dict(v2_codegen._VALID_VARIANTS)
    try:
        v2_codegen.set_valid_variants({0x009C00: frozenset({(1, 1)})})
        assert v2_codegen.resolve_variant_for_target(0x009C00, 1, 0) == (1, 1)
        assert BankEntry(name="Implicit", start=0x9C00).entry_m == 1
        assert BankEntry(name="Implicit", start=0x9C00).entry_x == 1
    finally:
        v2_codegen.set_valid_variants(saved_valid)


def test_cross_owner_variant_demands_are_order_independent_and_cycles_terminate():
    blobs = {
        0x9D00: bytes([0x80, 0x0E]), # A -> B
        0x9D10: bytes([0x80, 0xEE]), # B -> A
    }
    entries = [
        BankEntry(name="CycleA", start=0x9D00, end=0x9D02,
                  entry_m=0, entry_x=1),
        BankEntry(name="CycleB", start=0x9D10, end=0x9D12,
                  entry_m=0, entry_x=1),
    ]

    def demands_for(ordered):
        v2_codegen.take_unresolved_call_targets()
        _emit(blobs, ordered, valid_variants={
            0x009D00: frozenset({(1, 1)}),
            0x009D10: frozenset({(1, 1)}),
        })
        return v2_codegen.take_unresolved_call_targets()

    expected = {(0x009D00, 1, 1), (0x009D10, 1, 1)}
    assert demands_for(entries) == expected
    assert demands_for(list(reversed(entries))) == expected
