"""A–AK corpus for the analysis-only interprocedural M/X solver."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "recompiler"), str(ROOT)]

from _helpers import make_lorom_bank0
from v2.mx_analysis_solver import (Demand, Derivation, ExitFact, FactState,
                                   MxAnalysisSolver, RomAnalyzer, RomEntry, Seed,
                                   VariantId)


def v(pc, m=1, x=1):
    return VariantId(pc, m, x)


class FixtureGraph:
    def __init__(self, rules):
        self.rules = rules

    def __call__(self, node, facts):
        return self.rules[node](facts)


def fixed(*modes, demands=(), deps=(), unknown=(), poison=()):
    return lambda _facts: Derivation(set(modes), set(demands), set(deps),
                                     set(unknown), set(poison))


def no_exit(reason="proved infinite loop"):
    return lambda _facts: Derivation(no_exit_reasons={reason})


def solve(rules, seeds):
    solver = MxAnalysisSolver(FixtureGraph(rules))
    for seed in seeds:
        solver.add_seed(seed)
    return solver.solve()


def dependent(callee, continuation_pc, kind="call-entry"):
    def rule(facts):
        demand = Demand(callee, kind, continuation_pc - 3)
        fact = facts.get(callee, ExitFact.undiscovered())
        out = Derivation(demands={demand}, dependencies={callee})
        if fact.state in (FactState.EXACT, FactState.SET):
            out.exits.update(fact.modes)
        elif fact.state == FactState.UNKNOWN:
            out.unknown_reasons.add("unknown callee")
        elif fact.state == FactState.POISON:
            out.poison_reasons.add("poison callee")
        elif fact.state == FactState.NO_EXIT:
            out.no_exit_reasons.add("callee no exit")
        return out
    return rule


def test_a_to_k_local_edges_modes_and_variants():
    # A branch, B fallthrough, C sibling branch, F JMP/JML and G continuation
    # are all exact demands; H–K exercise union/dedup and all four modes.
    root = v(0x009000, 1, 0)
    targets = [v(0x009100, m, x) for m in (0, 1) for x in (0, 1)]
    demands = {Demand(t, "guest-tail", root.pc24) for t in targets}
    rules = {root: fixed(demands=demands)}
    rules.update({t: fixed((t.m, t.x)) for t in targets})
    result = solve(rules, [Seed(root, "host continuation", True)])
    assert set(targets).issubset(result.nodes)
    assert len({n.variant for n in result.nodes.values() if n.variant.pc24 == 0x009100}) == 4
    assert result.nodes[root].host_reentry


def test_d_e_jsr_jsl_return_uses_per_variant_exit():
    for kind in ("JSR", "JSL"):
        caller, callee = v(0x009200, 0, 0), v(0x009300, 0, 0)
        result = solve({caller: dependent(callee, 0x009204, kind),
                        callee: fixed((1, 0))},
                       [Seed(caller, "root")])
        assert result.nodes[caller].exit_fact.modes == frozenset({(1, 0)})
        assert result.nodes[caller].process_count == 2


def test_l_unknown_is_not_undiscovered_or_default():
    node = v(0x009D00)
    result = solve({node: fixed(unknown=("dynamic target",))}, [Seed(node, "root")])
    fact = result.nodes[node].exit_fact
    assert fact.state == FactState.UNKNOWN and not fact.modes
    assert ExitFact.undiscovered().state == FactState.UNDISCOVERED


def _order_fixture(reverse):
    caller, callee = v(0x00A000, 0, 1), v(0x00A100, 0, 1)
    rules = {caller: dependent(callee, 0x00A004), callee: fixed((1, 0))}
    seeds = [Seed(caller, "caller"), Seed(callee, "callee")]
    if reverse:
        seeds.reverse()
    return solve(rules, seeds)


def test_m_n_caller_callee_order_is_identical():
    normal, reverse = _order_fixture(False), _order_fixture(True)
    assert normal.to_json() == reverse.to_json()
    assert normal.nodes[v(0x00A000, 0, 1)].exit_fact.modes == frozenset({(1, 0)})


def test_o_three_node_chain_requeues_transitively():
    a, b, c = v(0x00A200), v(0x00A300), v(0x00A400)
    result = solve({a: dependent(b, 0x00A204), b: dependent(c, 0x00A304),
                    c: fixed((0, 1))}, [Seed(a, "root")])
    assert result.nodes[a].exit_fact.modes == frozenset({(0, 1)})
    assert result.nodes[a].process_count >= 2


def test_p_cycle_terminates_at_finite_least_fixpoint():
    a, b = v(0x00A500), v(0x00A600)
    def a_rule(facts):
        out = dependent(b, 0x00A504)(facts)
        out.exits.add((1, 1))  # known base return path breaks the cycle
        return out
    result = solve({a: a_rule, b: dependent(a, 0x00A604)}, [Seed(a, "root")])
    assert result.nodes[a].exit_fact.modes == frozenset({(1, 1)})
    assert result.nodes[b].exit_fact.modes == frozenset({(1, 1)})
    assert result.worklist_pops < 10


def test_q_multiple_exit_propagates_set():
    caller, callee = v(0x00A700), v(0x00A800)
    result = solve({caller: dependent(callee, 0x00A704),
                    callee: fixed((1, 0), (1, 1))}, [Seed(caller, "root")])
    assert result.nodes[callee].exit_fact.state == FactState.SET
    assert result.nodes[caller].exit_fact.modes == frozenset({(1, 0), (1, 1)})


def test_r_unknown_callee_never_assumes_preserve():
    caller, callee = v(0x00A900, 0, 0), v(0x00AA00, 0, 0)
    result = solve({caller: dependent(callee, 0x00A904),
                    callee: fixed(unknown=("unprovable",))}, [Seed(caller, "root")])
    assert result.nodes[caller].exit_fact.state == FactState.UNKNOWN
    assert not result.nodes[caller].exit_fact.modes


def test_s_host_reentry_demands_sibling_with_provenance():
    host, sibling = v(0x00AB00, 1, 0), v(0x00AC00, 1, 0)
    demand = Demand(sibling, "guest-tail", 0x00AB05)
    result = solve({host: fixed(demands=(demand,), deps=(sibling,)),
                    sibling: fixed((1, 0))},
                   [Seed(host, "host continuation metadata", True)])
    assert sibling in result.nodes
    origins = result.nodes[sibling].origins
    assert any(host.label in origin and "guest-tail" in origin for origin in origins)


def test_poison_is_distinct_and_artifact_is_stable():
    node = v(0x00AD00)
    first = solve({node: fixed(poison=("LLE required",))}, [Seed(node, "root")])
    second = solve({node: fixed(poison=("LLE required",))}, [Seed(node, "root")])
    assert first.nodes[node].exit_fact.state == FactState.POISON
    assert first.to_json() == second.to_json()


def test_t_direct_infinite_loop_is_no_exit():
    node = v(0x00AE00)
    result = solve({node: no_exit()}, [Seed(node, "root")])
    assert result.nodes[node].exit_fact.state == FactState.NO_EXIT


def test_u_caller_of_no_return_has_no_continuation_exit():
    caller, callee = v(0x00AF00), v(0x00B000)
    result = solve({caller: dependent(callee, 0x00AF04), callee: no_exit()},
                   [Seed(caller, "root")])
    assert result.nodes[caller].exit_fact.state == FactState.NO_EXIT
    assert not result.nodes[caller].exit_fact.modes


def test_v_mixed_normal_and_no_exit_paths_keeps_normal_exit():
    node = v(0x00B100)
    def rule(_facts):
        return Derivation(exits={(1, 0)}, no_exit_reasons={"other path loops"})
    result = solve({node: rule}, [Seed(node, "root")])
    assert result.nodes[node].exit_fact == ExitFact.known({(1, 0)})


def test_w_recursive_cycle_without_return_base_stays_undiscovered():
    a, b = v(0x00B200), v(0x00B300)
    result = solve({a: dependent(b, 0x00B204), b: dependent(a, 0x00B304)},
                   [Seed(a, "root")])
    assert result.nodes[a].exit_fact.state == FactState.UNDISCOVERED
    assert result.nodes[b].exit_fact.state == FactState.UNDISCOVERED


def test_x_indirect_jsr_singleton_propagates_live_mx_and_exit():
    caller, callee = v(0x00B400, 0, 1), v(0x00B500, 0, 1)
    result = solve({caller: dependent(callee, 0x00B404, "indirect_jsr"),
                    callee: fixed((1, 0))}, [Seed(caller, "root")])
    demand = next(iter(result.nodes[caller].demands))
    assert demand.target == callee
    assert result.nodes[caller].exit_fact.modes == frozenset({(1, 0)})


def test_y_indirect_jsr_multi_target_unions_exits():
    caller, a, b = v(0x00B600), v(0x00B700), v(0x00B800)
    def rule(facts):
        out = Derivation()
        for target in (a, b):
            out.demands.add(Demand(target, "indirect_jsr", 0x00B604,
                                   "synthetic target set"))
            out.dependencies.add(target)
            fact = facts.get(target, ExitFact.undiscovered())
            if fact.state in (FactState.EXACT, FactState.SET):
                out.exits.update(fact.modes)
        return out
    result = solve({caller: rule, a: fixed((1, 0)), b: fixed((1, 1))},
                   [Seed(caller, "root")])
    assert result.nodes[caller].exit_fact.modes == frozenset({(1, 0), (1, 1)})


def test_z_indirect_tail_has_no_fictitious_continuation():
    caller, target = v(0x00B900), v(0x00BA00)
    result = solve({caller: dependent(target, 0x00B904, "indirect_tail"),
                    target: no_exit()}, [Seed(caller, "root")])
    assert result.nodes[caller].exit_fact.state == FactState.NO_EXIT


def test_aa_non_code_target_is_not_silently_demanded():
    caller = v(0x00BB00)
    result = solve({caller: fixed(unknown=("indirect target DATA_REGION 00BC00",))},
                   [Seed(caller, "root")])
    assert len(result.nodes) == 1
    assert result.nodes[caller].exit_fact.state == FactState.UNKNOWN


def solve_indirect_rom(target_bodies, continuation, *, reverse_seeds=False):
    """Real decoder/adapter fixture using only synthetic bank-$00 PCs."""
    target_pcs = tuple(0x9000 + i * 0x100 for i in range(len(target_bodies)))
    blobs = {0x8000: bytes([0xFC, 0x00, 0x00]) + continuation}
    blobs.update(zip(target_pcs, target_bodies))
    rom = make_lorom_bank0(blobs)
    caller = RomEntry(v(0x808000), 0, 0x8003 + len(continuation), "func")
    targets = [RomEntry(v(0x800000 | pc), 0, pc + len(body), "func")
               for pc, body in zip(target_pcs, target_bodies)]
    analyzer = RomAnalyzer(
        rom, [caller, *targets],
        indirect_dispatch={0x008000: {
            "count": len(targets), "idx_reg": "X", "ptr_call": True,
            "targets": target_pcs}})
    solver = MxAnalysisSolver(analyzer)
    seeds = [Seed(caller.variant, "synthetic caller")]
    if reverse_seeds:
        seeds += [Seed(entry.variant, "synthetic target") for entry in reversed(targets)]
    for seed in seeds:
        solver.add_seed(seed)
    return solver.solve(), caller.variant, [entry.variant for entry in targets]


def continuation_demands(result, caller):
    return {d.target for d in result.nodes[caller].demands
            if d.kind == "indirect-continuation"}


def test_ab_indirect_jsr_mode_changing_singleton_demands_exit_mode():
    result, caller, _ = solve_indirect_rom(
        [bytes([0xC2, 0x10, 0x60])], bytes([0x60]))  # REP #X; RTS
    assert continuation_demands(result, caller) == {v(0x808003, 1, 0)}
    assert v(0x808003, 1, 1) not in result.nodes


def test_ac_two_targets_two_exact_exits_demand_both_continuations():
    result, caller, _ = solve_indirect_rom(
        [bytes([0xC2, 0x10, 0x60]), bytes([0x60])], bytes([0x60]))
    assert continuation_demands(result, caller) == {
        v(0x808003, 1, 0), v(0x808003, 1, 1)}


def test_ad_exact_plus_no_exit_keeps_only_exact_continuation():
    result, caller, _ = solve_indirect_rom(
        [bytes([0xC2, 0x10, 0x60]), bytes([0x80, 0xFE])], bytes([0x60]))
    assert continuation_demands(result, caller) == {v(0x808003, 1, 0)}


def test_ae_all_no_exit_has_no_continuation():
    result, caller, _ = solve_indirect_rom(
        [bytes([0x80, 0xFE]), bytes([0x80, 0xFE])], bytes([0x60]))
    assert not continuation_demands(result, caller)
    assert result.nodes[caller].exit_fact.state == FactState.NO_EXIT


def test_af_exact_plus_unknown_keeps_known_continuation_and_uncertainty():
    result, caller, _ = solve_indirect_rom(
        [bytes([0xC2, 0x10, 0x60]), bytes([0x40])], bytes([0x60]))
    assert continuation_demands(result, caller) == {v(0x808003, 1, 0)}
    fact = result.nodes[caller].exit_fact
    assert fact.state == FactState.UNKNOWN and fact.modes == frozenset({(1, 0)})
    assert v(0x808003, 1, 1) not in result.nodes


def test_ag_set_exit_demands_each_member():
    # BNE forks: REP #$30;RTS => M0X0, or REP #$10;RTS => M1X0.
    callee = bytes([0xD0, 0x03, 0xC2, 0x30, 0x60, 0xC2, 0x10, 0x60])
    result, caller, targets = solve_indirect_rom([callee], bytes([0x60]))
    assert result.nodes[targets[0]].exit_fact.state == FactState.SET
    assert continuation_demands(result, caller) == {
        v(0x808003, 0, 0), v(0x808003, 1, 0)}


def test_ah_discovery_order_is_independent_and_reprocesses():
    bodies = [bytes([0xC2, 0x10, 0x60]), bytes([0x60])]
    first, caller, targets = solve_indirect_rom(bodies, bytes([0x60]))
    second, _, _ = solve_indirect_rom(bodies, bytes([0x60]), reverse_seeds=True)
    assert {key: node.exit_fact for key, node in first.nodes.items()} == {
        key: node.exit_fact for key, node in second.nodes.items()}
    assert first.nodes[caller].process_count >= 2
    assert all(caller in first.reverse_dependencies[target] for target in targets)


def test_ai_unknown_plus_no_exit_invents_no_exact_continuation():
    result, caller, _ = solve_indirect_rom(
        [bytes([0x40]), bytes([0x80, 0xFE])], bytes([0x60]))
    assert not continuation_demands(result, caller)
    assert result.nodes[caller].exit_fact.state == FactState.UNKNOWN
    assert not result.nodes[caller].exit_fact.modes


def test_aj_exact_plus_poison_keeps_known_modes_separate_from_poison():
    node = v(0x80A000)
    first = ExitFact.known({(1, 0)})
    combined = MxAnalysisSolver._merge_fact(first, ExitFact.poison("unsupported"))
    assert combined.state == FactState.POISON
    assert combined.modes == frozenset({(1, 0)})


def test_ak_width_sensitive_continuation_is_redecoded_in_exit_m():
    # Callee clears M. At R, LDA #imm must consume two operand bytes; decoding
    # in the old M1 would instead treat $12 as a separate instruction.
    continuation = bytes([0xA9, 0x34, 0x12, 0x60])
    result, caller, _ = solve_indirect_rom(
        [bytes([0xC2, 0x20, 0x60])], continuation)
    cont = v(0x808003, 0, 1)
    assert continuation_demands(result, caller) == {cont}
    assert result.nodes[cont].exit_fact == ExitFact.known({(0, 1)})
    assert v(0x808003, 1, 1) not in result.nodes
