"""AM1-AM10 gates for the solver-authoritative manifest boundary."""

from v2.mx_analysis_solver import VariantId
from v2.mx_authoritative_manifest import (
    MaterializationPolicy, OwnerInfo, build_manifest, manifest_json,
    resolve_exact_or_fallback,
)
from v2.codegen import (resolve_variant_for_target, set_exact_variant_selection,
                        set_valid_variants)


def _fact(pc, m, x, state="EXACT", origins=("synthetic demand",)):
    return {"variant_id": {"pc24": f"{pc:06X}", "m": m, "x": x},
            "origins": list(origins), "host_reentry": False,
            "exit": {"state": state, "modes": []}, "demands": []}


def _owner(pc, *, kind="function", host=False, modes=((1, 1),)):
    return OwnerInfo(pc, (pc & 0xffff) + 0x20, f"Synthetic_{pc:06X}", kind,
                     frozenset(modes), True, pc if host else None)


def _statuses(doc):
    return {(int(e["pc24"], 16), e["M"], e["X"]):
            e["materialization_status"] for e in doc["entries"]}


def test_am1_exact_single_variant():
    doc = build_manifest([_fact(0xA09000, 1, 0)], [_owner(0xA09000)])
    assert set(_statuses(doc)) == {(0xA09000, 1, 0)}


def test_am2_two_legitimate_variants():
    facts = [_fact(0xA09100, 1, x) for x in (0, 1)]
    assert len(build_manifest(facts, [_owner(0xA09100)])["entries"]) == 2


def test_am3_strict_policy_denies_without_rewriting():
    wanted = VariantId(0xA09200, 1, 0)
    policy = MaterializationPolicy({wanted.pc24: frozenset({(1, 1)})})
    doc = build_manifest([_fact(wanted.pc24, 1, 0)], [_owner(wanted.pc24)], policy)
    entry = doc["entries"][0]
    assert entry["materialization_status"] == "INTERPRETER_FALLBACK"
    assert (entry["M"], entry["X"]) == (1, 0)
    assert resolve_exact_or_fallback(doc, wanted) is None


def test_am4_legacy_only_survivor_is_never_selected():
    wanted = VariantId(0xA09300, 1, 0)
    doc = build_manifest([_fact(wanted.pc24, 1, 0)], [_owner(wanted.pc24)])
    assert resolve_exact_or_fallback(doc, wanted) == wanted
    assert resolve_exact_or_fallback(doc, VariantId(wanted.pc24, 1, 1)) is None


def test_am5_no_exit_entry_is_materializable():
    doc = build_manifest([_fact(0xA09400, 1, 0, "NO_EXIT")], [_owner(0xA09400)])
    assert doc["entries"][0]["materialization_status"] == "MATERIALIZE_EXACT"


def test_am6_unknown_exit_does_not_erase_exact_entry():
    doc = build_manifest([_fact(0xA09500, 1, 0, "UNKNOWN")], [_owner(0xA09500)])
    assert doc["entries"][0]["materialization_status"] == "MATERIALIZE_EXACT"


def test_am7_internal_continuation_is_not_a_host_entry():
    doc = build_manifest([_fact(0xA09608, 1, 0)], [_owner(0xA09600)])
    assert doc["entries"][0]["materialization_status"] == "INTERNAL_ONLY"


def test_am8_host_continuation_keeps_contract():
    doc = build_manifest([_fact(0xA09700, 1, 0)],
                         [_owner(0xA09700, kind="continuation", host=True,
                                 modes=((1, 0),))])
    assert doc["entries"][0]["materialization_status"] == "HOST_ENTRY"


def test_am9_order_is_byte_deterministic():
    facts = [_fact(0xA09800, m, x) for m, x in ((1, 1), (0, 0), (1, 0))]
    owners = [_owner(0xA09800)]
    assert manifest_json(build_manifest(facts, owners)) == \
        manifest_json(build_manifest(reversed(facts), reversed(owners)))


def test_am10_all_four_variants_remain_distinct():
    facts = [_fact(0xA09900, m, x) for m in (0, 1) for x in (0, 1)]
    doc = build_manifest(facts, [_owner(0xA09900)])
    assert len(_statuses(doc)) == 4


def test_anti_nearest_survivor_codegen_mode():
    set_valid_variants({0x209A00: frozenset({(1, 1)})})
    set_exact_variant_selection(True)
    try:
        assert resolve_variant_for_target(0x209A00, 1, 0) == (1, 0)
    finally:
        set_exact_variant_selection(False)
        set_valid_variants({})
