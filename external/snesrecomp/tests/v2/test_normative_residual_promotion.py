"""NP1-NP14: `residual` as the normative continuation policy, and the strict
manifest's exact dispatch / host registry contract.

Synthetic manifests only, fictional logical bank $8F.  No game PC appears
anywhere in this file (NP14 checks that by reading the file itself).
"""

import inspect
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "recompiler"), str(ROOT), str(ROOT / "tools")]

from v2.mx_analysis_solver import (MxAnalysisSolver, RomAnalyzer, RomEntry,
                                   Seed, VariantId)
from v2.mx_authoritative_manifest import OwnerInfo, build_manifest
from v2.mx_manifest_consumer import (ProducedVariant, conformance,
                                     dispatch_registry_conformance,
                                     fallback_runtime_conformance,
                                     dispatch_required, registry_required)
import v2_mx_analyze
import v2_strict_manifest_regen as strict

BANK = 0x0F
LOG = 0x8F0000


def _entry(pc16, m, x, status, *, kind="function", declared=None,
           symbol=True):
    pc = LOG | pc16
    return {"pc24": f"{pc:06X}", "M": m, "X": x,
            "symbol": f"P{pc16:04X}_M{m}X{x}" if symbol else None,
            "owner": f"P{pc16:04X}", "entry_kind": kind,
            "materialization_status": status, "provenance": ["synthetic"],
            "policy": {"declared_modes": declared if declared is not None
                       else [f"M{m}X{x}"]}}


def _doc(*entries):
    return {"entries": list(entries), "unresolved_mx_continuations": []}


def _tables(doc):
    dispatch, _rows = strict._dispatch_source(doc, {BANK})
    registry, _reg = strict._registry_source(doc, {BANK}, {})
    return dispatch, registry


def _defined(doc):
    return {e["symbol"] for e in doc["entries"] if e["symbol"]}


def _codes(doc, dispatch, registry, defined=None):
    report = dispatch_registry_conformance(
        doc, dispatch, registry, _defined(doc) if defined is None else defined)
    return {code for code, n in report["counts"].items() if n}, report


def test_np1_residual_is_the_library_default():
    assert RomAnalyzer.NORMATIVE_CONTINUATION_POLICY == "residual"
    default = inspect.signature(RomAnalyzer.__init__).parameters[
        "continuation_policy"].default
    assert default == "residual"
    assert RomAnalyzer(b"\x00" * 0x8000, []).continuation_policy == "residual"


def test_np2_stop_remains_explicitly_selectable():
    assert "stop" in RomAnalyzer.CONTINUATION_POLICIES
    assert RomAnalyzer(b"\x00" * 0x8000, [],
                       continuation_policy="stop").continuation_policy == "stop"


def test_np3_tool_default_matches_library():
    default = inspect.signature(v2_mx_analyze.build_solver).parameters[
        "continuation_policy"].default
    assert default == RomAnalyzer.NORMATIVE_CONTINUATION_POLICY


def test_np4_facts_and_manifest_carry_policy_explicitly():
    rom = bytearray(b"\xEA" * 0x100000)
    rom[BANK * 0x8000] = 0x60                       # RTS at $8F:8000
    for policy in ("stop", "residual"):
        analyzer = RomAnalyzer(bytes(rom), [RomEntry(VariantId(LOG | 0x8000, 1, 1),
                                                    BANK, 0x8001, "function")],
                               continuation_policy=policy)
        solver = MxAnalysisSolver(analyzer)
        solver.add_seed(Seed(VariantId(LOG | 0x8000, 1, 1), "root"))
        facts = solver.solve().to_dict()
        assert facts["continuation_policy"] == policy
        owner = OwnerInfo(LOG | 0x8000, 0x8001, "P8000", "function",
                          frozenset({(1, 1)}))
        doc = build_manifest(facts["variants"], [owner],
                             continuation_policy=facts["continuation_policy"])
        assert doc["continuation_policy"] == policy


def test_np5_dispatch_criteria_are_status_based():
    assert dispatch_required(_entry(0x9000, 1, 1, "MATERIALIZE_EXACT"))
    assert dispatch_required(_entry(0x9000, 1, 1, "HOST_ENTRY"))
    assert not dispatch_required(_entry(0x9000, 1, 1, "INTERNAL_ONLY", symbol=False))
    assert not dispatch_required(_entry(0x9000, 1, 1, "INTERPRETER_FALLBACK", symbol=False))


