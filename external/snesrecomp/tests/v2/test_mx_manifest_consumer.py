"""SC1-SC17: generic strict-authoritative consumer contract."""

from v2.mx_analysis_solver import VariantId
from v2.mx_manifest_consumer import ProducedVariant, conformance


def _doc(*entries):
    return {"entries": [{"pc24": f"{pc:06X}", "M": m, "X": x,
                         "materialization_status": status, "provenance": ["synthetic"]}
                        for pc, m, x, status in entries]}


def _p(pc, m, x, **kw): return ProducedVariant(VariantId(pc, m, x), **kw)
def _codes(doc, *out): return {f["code"] for f in conformance(doc, out)["findings"]}


def test_sc1_exact_requested_exact_produced():
    assert not _codes(_doc((0xA00000,1,0,"MATERIALIZE_EXACT")), _p(0xA00000,1,0,symbol="P_M1X0"))
def test_sc2_wrong_survivor():
    assert "MC3" in _codes(_doc((0xA00100,1,0,"MATERIALIZE_EXACT")), _p(0xA00100,1,1,symbol="P_M1X1"))
def test_sc3_undeclared_extra():
    assert "MC2" in _codes(_doc((0xA00200,1,0,"MATERIALIZE_EXACT")), _p(0xA00200,1,0), _p(0xA00200,1,1))
def test_sc4_fallback_is_not_survivor():
    doc=_doc((0xA00300,1,0,"INTERPRETER_FALLBACK")); assert not _codes(doc); assert "MC4" in _codes(doc,_p(0xA00300,1,1))
def test_sc5_internal_only_is_not_host():
    doc=_doc((0xA00400,1,0,"INTERNAL_ONLY")); assert not _codes(doc,_p(0xA00400,1,0,kind="internal")); assert "MC5" in _codes(doc,_p(0xA00400,1,0,host_dispatch=True))
def test_sc6_host_requires_all_contracts():
    doc=_doc((0xA00500,1,0,"HOST_ENTRY")); assert not _codes(doc,_p(0xA00500,1,0,host_dispatch=True,registry=True)); assert "MC6" in _codes(doc,_p(0xA00500,1,0))
def test_sc7_prune_protected_root_is_missing():
    assert "MC1" in _codes(_doc((0xA00600,1,0,"MATERIALIZE_EXACT")))
def test_sc8_autopromote_is_undeclared():
    assert "MC2" in _codes(_doc((0xA00700,1,0,"MATERIALIZE_EXACT")),_p(0xA00700,1,0),_p(0xA00700,1,1,provenance="autopromote"))
def test_sc9_four_mx_are_independent():
    doc=_doc(*[(0xA00800,m,x,"MATERIALIZE_EXACT") for m in (0,1) for x in (0,1)])
    assert not _codes(doc,*[_p(0xA00800,m,x,symbol=f"P_M{m}X{x}") for m in (0,1) for x in (0,1)])
def test_sc10_deterministic_report():
    doc=_doc((0xA00900,1,0,"MATERIALIZE_EXACT")); assert conformance(doc,[_p(0xA00900,1,0)]) == conformance(doc,list(reversed([_p(0xA00900,1,0)])))
def test_sc11_unknown_exit_does_not_change_entry_contract():
    assert not _codes(_doc((0xA00A00,1,0,"MATERIALIZE_EXACT")),_p(0xA00A00,1,0))
def test_sc12_no_exit_does_not_change_entry_contract():
    assert not _codes(_doc((0xA00B00,1,0,"MATERIALIZE_EXACT")),_p(0xA00B00,1,0))
def test_sc13_prune_is_reported_as_mc7():
    from v2.mx_manifest_consumer import conformance
    doc = _doc((0xA00C00,1,0,"MATERIALIZE_EXACT"))
    report = conformance(doc, [_p(0xA00C00,1,0)],
                         removed=[_p(0xA00C00,1,0, provenance="strict prune")])
    assert "MC7" in {f["code"] for f in report["findings"]}

def test_sc14_attempt_alone_is_not_an_undeclared_output():
    """A refused demand that left no trace is audit-only, never MC2/MC9."""
    from v2.mx_manifest_consumer import AttemptedVariant, conformance
    doc = _doc((0xA00D00, 1, 0, "MATERIALIZE_EXACT"))
    report = conformance(doc, [_p(0xA00D00, 1, 0)], attempted=[
        AttemptedVariant(VariantId(0xA00D00, 1, 1), stage="emitter call target",
                         disposition="REJECTED", provenance="refused")])
    assert report["counts"]["MC2"] == 0
    assert report["counts"]["MC9"] == 0
    assert report["attempted_counts"] == {"total": 1, "REJECTED": 1, "REFERENCED": 0}
    assert report["attempted"][0]["manifest_status"] == "absent"


def test_sc15_referenced_undeclared_attempt_is_an_output_defect():
    """A refused demand the emitter still referenced is unclassifiable output."""
    from v2.mx_manifest_consumer import AttemptedVariant, conformance
    doc = _doc((0xA00E00, 1, 0, "MATERIALIZE_EXACT"))
    report = conformance(doc, [_p(0xA00E00, 1, 0)], attempted=[
        AttemptedVariant(VariantId(0xA00E00, 1, 1), stage="emitter call target",
                         disposition="REFERENCED", provenance="dangling")])
    codes = {f["code"] for f in report["findings"]}
    assert codes == {"MC9"}
    assert report["counts"]["MC2"] == 0
    assert report["attempted_counts"]["REFERENCED"] == 1


def test_sc16_produced_undeclared_is_mc2_not_mc9():
    """MC2 stays reserved for a body that was actually produced."""
    from v2.mx_manifest_consumer import conformance
    doc = _doc((0xA00F00, 1, 0, "MATERIALIZE_EXACT"))
    report = conformance(doc, [_p(0xA00F00, 1, 0), _p(0xA00F00, 1, 1,
                                                      provenance="autopromote")])
    assert report["counts"]["MC2"] == 1
    assert report["counts"]["MC9"] == 0


def test_sc17_attempt_for_a_declared_variant_is_not_a_finding():
    """An attempt that the manifest does authorise is never a defect."""
    from v2.mx_manifest_consumer import AttemptedVariant, conformance
    doc = _doc((0xA01000, 1, 0, "MATERIALIZE_EXACT"))
    report = conformance(doc, [_p(0xA01000, 1, 0)], attempted=[
        AttemptedVariant(VariantId(0xA01000, 1, 0), disposition="REFERENCED",
                         provenance="satisfied")])
    assert not report["findings"]
    assert report["attempted"][0]["manifest_status"] == "MATERIALIZE_EXACT"
