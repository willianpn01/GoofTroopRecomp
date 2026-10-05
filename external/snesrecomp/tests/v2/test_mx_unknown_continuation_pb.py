"""PB1-PB20: continuation semantics when a callee may return but its exit
M/X is not statically proven (PC-B13).

Every fixture is a synthetic ROM with fictional addresses in logical bank
$8F.  No game PC appears anywhere in this file.

The corpus is run against all four candidate policies so the comparison in
`mx_unknown_exit_continuation_semantics.md` is reproducible:

    stop      truncate demand discovery at the call site   (legacy)
    preserve  assume the callee preserved the caller M/X
    all_four  demand the continuation at all four M/X
    residual  demand each *proven* exit mode exactly, and record the
              unproven residue as a ContinuationBoundary
"""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "recompiler"), str(ROOT)]

from v2.mx_analysis_solver import (ContinuationBoundary, ExitFact, FactState,
                                   MxAnalysisSolver, RomAnalyzer, RomEntry,
                                   Seed, VariantId)

POLICIES = RomAnalyzer.CONTINUATION_POLICIES
BANK = 0x0F                       # logical bank $8F -- fictional
LOG = 0x8F0000

# ---------------------------------------------------------------- opcodes
def JSR(a):  return bytes([0x20, a & 0xFF, (a >> 8) & 0xFF])
def JSL(a):  return bytes([0x22, a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF])
def JMP(a):  return bytes([0x4C, a & 0xFF, (a >> 8) & 0xFF])
def JML(a):  return bytes([0x5C, a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF])
def JMPIX(a): return bytes([0x7C, a & 0xFF, (a >> 8) & 0xFF])
def BEQ(d):  return bytes([0xF0, d & 0xFF])
RTS, RTL, RTI, NOP = b"\x60", b"\x6B", b"\x40", b"\xEA"
SEP_M, REP_M = b"\xE2\x20", b"\xC2\x20"
SEP_X, REP_X = b"\xE2\x10", b"\xC2\x10"
SELFLOOP = b"\x80\xFE"            # BRA * -- provably no normal exit


class Rom:
    """A 1 MiB LoROM image; `put` writes at a logical bank-$8F address."""

    def __init__(self):
        self.buf = bytearray(b"\xEA" * 0x100000)

    def put(self, addr16, *blobs):
        off = BANK * 0x8000 + (addr16 - 0x8000)
        data = b"".join(blobs)
        self.buf[off:off + len(data)] = data
        return addr16 + len(data)


def entry(addr16, m=1, x=1, end=None, kind="function"):
    return RomEntry(VariantId(LOG | addr16, m, x), BANK, end, kind)


def run(rom, entries, seeds, policy, **kw):
    analyzer = RomAnalyzer(bytes(rom.buf), entries, continuation_policy=policy,
                           **kw)
    solver = MxAnalysisSolver(analyzer)
    for s in seeds:
        solver.add_seed(s)
    return solver.solve()


def labels(result):
    return {v.label for v in result.nodes}


def boundaries(result):
    out = set()
    for node in result.nodes.values():
        out |= node.boundaries
    return out


def fact(result, addr16, m=1, x=1):
    node = result.nodes.get(VariantId(LOG | addr16, m, x))
    return node.exit_fact if node else ExitFact.undiscovered()


# ============================================================== PB1
def _pb1_rom():
    """A calls B; B is RTI (may return, exit M/X unprovable); A then falls
    through into a separately-declared sibling C."""
    rom = Rom()
    rom.put(0x9000, JSR(0x9100))          # $9000 call
    rom.put(0x9003, NOP, NOP)             # $9003 continuation, ends $9005
    rom.put(0x9005, RTS)                  # $9005 = sibling C
    rom.put(0x9100, RTI)                  # B: exit UNKNOWN, no proven mode
    ents = [entry(0x9000, end=0x9005), entry(0x9100), entry(0x9005)]
    return rom, ents


