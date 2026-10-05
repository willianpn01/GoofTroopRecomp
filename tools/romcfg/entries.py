"""Entry-set derivation: pointer-literal roots and the partition rule.

Callable entries come from the analysis (vectors, JSR/JSL/JML targets, table
targets).  The partition rule then promotes every branch/JMP target that
leaves the address range of the function that owns its source ("shared
flow"), so each function is a contiguous [entry, next entry) range.  Pure
function of ROM-decoded flow; no labels are involved.
"""

from __future__ import annotations

import bisect

from .cpu65816 import CODE_BANKS

TERMINAL = ("RTS", "RTL", "RTI", "JMP", "JML", "BRA", "BRL")


def _stored_to_ram(res, b, pc, ins, m, x):
    """True when the literal loaded at pc is immediately stored to RAM by the
    same register (STA/STX/STY dp | abs < $2000 | long $7E/$7F)."""
    nxt = res.insns[b].get((pc + ins.length) & 0xFFFF, {})
    st = nxt.get((m, x))
    want = {"LDA": "STA", "LDX": "STX", "LDY": "STY"}.get(ins.mn)
    if st is None or st.mn != want:
        return False
    if st.mode in ("dp", "dpx", "dpy"):
        return True
    if st.mode in ("abs", "absx", "absy"):
        return st.operand < 0x2000
    if st.mode in ("long", "longx"):
        return (st.operand >> 16) in (0x7E, 0x7F)
    return False


def pointer_literals(res, decisions):
    """Code-pointer literal rule.

    A 16-bit literal that is stored to RAM right away (LDA/LDX/LDY #imm16
    followed by the matching STA/STX/STY to direct page, low WRAM or bank
    $7E/$7F) or pushed (PEA), with value v designating t = v + 1 (RTS
    trampoline convention: pushed, RTS adds one) or t = v (JMP convention),
    such that
      * t is in the same code bank, below the header area ($FFB0),
      * t is not covered by any decoded instruction nor by a jump table,
      * the decoded instruction just before t is terminal (RTS/RTL/RTI/JMP/
        JML/BRA/BRL, not an indexed JMP) and ends exactly at t,
    marks t as an entry reached through a stored code pointer.  The entry
    mode is the certain mode of that terminal instruction.  Returns a set of
    (bank, pc, m, x, reason)."""
    out = set()
    tab = {b: set() for b in CODE_BANKS}
    for d in decisions.values():
        tab[d.bank].update((d.operand + i) & 0xFFFF for i in range(2 * (d.bias + max(d.count, 1))))
    for b in CODE_BANKS:
        cov = res.covered[b]
        ends = {}
        for pc, modes in res.insns[b].items():
            ins = next(iter(modes.values()))
            ends[(pc + ins.length) & 0xFFFF] = (pc, ins)
        for pc, modes in res.insns[b].items():
            for (m, x), ins in modes.items():
                wide = (ins.mode == "immM" and not m) or (ins.mode == "immX" and not x)
                if ins.mn == "PEA":
                    pass
                elif not (wide and _stored_to_ram(res, b, pc, ins, m, x)):
                    continue
                for t, why in (((ins.operand + 1) & 0xFFFF, "ptr_literal_rts"), (ins.operand, "ptr_literal_jmp")):
                    if not 0x8000 <= t < 0xFFB0 or t in cov or t in tab[b] or t not in ends:
                        continue
                    tpc, tins = ends[t]
                    if tins.mn not in TERMINAL or tins.mode == "absix":
                        continue
                    for mm, xx in sorted(res.state_certain[b].get(tpc, ())):
                        out.add((b, t, mm, xx, why))
    return out


def partition(res, decisions):
    """Return bank -> pc -> set((m,x)) after the shared-flow promotion, and
    bank -> pc -> set(reasons)."""
    entries = {b: {pc: set(ms) for pc, ms in res.entries[b].items()} for b in CODE_BANKS}
    reasons = {b: {pc: set(r) for pc, r in res.reasons[b].items()} for b in CODE_BANKS}
    for b in CODE_BANKS:
        tstarts = sorted({d.start for d in decisions.values() if d.bank == b and d.count})
        while True:
            ordered = sorted(entries[b])
            bounds = sorted(set(ordered) | set(tstarts))
            added = []
            for src, t in res.flow[b]:
                i = bisect.bisect_right(ordered, src) - 1
                if i < 0:
                    continue
                owner = ordered[i]
                j = bisect.bisect_right(bounds, owner)
                boundary = bounds[j] if j < len(bounds) else None
                if (t < owner or (boundary is not None and t >= boundary)) and t not in entries[b]:
                    added.append(t)
            if not added:
                break
            for t in added:
                modes = res.state_certain[b].get(t) or res.state_modes[b].get(t) or {(1, 1)}
                entries[b].setdefault(t, set()).update(modes)
                reasons[b].setdefault(t, set()).add("shared-flow")
    return entries, reasons
