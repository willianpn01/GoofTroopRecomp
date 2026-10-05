"""Indexed-jump table descriptor derivation.

Inputs: ROM + analysis.Result only.

For each indexed-dispatch site `JMP (T,X)` / `JSR (T,X)` two independent
lines of evidence are computed:

1. Index range (static).  Every backward path that computes X before the
   site (up to its origin: an immediate or memory load) is evaluated forward
   with interval arithmetic through CMP+BCC/BCS guards, AND masks, ASL/LSR,
   CLC/SEC+ADC/SBC, INC/DEC and register transfers.  The union over all paths
   is the set of possible byte offsets.  If any path is not understood, no
   range is claimed.  A range bounded on every path by a constant, guard or
   mask is *proven*; a range bounded only by register width is an upper cap.
2. Table extent (structural).  From the first slot that does not overlap
   decoded code ("bias"), words are accepted while each is a plausible
   in-bank code pointer (>= $8000, not into the middle of a decoded
   instruction) and the slot does not reach: decoded code, another table
   start, an explicit long data reference, or the lowest handler address the
   table itself points at.  A slot whose every walked handler is garbage
   (all paths end in BRK/COP/STP/WDM) also ends the table.

Classes: J1 STATICALLY_PROVEN (proven index range, consistent with the
structural extent), J2 STATICALLY_INFERRED_HIGH_CONFIDENCE (structural
extent terminated by hard evidence), J5 UNRESOLVED.  J3 RUNTIME_OBSERVED
and J4 REQUIRES_PROJECT_HINT are reserved for recomp_hints.json rows and are
never produced here.
"""

from __future__ import annotations

from dataclasses import dataclass

MAX_ENTRIES = 128
MAX_BIAS = 4
MAX_DEPTH = 16
MAX_PATHS = 64
HARD_STOPS = ("code", "handler", "boundary", "bad_handler")


@dataclass(frozen=True)
class Decision:
    bank: int
    site: int
    operand: int
    bias: int
    count: int
    cls: str          # J1..J5
    method: str       # short machine tag
    note: str = ""

    @property
    def start(self) -> int:
        return (self.operand + 2 * self.bias) & 0xFFFF


# ----------------------------------------------------------------------
# 1. static index range

def _insn_at(res, bank, pc):
    modes = res.insns[bank].get(pc, {})
    decs = {(i.mn, i.mode, i.operand, i.length) for i in modes.values()}
    if len(decs) != 1:
        return None, None
    mode = sorted(modes)[0]
    return modes[mode], mode


def _paths(res, bank, site):
    """Backward paths from the site to the load that originates X.  Each path
    is a list of (insn, edge_kind_to_next, (m, x)) oldest first; None if not
    enumerable.  The register feeding X is tracked backwards through
    transfers (TAX: X<-A, ...), so unrelated loads do not end a path."""
    loads = {"LDA": "A", "PLA": "A", "TDC": "A", "TSC": "A",
             "LDX": "X", "PLX": "X", "TSX": "X", "LDY": "Y", "PLY": "Y"}
    out = []
    stack = [(site, [], "X")]
    while stack:
        pc, acc, need = stack.pop()
        preds = res.preds[bank].get(pc, set())
        if not preds or len(acc) >= MAX_DEPTH:
            return None
        for ppc, kind in sorted(preds):
            ins, mode = _insn_at(res, bank, ppc)
            if ins is None or any(p[0].pc == ppc for p in acc):
                return None
            path = [(ins, kind, mode)] + acc
            mn = ins.mn
            if loads.get(mn) == need:
                out.append(path)
                continue
            nneed = need
            if mn in ("TAX", "TAY", "TXA", "TYA", "TXY", "TYX") and mn[2] == need:
                nneed = mn[1]
            stack.append((ppc, path, nneed))
            if len(out) + len(stack) > MAX_PATHS:
                return None
    return out