def test_pb1_unknown_but_may_return_keeps_continuation_reachable():
    seen = {}
    for policy in POLICIES:
        rom, ents = _pb1_rom()
        r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], policy)
        seen[policy] = (labels(r), boundaries(r))

    # stop: PC-B13.  The continuation is never demanded at any width.
    assert not any(l.startswith("8F9003") for l in seen["stop"][0])
    assert seen["stop"][1] == set()

    # preserve: invents an exact VariantId with no evidence -- unsound.
    assert "8F9003_M1X1" in seen["preserve"][0]
    assert seen["preserve"][1] == set()

    # all_four: reachability without an assumption, but four static variants
    # and UNKNOWN has been silently widened into SET(all).
    assert {f"8F9003_M{m}X{x}" for m in (0, 1) for x in (0, 1)} <= seen["all_four"][0]
    assert seen["all_four"][1] == set()

    # residual: no exact VariantId is invented (B proves no exit mode), and
    # the continuation is still asserted reachable -- as a boundary.
    assert not any(l.startswith("8F9003") for l in seen["residual"][0])
    b = seen["residual"][1]
    assert len(b) == 1
    (only,) = b
    assert only.pc24 == (LOG | 0x9003)
    assert only.via_exit_state == "UNKNOWN"
    assert only.proven_modes == frozenset()
    assert only.via_target == VariantId(LOG | 0x9100, 1, 1)


# ============================================================== PB2
def test_pb2_no_exit_callee_demands_no_continuation():
    for policy in POLICIES:
        rom = Rom()
        rom.put(0x9000, JSR(0x9100))
        rom.put(0x9003, NOP, NOP)
        rom.put(0x9005, RTS)
        rom.put(0x9100, SELFLOOP)          # provably no normal exit
        ents = [entry(0x9000, end=0x9005), entry(0x9100), entry(0x9005)]
        r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], policy)
        assert fact(r, 0x9100).state is FactState.NO_EXIT, policy
        # NO_RETURN: no continuation demand and no boundary, under EVERY
        # policy.  R6 must not regress to buy back PB1.
        assert not any(l.startswith("8F9003") for l in labels(r)), policy
        assert boundaries(r) == set(), policy
        assert fact(r, 0x9000).state is FactState.NO_EXIT, policy


# ============================================================== PB3 / PB4
def _exact_case(caller_m, caller_x, sep):
    rom = Rom()
    rom.put(0x9000, JSR(0x9100))
    rom.put(0x9003, NOP, RTS)
    rom.put(0x9100, sep, RTS)
    ents = [entry(0x9000, caller_m, caller_x, end=0x9005), entry(0x9100, caller_m, caller_x)]
    return rom, ents


def test_pb3_exact_same_width_continuation_is_exact():
    for policy in POLICIES:
        rom, ents = _exact_case(1, 0, SEP_M)          # callee re-asserts M=1
        r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 0), "root")], policy)
        assert fact(r, 0x9100, 1, 0).state is FactState.EXACT, policy
        assert fact(r, 0x9100, 1, 0).modes == frozenset({(1, 0)}), policy
        assert fact(r, 0x9000, 1, 0).modes == frozenset({(1, 0)}), policy
        assert boundaries(r) == set(), policy


def test_pb4_exact_changed_width_continuation_follows_callee():
    for policy in POLICIES:
        rom, ents = _exact_case(1, 1, REP_X)          # M1X1 in, M1X0 out
        r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], policy)
        assert fact(r, 0x9100, 1, 1).modes == frozenset({(1, 0)}), policy
        # The caller's own exit follows the callee's, not its entry width.
        assert fact(r, 0x9000, 1, 1).modes == frozenset({(1, 0)}), policy
        assert boundaries(r) == set(), policy


# ============================================================== PB5
def test_pb5_set_two_modes_yields_two_exact_continuations():
    for policy in POLICIES:
        rom = Rom()
        rom.put(0x9000, JSR(0x9100))
        rom.put(0x9003, BEQ(0x02), RTS)          # cond branch: decoder forks
        rom.put(0x9007, RTS)
        # callee: BEQ over a REP #$10 -> exits {M1X1, M1X0}
        rom.put(0x9100, BEQ(0x02), REP_X, RTS)
        ents = [entry(0x9000, end=0x9008), entry(0x9100)]
        r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], policy)
        assert fact(r, 0x9100).state is FactState.SET, policy
        assert fact(r, 0x9100).modes == frozenset({(1, 1), (1, 0)}), policy
        # SET is fully proven: no boundary under any policy.
        assert boundaries(r) == set(), policy


