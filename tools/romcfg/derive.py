"""Derivation orchestrator: canonical ROM bytes -> (tables, entries, modes).

Allowed inputs: the canonical ROM bytes and optional extra roots (entry
hints from recomp_hints.json).  Nothing is read from disk here.
"""

from __future__ import annotations

import sys
import threading

from . import analysis
from . import entries as derive_entries
from . import jump_tables as derive_jump_tables
from . import modes as derive_modes
from .cpu65816 import CODE_BANKS

MAX_ITER = 40
# The callee-exit summaries recurse along call chains.  Run the derivation in
# a thread with an explicit stack so the depth does not depend on the host's
# default main-thread stack (Windows: 1 MiB).  The supported ROM needs fewer
# than 1,000 frames; both values are safety margins.
RECURSION_LIMIT = 20000
THREAD_STACK = 256 * 1024 * 1024


def _stops(res, tables):
    stops = {b: set(res.data_refs[b]) for b in CODE_BANKS}
    for (b, _site), (start, count) in tables.items():
        if count:
            stops[b].add(start)
    return stops


def derive(rom, log=None, hint_roots=()):
    """Iterate analysis <-> table decisions <-> pointer-literal roots to a
    fixpoint.  Fail-closed: no fixpoint within MAX_ITER raises.

    hint_roots: iterable of (bank, pc, m, x, reason) project entry hints."""
    tables: dict = {}
    decisions: dict = {}
    fixed = set(hint_roots)
    roots: set = set(fixed)
    for it in range(MAX_ITER):
        res = analysis.analyse(rom, tables, roots)
        decisions = derive_jump_tables.decide(res, rom, _stops(res, tables), decisions)
        new = {k: (d.start, d.count) for k, d in decisions.items() if d.count}
        new_roots = roots | derive_entries.pointer_literals(res, decisions)
        if log is not None:
            log.append(f"iter {it}: sites={len(res.sites)} tables={len(new)} "
                       f"entries={sum(len(e) for e in res.entries.values())} roots={len(new_roots)}")
        if new == tables and new_roots == roots:
            break
        tables, roots = new, new_roots
    else:
        raise RuntimeError(f"derivation did not reach a fixpoint in {MAX_ITER} iterations")
    entries, reasons = derive_entries.partition(res, decisions)
    modes = derive_modes.entry_modes(res, entries)
    return {"analysis": res, "decisions": decisions, "tables": tables,
            "roots": sorted(roots - fixed), "hint_roots": sorted(fixed),
            "entries": entries, "reasons": reasons, "modes": modes}


def run(rom, log=None, hint_roots=()):
    """derive() on a worker thread with a fixed stack and recursion limit."""
    box: dict = {}

    def work():
        try:
            box["result"] = derive(rom, log, hint_roots)
        except BaseException as exc:          # noqa: BLE001 - re-raised below
            box["error"] = exc

    old_limit = sys.getrecursionlimit()
    old_stack = threading.stack_size()
    sys.setrecursionlimit(max(old_limit, RECURSION_LIMIT))
    threading.stack_size(THREAD_STACK)
    try:
        t = threading.Thread(target=work, name="romcfg-derive")
        t.start()
        t.join()
    finally:
        threading.stack_size(old_stack)
        sys.setrecursionlimit(old_limit)
    if "error" in box:
        raise box["error"]
    return box["result"]