def test_np6_clean_tables_conform_and_skip_internal_and_fallback():
    doc = _doc(_entry(0x9000, 1, 1, "MATERIALIZE_EXACT"),
               _entry(0x9000, 1, 0, "MATERIALIZE_EXACT"),
               _entry(0x9004, 1, 1, "INTERNAL_ONLY", symbol=False),
               _entry(0x9100, 1, 0, "HOST_ENTRY", kind="continuation"),
               _entry(0x9200, 1, 1, "INTERPRETER_FALLBACK", symbol=False))
    dispatch, registry = _tables(doc)
    codes, report = _codes(doc, dispatch, registry)
    assert not codes, report["findings"]
    assert report["dispatch"]["rows"] == 3 and report["dispatch"]["pcs"] == 2
    assert "8F9004" not in dispatch.upper().replace("0X", "")
    assert [r["symbol"] for r in report["registry"]["rows"]] == ["P9100_M1X0"]
    assert report["registry"]["rows"][0]["kind"] == "INTERP_AOT_ENTRY_CONTINUATION"


def test_np7_exact_slot_index_never_a_sibling():
    doc = _doc(_entry(0x9000, 0, 1, "MATERIALIZE_EXACT"))
    dispatch, _ = _tables(doc)
    row = re.search(r"\{ 0x0F9000u, \{ ([^}]*) \} \}", dispatch).group(1)
    assert [s.strip() for s in row.split(",")] == ["NULL", "P9000_M0X1", "NULL", "NULL"]


def test_np8_only_declared_host_width_enters_registry():
    doc = _doc(_entry(0x9100, 1, 0, "HOST_ENTRY", kind="continuation", declared=["M1X0"]),
               _entry(0x9100, 1, 1, "HOST_ENTRY", kind="continuation", declared=["M1X0"]))
    assert registry_required(doc["entries"][0])
    assert not registry_required(doc["entries"][1])
    dispatch, registry = _tables(doc)
    codes, report = _codes(doc, dispatch, registry)
    assert not codes, report["findings"]
    assert report["registry"]["host_entries_without_registry_row"] == ["8F9100_M1X1"]
    assert "P9100_M1X1" in dispatch and "P9100_M1X1" not in registry


def test_np9_two_declared_widths_at_one_host_pc_fail_closed():
    doc = _doc(_entry(0x9100, 1, 0, "HOST_ENTRY", declared=["M1X0", "M1X1"]),
               _entry(0x9100, 1, 1, "HOST_ENTRY", declared=["M1X0", "M1X1"]))
    try:
        _tables(doc)
    except ValueError as exc:
        assert "duplicate AOT registry logical PC" in str(exc)
    else:
        raise AssertionError("registry accepted two rows for one logical PC")


def test_np10_tampered_tables_are_caught_from_text():
    doc = _doc(_entry(0x9000, 1, 1, "MATERIALIZE_EXACT"),
               _entry(0x9004, 1, 1, "INTERNAL_ONLY", symbol=False),
               _entry(0x9100, 1, 1, "HOST_ENTRY"))
    dispatch, registry = _tables(doc)
    # Unauthorized sibling width + wrong slot.
    bad = dispatch.replace("{ NULL, NULL, NULL, P9000_M1X1 }",
                           "{ NULL, NULL, P9000_M1X1, P9000_M1X1 }")
    assert {"DC3", "DC1"} <= _codes(doc, bad, registry)[0]
    # Missing row.
    bad = re.sub(r"^.*P9000_M1X1 \} \},.*\n", "", dispatch, flags=re.M)
    assert "DC2" in _codes(doc, bad, registry)[0]
    # Internal-only label exposed through dispatch.
    bad = dispatch.replace("const DispatchEntry g_dispatch_table[] = {",
                           "const DispatchEntry g_dispatch_table[] = {\n"
                           "    { 0x0F9004u, { NULL, NULL, NULL, L9004_M1X1 } },")
    assert "DC5" in _codes(doc, bad, registry)[0]
    # Undefined symbol.
    assert "DC4" in _codes(doc, dispatch, registry, defined={"P9100_M1X1"})[0]
    # Registry: internal row, wrong kind, unsorted.
    bad = registry.replace("P9100_M1X1, INTERP_AOT_ENTRY_FUNCTION",
                           "P9100_M1X1, INTERP_AOT_ENTRY_CONTINUATION")
    assert "RC3" in _codes(doc, dispatch, bad)[0]
    bad = registry.replace("const unsigned", "    { 0x8F9004u, L9004_M1X1, "
                           "INTERP_AOT_ENTRY_FUNCTION },\nconst unsigned")
    codes = _codes(doc, dispatch, bad)[0]
    assert "RC5" in codes or "RC1" in codes
    assert "RC6" in codes
    empty = re.sub(r"^\s*\{ 0x8F9100u.*\n", "", registry, flags=re.M)
    assert "RC2" in _codes(doc, dispatch, empty)[0]