# ============================================================== PB6
def _pb6_rom():
    """B has one proven RTS exit AND an unprovable RTI path: UNKNOWN with a
    non-empty proven mode set."""
    rom = Rom()
    rom.put(0x9000, JSR(0x9100))
    rom.put(0x9003, NOP, NOP)
    rom.put(0x9005, RTS)
    rom.put(0x9100, BEQ(0x01), RTI)             # unprovable path
    rom.put(0x9103, REP_X, RTS)                 # proven exit path -> M1X0
    ents = [entry(0x9000, end=0x9005), entry(0x9100), entry(0x9005)]
    return rom, ents


def test_pb6_unknown_with_known_modes_keeps_the_exact_continuations():
    rom, ents = _pb6_rom()
    r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], "residual")
    f = fact(r, 0x9100)
    assert f.state is FactState.UNKNOWN
    assert f.modes and f.modes <= {(1, 1), (1, 0)}
    # The proven modes survive as exact continuations ...
    conts = {l for l in labels(r) if l.startswith("8F9003")}
    assert conts == {f"8F9003_M{m}X{x}" for m, x in sorted(f.modes)}
    # ... and the residual uncertainty is still recorded, not erased.
    (b,) = boundaries(r)
    assert b.proven_modes == f.modes
    assert b.via_exit_state == "UNKNOWN"

    # stop discards the proven modes entirely -- the precision loss PB6 targets.
    rom, ents = _pb6_rom()
    s = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], "stop")
    assert not any(l.startswith("8F9003") for l in labels(s))


# ============================================================== PB7
def test_pb7_poison_is_a_may_return_boundary_not_an_ordinary_unknown():
    rom = Rom()
    rom.put(0x9000, JSR(0x9100))
    rom.put(0x9003, NOP, NOP)
    rom.put(0x9005, RTS)
    rom.put(0x9100, RTS)
    ents = [entry(0x9000, end=0x9005), entry(0x9100), entry(0x9005)]

    class Poisoned(RomAnalyzer):
        def __call__(self, variant, facts):
            if variant.pc24 == (LOG | 0x9100):
                from v2.mx_analysis_solver import Derivation
                return Derivation(poison_reasons={"synthetic poison"})
            return super().__call__(variant, facts)

    solver = MxAnalysisSolver(
        Poisoned(bytes(rom.buf), ents, continuation_policy="residual"))
    solver.add_seed(Seed(VariantId(LOG | 0x9000, 1, 1), "root"))
    r = solver.solve()
    assert fact(r, 0x9100).state is FactState.POISON
    # POISON asserts may-return with zero proven modes: reachability is kept
    # as a boundary, no VariantId is invented, and the caller is POISON --
    # a strictly stronger state than UNKNOWN, so the boundary is hard.
    (b,) = boundaries(r)
    assert b.via_exit_state == "POISON"
    assert b.proven_modes == frozenset()
    assert not any(l.startswith("8F9003") for l in labels(r))
    assert fact(r, 0x9000).state is FactState.POISON


# ============================================================== PB8 / PB9
def test_pb8_direct_jsr_unknown_exit():
    rom, ents = _pb1_rom()
    r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], "residual")
    (b,) = boundaries(r)
    assert b.kind == "direct-call" and b.site_pc24 == (LOG | 0x9000)
    assert b.pc24 == (LOG | 0x9003)          # JSR is 3 bytes


def test_pb9_direct_jsl_unknown_exit():
    rom = Rom()
    rom.put(0x9000, JSL(LOG | 0x9100))
    rom.put(0x9004, NOP, NOP)
    rom.put(0x9006, RTS)
    rom.put(0x9100, RTI)
    ents = [entry(0x9000, end=0x9006), entry(0x9100), entry(0x9006)]
    r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], "residual")
    (b,) = boundaries(r)
    assert b.kind == "direct-call" and b.pc24 == (LOG | 0x9004)   # JSL is 4
    assert b.via_target == VariantId(LOG | 0x9100, 1, 1)


# ============================================================== PB10 / PB11
def _indirect(targets, bodies):
    rom = Rom()
    rom.put(0x9000, JMPIX(0xA000))
    rom.put(0x9003, NOP, RTS)
    for addr, body in bodies.items():
        rom.put(addr, body)
    ents = [entry(0x9000, end=0x9005)] + [entry(a) for a in bodies]
    disp = {(BANK << 16) | 0x9000: {
        "count": len(targets), "idx_reg": "X", "table_bases": (),
        "targets": [LOG | t for t in targets], "ptr_call": True}}
    return rom, ents, disp