def _eval(path):
    """Interval of X (byte offset) at the site along one path, or None.
    Returns (lo, hi, bounded, assume_b0): bounded means const/guard/mask;
    assume_b0 means an 8-bit A was moved into a 16-bit X (hidden B assumed 0)."""
    first, _, (m0, x0) = path[0]
    reg = {"LDA": "A", "PLA": "A", "LDX": "X", "PLX": "X", "LDY": "Y", "PLY": "Y"}.get(first.mn)
    if reg is None:
        return None
    width = 0xFF if (m0 if reg == "A" else x0) else 0xFFFF
    if first.mode in ("immM", "immX"):
        lo = hi = first.operand
        bounded = True
    else:
        lo, hi, bounded = 0, width, False
    carry = None
    last_cmp = None
    assume_b0 = False
    for ins, edge, (m, x) in path[1:]:
        mn = ins.mn
        if mn in ("CMP", "CPX", "CPY") and ins.mode in ("immM", "immX"):
            r = {"CMP": "A", "CPX": "X", "CPY": "Y"}[mn]
            last_cmp = ins.operand if r == reg else None
            continue
        if mn in ("BCS", "BCC"):
            if last_cmp is not None:
                below = (mn == "BCS" and edge == "seq") or (mn == "BCC" and edge == "taken")
                if below:
                    hi = min(hi, last_cmp - 1)
                    bounded = True
                else:
                    lo = max(lo, last_cmp)
            last_cmp = None
            continue
        if mn in ("BEQ", "BNE", "BMI", "BPL", "BVC", "BVS", "BRA", "BRL", "JMP"):
            continue
        last_cmp = None
        if mn in ("CLC", "SEC"):
            carry = 0 if mn == "CLC" else 1
            continue
        if reg == "A":
            if mn == "AND" and ins.mode == "immM":
                lo, hi = 0, min(hi, ins.operand)
                bounded = True
                continue
            if mn in ("ASL", "LSR") and ins.mode == "acc":
                lo, hi = (lo * 2, hi * 2) if mn == "ASL" else (lo // 2, hi // 2)
                if hi > (0xFF if m else 0xFFFF):
                    return None
                carry = None
                continue
            if mn in ("ADC", "SBC") and ins.mode == "immM" and carry is not None:
                d = ins.operand + carry if mn == "ADC" else -(ins.operand + 1 - carry)
                if lo + d < 0:
                    return None
                lo, hi, carry = lo + d, hi + d, None
                continue
            if mn in ("INC", "DEC") and ins.mode == "acc":
                d = 1 if mn == "INC" else -1
                if lo + d < 0:
                    return None
                lo, hi = lo + d, hi + d
                continue
            if mn in ("TAX", "TAY"):
                if m and not x:
                    assume_b0 = True     # 16-bit index takes the hidden B byte; assume 0
                reg = mn[2]
                if x:
                    hi = min(hi, 0xFF)
                continue
            if mn in ("STA", "STZ", "STX", "STY", "PHA", "PHP", "PHB", "PHK", "PHD", "PHX", "PHY",
                      "PLB", "SEP", "REP", "NOP", "CPX", "CPY", "BIT", "LDY"):
                continue
            return None
        # reg is X or Y
        if mn in ("INX", "DEX", "INY", "DEY"):
            if mn[-1] == reg:
                d = 1 if mn.startswith("IN") else -1
                if lo + d < 0:
                    return None
                lo, hi = lo + d, hi + d
            continue
        if mn in ("TXA", "TYA", "TXY", "TYX"):
            if mn[1] == reg or mn[2] == reg:
                return None              # tracked value copied/overwritten: not modelled
            continue
        if mn in ("STA", "STX", "STY", "STZ", "PHA", "PHX", "PHY", "PHP", "PHB", "PHK", "PLB",
                  "SEP", "REP", "NOP", "LDA", "CMP", "AND", "ORA", "EOR", "ASL", "LSR", "ADC", "SBC",
                  "INC", "DEC", "BIT", "XBA", "PLA", "TAY" if reg == "X" else "TAX",
                  "LDY" if reg == "X" else "LDX"):
            continue
        return None
    if reg != "X":
        return None
    return lo, hi, bounded, assume_b0


def index_range(res, bank, site):
    """(lo, hi, kind) byte-offset interval of X at the site, or None.
    kind: 'proven' (bounded on every path, no assumption), 'masked' (bounded
    but relying on hidden B = 0), 'width' (register width only)."""
    paths = _paths(res, bank, site)
    if not paths:
        return None
    vals = [_eval(p) for p in paths]
    if any(v is None for v in vals):
        return None
    lo = min(v[0] for v in vals)
    hi = max(v[1] for v in vals)
    if lo > hi:
        return None
    if not all(v[2] for v in vals):
        return lo, hi, "width"
    return lo, hi, "masked" if any(v[3] for v in vals) else "proven"


# ----------------------------------------------------------------------
# 2. table extent

def _plausible_target(res, bank, t):
    if not 0x8000 <= t <= 0xFFFF:
        return False
    if t in res.covered[bank] and t not in res.starts[bank]:
        return False           # points inside a decoded instruction
    return True


def extent(res, rom, bank, start, stops, limit):
    """Count consecutive plausible pointer words from `start` -> (count, reason)."""
    cov = res.covered[bank]
    min_target = None
    k = 0
    while k < limit:
        a = (start + 2 * k) & 0xFFFF
        if a < 0x8000:
            return k, "bank_end"
        if k and a in stops:
            return k, "boundary"
        if a in cov or ((a + 1) & 0xFFFF) in cov:
            return k, "code"
        if min_target is not None and a >= min_target:
            return k, "handler"
        w = rom.w(bank, a)
        if not _plausible_target(res, bank, w):
            return k, "invalid_word"
        if w > start and (min_target is None or w < min_target):
            min_target = w
        k += 1
    return k, "limit"


def decide(res, rom, stops_by_bank, prev=None):
    """One Decision per discovered site.  prev: (bank, site) -> previous
    Decision (to map walked handler indices onto the current bias)."""
    out = {}
    prev = prev or {}
    for (bank, site), s in sorted(res.sites.items()):
        cov = res.covered[bank]
        if s.operand < 0x8000:
            out[(bank, site)] = Decision(bank, site, s.operand, 0, 0, "J5", "ram_table",
                                         "table operand is not in ROM space (dynamic)")
            continue
        bias = 0
        while bias < MAX_BIAS:
            a = (s.operand + 2 * bias) & 0xFFFF
            if a in cov or ((a + 1) & 0xFFFF) in cov:
                bias += 1
                continue
            break
        if bias >= MAX_BIAS:
            out[(bank, site)] = Decision(bank, site, s.operand, 0, 0, "J5", "no_free_slot")
            continue
        start = (s.operand + 2 * bias) & 0xFFFF
        stops = stops_by_bank[bank] - {start}
        n_ext, why = extent(res, rom, bank, start, stops, MAX_ENTRIES)
        # handler plausibility (targets walked in the previous iteration)
        pd = prev.get((bank, site))
        shift = (pd.bias - bias) if pd else 0
        per_index: dict = {}
        for i, tv in res.table_targets.get((bank, site), ()):
            per_index.setdefault(i + shift, []).append(tv)
        for i in sorted(per_index):
            if 0 <= i < n_ext and per_index[i] and all(v in res.bad_variants for v in per_index[i]):
                n_ext, why = i, "bad_handler"
                break
        rng = index_range(res, bank, site)
        if rng is not None and rng[0] % 2 == 0 and rng[1] % 2 == 0 and rng[1] < 2 * MAX_ENTRIES:
            lo_i, hi_i, kind = rng[0] // 2, rng[1] // 2, rng[2]
            if kind == "masked" and bias <= lo_i and hi_i < bias + n_ext:
                why += "+index_masked"          # corroborates the extent (J2)
            elif kind == "proven":
                if bias <= lo_i and hi_i < bias + n_ext:
                    out[(bank, site)] = Decision(bank, site, s.operand, bias, n_ext, "J1",
                                                 f"extent/{why}+index_proven",
                                                 f"index {lo_i}..{hi_i} within extent")
                    continue
                ok = lo_i >= bias and all(
                    _plausible_target(res, bank, rom.w(bank, (s.operand + 2 * i) & 0xFFFF))
                    for i in range(bias, hi_i + 1))
                if ok:
                    out[(bank, site)] = Decision(bank, site, s.operand, bias, hi_i - bias + 1, "J1",
                                                 "index_proven",
                                                 f"index {lo_i}..{hi_i}; structural extent {n_ext}/{why}")
                    continue
                out[(bank, site)] = Decision(bank, site, s.operand, bias, n_ext, "J5", "conflict",
                                             f"proven index {lo_i}..{hi_i} vs extent {bias}+{n_ext}/{why}")
                continue
            if kind == "width" and hi_i < bias + n_ext:   # width-only cap
                n_ext, why = hi_i - bias + 1, why + "+width_cap"
        if n_ext <= 0:
            out[(bank, site)] = Decision(bank, site, s.operand, bias, 0, "J5", f"extent0/{why}")
            continue
        cls = "J2" if why.split("+")[0] in HARD_STOPS else "J5"
        out[(bank, site)] = Decision(bank, site, s.operand, bias, n_ext, cls, f"extent/{why}")
    return out