def test_np11_registry_rows_follow_the_manifest_only():
    base = [_entry(0x9100, 1, 1, "HOST_ENTRY"), _entry(0x9200, 1, 0, "HOST_ENTRY")]
    _dispatch, registry = _tables(_doc(*base))
    assert registry.count("INTERP_AOT_ENTRY_FUNCTION },") == 2
    demoted = [base[0], dict(base[1], materialization_status="MATERIALIZE_EXACT")]
    _dispatch, registry = _tables(_doc(*demoted))
    assert registry.count("INTERP_AOT_ENTRY_FUNCTION },") == 1
    assert "P9200" not in registry


def test_np12_host_width_contract_in_conformance():
    host = _entry(0x9100, 1, 1, "HOST_ENTRY", declared=["M1X0"])
    doc = _doc(host)
    ok = ProducedVariant(VariantId(LOG | 0x9100, 1, 1), symbol="P9100_M1X1",
                         host_dispatch=True, registry=False)
    assert not conformance(doc, [ok])["findings"]
    leaked = ProducedVariant(VariantId(LOG | 0x9100, 1, 1), symbol="P9100_M1X1",
                             host_dispatch=True, registry=True)
    assert "MC5" in {f["code"] for f in conformance(doc, [leaked])["findings"]}
    declared = _entry(0x9100, 1, 1, "HOST_ENTRY")
    missing = ProducedVariant(VariantId(LOG | 0x9100, 1, 1), symbol="P9100_M1X1",
                              host_dispatch=True, registry=False)
    assert "MC6" in {f["code"] for f in conformance(_doc(declared), [missing])["findings"]}


def test_np13_tables_are_order_independent():
    entries = [_entry(0x9000 + 4 * i, m, x, "MATERIALIZE_EXACT")
               for i in range(4) for m in (0, 1) for x in (0, 1)]
    entries.append(_entry(0x9100, 1, 1, "HOST_ENTRY"))
    forward = _tables(_doc(*entries))
    backward = _tables(_doc(*reversed(entries)))
    assert forward == backward


def test_np14_no_game_address_in_generic_code_or_this_file():
    # A ROM address in the Goof banks; the LoROM mirror mask 0x800000 is not one.
    pattern = re.compile(r"0x(8[0-2]|0[0-2])(?!0000\b)[0-9A-Fa-f]{4}\b")
    for path in (Path(__file__), ROOT / "tools" / "v2_strict_manifest_regen.py",
                 ROOT / "recompiler" / "v2" / "mx_manifest_consumer.py"):
        text = path.read_text(encoding="utf-8")
        assert not pattern.search(text), (path, pattern.search(text))


def test_fb1_fallback_requires_real_runtime_disposition():
    doc = _doc(_entry(0x9340, 1, 1, "INTERPRETER_FALLBACK", symbol=False))
    missing = fallback_runtime_conformance(doc, {BANK: "/* manifest only */"})
    assert missing["counts"]["FB1"] == 1
    emitted = fallback_runtime_conformance(
        doc, {BANK: "return interp_tier_run_long_call(cpu, 0x8f9340u, 0x8f9000u);"})
    assert not emitted["findings"]
    assert emitted["represented"] == ["8F9340_M1X1"]


def test_fb2_skipped_fallback_is_not_conformance():
    doc = _doc(_entry(0x9350, 1, 1, "INTERPRETER_FALLBACK", symbol=False))
    report = fallback_runtime_conformance(
        doc, {BANK: "/* target $8F9350 skipped — no CFG */"})
    assert report["counts"] == {"FB1": 1, "FB2": 1}