def test_pb10_indirect_one_exact_one_unknown_keeps_the_exact_continuation():
    rom, ents, disp = _indirect(
        [0x9100, 0x9200], {0x9100: REP_X + b"\x60", 0x9200: RTI})
    r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")],
            "residual", indirect_dispatch=disp)
    assert fact(r, 0x9100).modes == frozenset({(1, 0)})
    assert fact(r, 0x9200).state is FactState.UNKNOWN
    # The proven target's exact continuation exists ...
    assert "8F9003_M1X0" in labels(r)
    # ... and the unresolved residue stays explicit in the caller's fact.
    f = fact(r, 0x9000)
    assert f.state is FactState.UNKNOWN
    assert any("unknown indirect exit" in (f.reason or "") for _ in (0,))


def test_pb11_all_indirect_targets_no_exit_yields_no_continuation():
    rom, ents, disp = _indirect(
        [0x9100, 0x9200], {0x9100: SELFLOOP, 0x9200: SELFLOOP})
    r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")],
            "residual", indirect_dispatch=disp)
    assert fact(r, 0x9100).state is FactState.NO_EXIT
    assert fact(r, 0x9200).state is FactState.NO_EXIT
    assert not any(l.startswith("8F9003") for l in labels(r))
    assert boundaries(r) == set()


# ============================================================== PB12
def test_pb12_tail_call_invents_no_continuation():
    for policy in POLICIES:
        rom = Rom()
        rom.put(0x9000, JML(LOG | 0x9100))     # tail: no return site at all
        rom.put(0x9100, RTI)
        ents = [entry(0x9000, end=0x9004), entry(0x9100)]
        r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], policy)
        assert fact(r, 0x9100).state is FactState.UNKNOWN, policy
        # A tail call has no continuation: nothing at $9004, no boundary.
        assert not any(l.startswith("8F9004") for l in labels(r)), policy
        assert boundaries(r) == set(), policy


# ============================================================== PB13
def test_pb13_tail_call_past_end_is_the_808122_class():
    """Caller ends before a sibling; the fall-through past `end:` becomes a
    guest-tail edge, and it sits downstream of an UNKNOWN-exit call."""
    rom, ents = _pb1_rom()
    for policy in POLICIES:
        rom2, ents2 = _pb1_rom()
        r = run(rom2, ents2, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], policy)
        tails = {l for l in labels(r) if l.startswith("8F9005")}
        if policy == "stop":
            # The exact class that leaves a dangling exact symbol: the
            # emitter reaches $9005, the solver universe does not.
            assert tails == set()
        elif policy == "preserve":
            assert tails == {"8F9005_M1X1"}          # assumed, not proven
        elif policy == "all_four":
            assert len(tails) == 4
        else:
            # residual: no unproven exact symbol, and the truncation is
            # explicit at $9003 so the emitter can stop there instead of
            # walking on to $9005 under an assumption.
            assert tails == set()
            assert len(boundaries(r)) == 1


# ============================================================== PB14
def test_pb14_unknown_return_never_becomes_a_wrong_owner_exact_body():
    """The continuation PC lies inside another declared owner's body."""
    rom = Rom()
    rom.put(0x9000, JSR(0x9100))
    rom.put(0x9003, NOP, RTS)
    rom.put(0x9100, RTI)
    # $9004 is mid-body of the owner declared at $9003 -- not an entry.
    ents = [entry(0x9000, end=0x9005), entry(0x9100)]
    r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], "residual")
    (b,) = boundaries(r)
    assert b.pc24 == (LOG | 0x9003)
    # The boundary names a PC and a provenance.  It is not a VariantId and
    # carries no owner claim, so it cannot be resolved into another owner's
    # exact body.
    assert not isinstance(b, VariantId)
    assert b.proven_modes == frozenset()
    assert not any(l.startswith("8F9003") for l in labels(r))


# ============================================================== PB15
def test_pb15_all_four_incoming_states_analysed_independently():
    per_state = {}
    for m in (0, 1):
        for x in (0, 1):
            rom, ents = _pb1_rom()
            ents = [entry(0x9000, m, x, end=0x9005), entry(0x9100, m, x),
                    entry(0x9005, m, x)]
            r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, m, x), "root")],
                    "residual")
            per_state[(m, x)] = boundaries(r)
    # Each incoming state produces exactly one boundary, at the same PC,
    # attributed to the callee variant entered with that same state.
    for (m, x), bs in per_state.items():
        assert len(bs) == 1, (m, x)
        (b,) = bs
        assert b.pc24 == (LOG | 0x9003)
        assert b.via_target == VariantId(LOG | 0x9100, m, x)
        assert b.proven_modes == frozenset()


# ============================================================== PB16
def test_pb16_seed_order_does_not_change_the_result():
    seeds = [Seed(VariantId(LOG | 0x9000, 1, 1), "root"),
             Seed(VariantId(LOG | 0x9100, 1, 1), "root2"),
             Seed(VariantId(LOG | 0x9005, 1, 1), "root3")]
    for policy in POLICIES:
        snaps = []
        for order in (seeds, list(reversed(seeds))):
            rom, ents = _pb1_rom()
            r = run(rom, ents, order, policy)
            snaps.append((
                sorted(labels(r)),
                sorted((b.pc24, b.site_pc24, b.via_target, b.via_exit_state,
                        tuple(sorted(b.proven_modes))) for b in boundaries(r)),
                sorted((v.label, n.exit_fact.state.value,
                        tuple(sorted(n.exit_fact.modes)))
                       for v, n in r.nodes.items()),
            ))
        assert snaps[0] == snaps[1], policy


# ============================================================== PB17
def _pb17_rom():
    """A recursive SCC with a base case -- the shape real recursion takes.
    A: BEQ base ; JSR B ; ... ; RTS      B: JSR A ; RTS"""
    rom = Rom()
    rom.put(0x9000, BEQ(0x04))          # -> $9006
    rom.put(0x9002, JSR(0x9100))
    rom.put(0x9005, NOP)
    rom.put(0x9006, RTS)
    rom.put(0x9100, JSR(0x9000))
    rom.put(0x9103, RTS)
    return rom, [entry(0x9000, end=0x9007), entry(0x9100, end=0x9104)]


def test_pb17_recursive_scc_with_unknown_converges():
    for policy in POLICIES:
        rom, ents = _pb17_rom()
        r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], policy)
        assert r.worklist_pops < 200, policy
        # The SCC is resolved through its base case: no node is left at
        # bottom, and the mutual recursion did not diverge.
        for v, n in r.nodes.items():
            assert n.exit_fact.state is not FactState.UNDISCOVERED, (policy, v)
        assert fact(r, 0x9000).modes == frozenset({(1, 1)}), policy
        assert fact(r, 0x9100).modes == frozenset({(1, 1)}), policy


def test_pb17b_base_case_free_scc_terminates_at_bottom_under_every_policy():
    """A degenerate SCC whose every member's only outcome depends on another
    member.  Each node defers on its UNDISCOVERED dependency, so no fact is
    ever produced and nothing re-queues anyone.

    This is a PRE-EXISTING limitation of the worklist (no SCC equation
    solving), it is identical under all four continuation policies, and it
    is therefore NOT caused by -- and not fixed by -- the PC-B13 change.
    The corpus pins it so a future SCC solver has a gate to flip.
    """
    rom = Rom()
    rom.put(0x9000, JSR(0x9100))
    rom.put(0x9003, NOP, RTS)
    rom.put(0x9100, JSR(0x9000))
    rom.put(0x9103, RTS)
    ents = [entry(0x9000, end=0x9005), entry(0x9100, end=0x9104)]
    seen = set()
    for policy in POLICIES:
        r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], policy)
        # Terminates (no oscillation) and is deterministic across policies.
        assert r.worklist_pops == 2, policy
        seen.add(tuple(sorted(
            (v.label, n.exit_fact.state.value) for v, n in r.nodes.items())))
        assert all(n.exit_fact.state is FactState.UNDISCOVERED
                   for n in r.nodes.values()), policy
    assert len(seen) == 1, "policy must not change the degenerate-SCC outcome"


# ============================================================== PB18
def test_pb18_unknown_later_resolved_reprocesses_the_caller():
    """A three-level chain in which the middle node is UNKNOWN on its first
    derivation, gains an EXACT mode on its second and a second mode on its
    third.  Every improvement must reach the caller through the reverse
    dependency."""
    from v2.mx_analysis_solver import Derivation, Demand

    caller = VariantId(0x0F9000, 1, 1)
    mid = VariantId(0x0F9100, 1, 1)
    leaf1 = VariantId(0x0F9200, 1, 1)
    leaf2 = VariantId(0x0F9300, 1, 1)

    def mirror(node, dep, facts, extra=None):
        out = Derivation(demands={Demand(dep, "call-entry", node.pc24)},
                         dependencies={dep})
        f = facts.get(dep, ExitFact.undiscovered())
        if extra is not None and f.state is not FactState.UNDISCOVERED:
            out.demands.add(Demand(extra, "call-entry", node.pc24 + 3))
            out.dependencies.add(extra)
            g = facts.get(extra, ExitFact.undiscovered())
            if g.state in (FactState.EXACT, FactState.SET):
                out.exits.update(g.modes)
            elif g.state is FactState.UNDISCOVERED:
                out.unknown_reasons.add("second callee not yet proven")
        if f.state in (FactState.EXACT, FactState.SET):
            out.exits.update(f.modes)
        elif f.state is FactState.UNDISCOVERED:
            out.unknown_reasons.add("callee not yet proven")
        elif f.state is FactState.UNKNOWN:
            out.unknown_reasons.add("unknown callee")
            out.exits.update(f.modes)
        return out

    def analyze(node, facts):
        if node == leaf1:
            return Derivation(exits={(1, 0)})
        if node == leaf2:
            return Derivation(exits={(0, 0)})
        if node == mid:
            return mirror(node, leaf1, facts, extra=leaf2)
        return mirror(node, mid, facts)

    solver = MxAnalysisSolver(analyze)
    solver.add_seed(Seed(caller, "root"))
    r = solver.solve()

    # The middle node really was re-derived, not decided once.
    assert r.nodes[mid].process_count >= 3
    assert r.nodes[caller].process_count >= 2
    assert r.reverse_dependencies[mid] == {caller}
    assert r.reverse_dependencies[leaf1] == {mid}
    # UNKNOWN -> +EXACT -> +SET: precision strictly improved at the caller.
    assert {(1, 0), (0, 0)} <= r.nodes[caller].exit_fact.modes
    assert {(1, 0), (0, 0)} <= r.nodes[mid].exit_fact.modes


def test_pb18b_boundary_does_not_block_later_precision():
    """A residual boundary is a record, not a fact: when the callee's exit is
    later proven, the caller must still improve."""
    rom, ents = _pb6_rom()
    r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], "residual")
    # The callee proved M1X0/M1X1 exits, and those reached the caller as
    # exact continuations even though a boundary was also recorded.
    assert fact(r, 0x9100).modes
    assert boundaries(r)
    assert {l for l in labels(r) if l.startswith("8F9003")}


# ============================================================== PB19
def test_pb19_unknown_never_resolved_converges_without_inventing_state():
    for policy in POLICIES:
        rom, ents = _pb1_rom()
        r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], policy)
        assert fact(r, 0x9100).state is FactState.UNKNOWN, policy
        assert fact(r, 0x9100).modes == frozenset(), policy
        assert fact(r, 0x9000).state is FactState.UNKNOWN, policy
        if policy in ("stop", "residual"):
            # No exact continuation VariantId was invented at any width.
            assert not any(l.startswith("8F9003") for l in labels(r)), policy


# ============================================================== PB20
def test_pb20_boundary_is_deliverable_to_a_runtime_without_a_static_survivor():
    """The boundary record carries everything a runtime hand-off needs, and
    nothing that presumes one."""
    rom, ents = _pb6_rom()
    r = run(rom, ents, [Seed(VariantId(LOG | 0x9000, 1, 1), "root")], "residual")
    (b,) = boundaries(r)
    # Serialisable, hashable, ordered, and value-equal -- so it can be
    # emitted into a manifest deterministically.
    assert isinstance(hash(b), int)
    twin = ContinuationBoundary(b.pc24, b.site_pc24, b.via_target,
                                b.via_exit_state, b.proven_modes, b.kind)
    assert twin == b and sorted({b, twin}) == [b]
    # Resume PC, the site that produced it, the callee blamed, the exit
    # state, and the proven-mode subset a dispatch may enter exactly.
    assert b.pc24 and b.site_pc24 and b.via_target and b.via_exit_state
    assert b.proven_modes <= {(0, 0), (0, 1), (1, 0), (1, 1)}
    # It names no symbol and no survivor.
    assert not hasattr(b, "symbol") and not hasattr(b, "survivor")
