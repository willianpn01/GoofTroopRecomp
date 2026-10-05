"""ROM-only 65816 control-flow analysis.

Inputs: canonical ROM bytes and a table-decision map produced by
jump_tables.  Nothing else.

Model
-----
* A *variant* is (bank, pc, m, x, unc): a callable entry reached with a mode;
  `unc` marks M/X bits known only through an ambiguous callee exit
  (bit0 = M, bit1 = X).
* Each variant is walked intra-procedurally (branches, JMP abs followed;
  PHP/PLP tracked on a small mode stack; REP/SEP/XCE applied).
* Calls (JSR/JSL/JSR (abs,X)) continue with the callee's *exit modes*
  (computed recursively; a call cycle falls back to the caller mode).  If a
  callee can exit with both values of a flag, that flag becomes uncertain in
  the caller until a REP/SEP re-establishes it.  When the call-site mode is
  one of the possible exits, only that (mode-preserving) continuation is
  followed; otherwise every exit is followed and marked unlikely (bit2).  An entry trusts only its best-ranked variants
  (certain > likely > unlikely).
* A callee that performs a context switch (task yield) resumes its caller
  later with the caller's saved P, so the caller keeps its own mode.
* Tail transfers (JML, JMP (abs,X)) contribute the target's exit modes.
* Trust: a walk state is *doomed* when every path from it ends in
  BRK/COP/STP/WDM or leaves ROM space (garbage decoding).  A variant whose
  entry state is doomed is *bad*.  A variant is *good* when a root or a good
  variant registered it from a non-doomed state.  Aggregates (coverage,
  flow, entries, modes, sites) come from non-doomed states of good variants.
* TCS/TXS with a value loaded from memory switches to another stack: the
  PHP model is cleared, so a later PLP pops an unknown frame and forks all
  four modes as uncertain states (garbage decodings are pruned as doomed;
  state-level ranking prefers certain decodings of shared code such as the
  task scheduler, which the reset path reaches with certain modes).  Stack
  init (LDX #imm : TXS) and frame arithmetic (TSC ... TCS) keep the model.
* State-level ranking: at each pc only the best-ranked non-doomed states
  (certain > likely > unlikely) contribute coverage, flow, sites; an
  uncertain decoding misaligned with a certain instruction is discarded.
* Certain-vs-certain misalignment (a real but data-dependent mode leak into
  a routine): the decoding whose variant has less call-site support (tie:
  more misalignments) is discarded from the conflict onward.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .cpu65816 import BRANCHES, CODE_BANKS, HALTS, RETURNS, Rom, branch_target

PHP_DEPTH = 6


def fold_bank(bank: int) -> int:
    """Physical LoROM bank for a 24-bit bank byte in the 512 KiB ROM."""
    return (bank & 0x7F) % 16


@dataclass
class Site:
    bank: int
    pc: int
    op: int                  # 0x7C JMP (abs,X) / 0xFC JSR (abs,X)
    operand: int
    modes: set = field(default_factory=set)
    certain_modes: set = field(default_factory=set)


@dataclass
class Walk:
    """One variant's walk as a state graph; state key = (pc, m, x, php, unc)."""
    visits: list = field(default_factory=list)     # (key, Insn)
    succ: dict = field(default_factory=dict)       # key -> [key]
    halts: set = field(default_factory=set)        # keys that execute BRK/COP/STP/WDM or leave ROM
    flow: list = field(default_factory=list)       # (key, src_pc, target_pc)
    preds: list = field(default_factory=list)      # (pred state key, pc, pred_pc, kind)
    sites: list = field(default_factory=list)      # (key, site_pc, op, operand, m, x, unc)
    data_refs: list = field(default_factory=list)  # (key, bank, pc)
    indirect: list = field(default_factory=list)   # (key, pc, mnemonic, operand)
    doomed: set = field(default_factory=set)
    bad: str = ""
    switched: bool = False


def _rank(unc: int) -> int:
    """0 certain, 1 uncertain but mode-preserving guess, 2 non-preserving guess."""
    return 0 if not unc else (2 if unc & 4 else 1)


def _doomed(w: Walk) -> set:
    """States from which every intra-walk path ends in a halt."""
    doomed = set(w.halts)
    preds: dict = {}
    for k, ss in w.succ.items():
        for t in ss:
            preds.setdefault(t, set()).add(k)
    stack = list(doomed)
    while stack:
        t = stack.pop()
        for k in preds.get(t, ()):
            if k not in doomed and all(s in doomed for s in w.succ.get(k, ())):
                doomed.add(k)
                stack.append(k)
    return doomed


class Result:
    """Trusted aggregates (built from untainted variants)."""

    def __init__(self):
        B = CODE_BANKS
        self.entries = {b: {} for b in B}        # bank -> pc -> set((m,x))
        self.certain = {}                        # (bank, pc) -> set((m,x)) certain registrations
        self.reasons = {b: {} for b in B}
        self.insns = {b: {} for b in B}          # bank -> pc -> {(m,x): Insn}
        self.covered = {b: set() for b in B}
        self.starts = {b: set() for b in B}
        self.flow = {b: [] for b in B}
        self.state_modes = {b: {} for b in B}
        self.state_certain = {b: {} for b in B}
        self.sites = {}
        self.data_refs = {b: set() for b in B}
        self.preds = {b: {} for b in B}
        self.exits = {}
        self.walks = {}
        self.registrars = {}                     # variant -> set(registering variant | None)
        self.reg_reason = {}                     # (variant, registrar) -> reason
        self.bad_variants = set()
        self.tainted = set()
        self.context_switch = set()
        self.far_aliases = set()
        self.notes = []
        self.conflict_dropped = {}               # variant -> states dropped by certain-conflict rule
        self.table_targets = {}                  # (bank, site) -> list[(index, variant)]


class Analyzer:
    def __init__(self, rom: Rom, tables: dict):
        """tables: (bank, site_pc) -> (table_start_pc, count)."""
        self.rom = rom
        self.tables = tables
        self.r = Result()
        self.in_progress: set = set()

    # ------------------------------------------------------------------
    def register(self, src, bank: int, pc: int, m: int, x: int, why: str, unc: int = 0):
        if bank not in CODE_BANKS or not 0x8000 <= pc <= 0xFFFF:
            return None
        var = (bank, pc, m, x, unc)
        self.r.registrars.setdefault(var, set()).add(src)
        self.r.reg_reason.setdefault((var, src), set()).add(why)
        return var

    def summary(self, var, caller_mode: tuple) -> frozenset:
        """Exit modes of a variant; walks it on first use."""
        if var is None:
            return frozenset({caller_mode})
        if var in self.r.exits:
            ex = self.r.exits[var]
        elif var in self.in_progress:
            return frozenset({caller_mode})        # recursion: assume mode preserved
        else:
            self.in_progress.add(var)
            ex = self.walk(var)
            self.in_progress.discard(var)
            self.r.exits[var] = ex
        return ex if ex else frozenset({caller_mode})

    def long_target(self, bank_byte: int, pc: int) -> tuple[int, int]:
        pb = fold_bank(bank_byte)
        if pb != (bank_byte & 0x7F) and pb in CODE_BANKS:
            self.r.far_aliases.add(((bank_byte << 16) | pc, pb, pc))
        return pb, pc

    # ------------------------------------------------------------------
    def walk(self, var) -> frozenset:
        bank, entry, m0, x0, unc0 = var
        rom = self.rom
        w = Walk()
        self.r.walks[var] = w
        exits: set = set()
        seen: set = set()
        # work item: (pc, m, x, php, unc, pred(pc, kind) | None, parent state key | None)
        work = [(entry, m0, x0, (), unc0, None, None)]
        while work:
            pc, m, x, php, unc, prev, parent = work.pop()
            src = {"A": None, "X": None}                  # provenance of A / X in this block
            while True:
                key = (pc, m, x, php, unc)
                if parent is not None:
                    w.succ.setdefault(parent, []).append(key)
                if prev is not None:
                    w.preds.append((parent, pc) + prev)
                    prev = None
                if key in seen:
                    break
                seen.add(key)
                if not 0x8000 <= pc <= 0xFFFF:
                    w.halts.add(key)
                    break
                ins = rom.decode(bank, pc, m, x)
                w.visits.append((key, ins))
                parent = key
                op, mode = ins.op, ins.mode
                nxt = (pc + ins.length) & 0xFFFF
                if op in HALTS:
                    w.halts.add(key)
                    break
                # Only explicit-bank (long) operands are data references into a
                # code bank; an absolute operand depends on DB, which this game
                # mostly points at WRAM or data banks.
                if mode in ("long", "longx") and ins.mn not in ("JML", "JSL"):
                    pb = fold_bank(ins.operand >> 16)
                    if pb in CODE_BANKS and (ins.operand & 0xFFFF) >= 0x8000 and (ins.operand >> 16) & 0x7F < 0x40:
                        w.data_refs.append((key, pb, ins.operand & 0xFFFF))
                if op in (0xC2, 0xE2):                    # REP / SEP
                    v = 0 if op == 0xC2 else 1
                    if ins.operand & 0x20:
                        m, unc = v, unc & ~1
                    if ins.operand & 0x10:
                        x, unc = v, unc & ~2
                    if not unc & 3:
                        unc = 0                           # both flags re-established
                elif op == 0xFB:                          # XCE (only used to enter native mode)
                    m, x, unc = 1, 1, 0
                elif op == 0x08:
                    php = (php + ((m, x, unc),))[-PHP_DEPTH:]
                elif op == 0x28:                          # PLP
                    if php:
                        (m, x, unc), php = php[-1], php[:-1]
                    else:
                        # P from a frame we did not push (e.g. after a stack
                        # switch): fork every mode as uncertain; doomed-state
                        # pruning discards the decodings that are garbage.
                        for mm in (0, 1):
                            for xx in (0, 1):
                                work.append((nxt, mm, xx, php, 3, (pc, "seq"), key))
                        break
                elif op in (0x1B, 0x9A):                  # TCS / TXS
                    if src["A" if op == 0x1B else "X"] == "mem":
                        # S loaded from memory: switch to another stack.  Saved
                        # P frames now belong to the other context, so a later
                        # PLP pops an unknown frame (forks all modes).
                        w.switched = True
                        php = ()
                if ins.mn in ("LDA", "PLA", "LDX", "PLX"):
                    src["A" if ins.mn in ("LDA", "PLA") else "X"] = (
                        "imm" if mode in ("immM", "immX") else "mem")
                elif ins.mn in ("TSC", "TDC", "TXA", "TYA"):
                    src["A"] = "reg"
                elif ins.mn in ("TSX", "TAX", "TYX"):
                    src["X"] = "reg"
                if op in RETURNS:
                    exits.add((m, x))
                    break
                if op in BRANCHES or op in (0x80, 0x82):
                    t = branch_target(ins)
                    w.flow.append((key, pc, t))
                    work.append((t, m, x, php, unc, (pc, "taken"), key))
                    if op in (0x80, 0x82):
                        break
                elif op == 0x4C:                          # JMP abs
                    w.flow.append((key, pc, ins.operand))
                    work.append((ins.operand, m, x, php, unc, (pc, "taken"), key))
                    break
                elif op == 0x5C:                          # JML long (tail)
                    tb, tpc = self.long_target(ins.operand >> 16, ins.operand & 0xFFFF)
                    exits |= self.summary(self.register((var, key), tb, tpc, m, x, "jml", unc), (m, x))
                    break
                elif op in (0x6C, 0xDC):                  # JMP (abs) / JML [abs]
                    w.indirect.append((key, pc, ins.mn, ins.operand))
                    break
                elif op == 0x20:                          # JSR abs
                    outs = self.summary(self.register((var, key), bank, ins.operand, m, x, "jsr", unc), (m, x))
                    self._fanout(work, nxt, outs, php, unc, key, (m, x))
                    break
                elif op == 0x22:                          # JSL long
                    tb, tpc = self.long_target(ins.operand >> 16, ins.operand & 0xFFFF)
                    outs = self.summary(self.register((var, key), tb, tpc, m, x, "jsl", unc), (m, x))
                    self._fanout(work, nxt, outs, php, unc, key, (m, x))
                    break
                elif op in (0x7C, 0xFC):                  # JMP/JSR (abs,X)
                    w.sites.append((key, pc, op, ins.operand, m, x, unc))
                    outs: set = set()
                    tab = self.tables.get((bank, pc))
                    if tab is not None:
                        start, count = tab
                        lst = self.r.table_targets.setdefault((bank, pc), [])
                        for i in range(count):
                            t = rom.w(bank, (start + 2 * i) & 0xFFFF)
                            if t:
                                tv = self.register((var, key), bank, t, m, x, "table", unc)
                                lst.append((i, tv))
                                outs |= self.summary(tv, (m, x))
                    if op == 0x7C:
                        exits |= outs
                        break
                    self._fanout(work, nxt, frozenset(outs) or frozenset({(m, x)}), php, unc, key, (m, x))
                    break
                prev = (pc, "seq")
                pc = nxt
        w.doomed = _doomed(w)
        if (entry, m0, x0, (), unc0) in w.doomed:
            w.bad = "entry_doomed"
            return frozenset()                         # garbage: no exit evidence
        if w.switched:
            # Context switch (task yield): the RTS/RTL reached belongs to
            # another task.  The caller resumes with the P it saved.
            self.r.context_switch.add(var)
            return frozenset()
        return frozenset(exits)

    def _fanout(self, work, nxt, outs, php, unc, key, call_mode):
        amb = (1 if len({m for m, _ in outs}) > 1 else 0) | (2 if len({x for _, x in outs}) > 1 else 0)
        if amb and call_mode in outs:
            outs = (call_mode,)            # preserving exit exists: follow it only
        for mm, xx in sorted(outs):
            u = unc | amb
            if amb and (mm, xx) != call_mode:
                u |= 4                     # non-preserving guess: least likely
            work.append((nxt, mm, xx, php, u, None, key))

    # ------------------------------------------------------------------
    def _dominated(self, children) -> set:
        r = self.r
        # fixpoint: certain registrations counted only from non-dominated,
        # non-bad registrars reached from roots.
        dominated: set = set()
        while True:
            reach: set = set()
            stack = [v for v, srcs in r.registrars.items() if None in srcs and v not in r.bad_variants]
            while stack:
                v = stack.pop()
                if v in reach or v in dominated:
                    continue
                reach.add(v)
                dm = r.walks[v].doomed
                for c, skey in children.get(v, ()):
                    if c not in reach and c not in r.bad_variants and skey not in dm:
                        stack.append(c)
            best: dict = {}
            for v in reach:
                best[(v[0], v[1])] = min(best.get((v[0], v[1]), 9), _rank(v[4]))
            new = {v for v in reach if _rank(v[4]) > best[(v[0], v[1])]}
            if new <= dominated:
                return dominated
            dominated |= new

    def _closure(self, children, dead) -> set:
        """Good variants: reached from roots through registrations made from
        live states (not doomed, not in `dead`) of good, non-dominated variants."""
        r = self.r
        dominated = self._dominated(children)
        good: set = set()
        stack = [v for v, srcs in r.registrars.items() if None in srcs and v not in r.bad_variants]
        while stack:
            v = stack.pop()
            if v in good or v in dominated:
                continue
            good.add(v)
            dm = r.walks[v].doomed
            dd = dead.get(v, ())
            for c, skey in children.get(v, ()):
                if c not in good and c not in r.bad_variants and skey not in dm and skey not in dd:
                    stack.append(c)
        return good

    def _certain_conflicts(self, good, dead) -> dict:
        """Misaligned *certain* decodings of the same bytes: the decoding whose
        variant has less call-site support loses, together with every state
        downstream of the conflict in that walk.  Returns var -> set(keys)."""
        r = self.r
        support = {v: len(r.registrars.get(v, ())) for v in good}
        starts: dict = {}
        for var in good:
            w = r.walks[var]
            dd = dead.get(var, ())
            for key, ins in w.visits:
                if key in w.doomed or key in dd or key[4]:
                    continue
                starts.setdefault((var[0], key[0]), []).append((var, key, ins.length))
        pairs = []
        nconf: dict = {}
        for (b, pc), lst in starts.items():
            for var, key, ln in lst:
                for i in range(1, ln):
                    for var2, key2, _ln2 in starts.get((b, (pc + i) & 0xFFFF), ()):
                        pairs.append((var, key, var2, key2))
                        nconf[var] = nconf.get(var, 0) + 1
                        nconf[var2] = nconf.get(var2, 0) + 1
        losers: dict = {}
        for var, key, var2, key2 in pairs:
            # less call-site support loses; tie -> more misalignments loses
            a_ = (support[var], -nconf[var])
            b_ = (support[var2], -nconf[var2])
            if a_ < b_:
                losers.setdefault(var, set()).add(key)
            elif b_ < a_:
                losers.setdefault(var2, set()).add(key2)
        out = {}
        for var, keys in losers.items():
            w = r.walks[var]
            seen = set(keys)
            stack = list(keys)
            while stack:
                k = stack.pop()
                for t in w.succ.get(k, ()):
                    if t not in seen:
                        seen.add(t)
                        stack.append(t)
            out[var] = seen
        return out

    def finish(self):
        r = self.r
        r.bad_variants = {v for v, w in r.walks.items() if w.bad}
        children: dict = {}
        for v, srcs in r.registrars.items():
            for s in srcs:
                if s is not None:
                    children.setdefault(s[0], []).append((v, s[1]))
        dead: dict = {}
        for _ in range(6):
            good = self._closure(children, dead)
            new = self._certain_conflicts(good, dead)
            merged = {v: set(dead.get(v, ())) | new.get(v, set()) for v in set(dead) | set(new)}
            if merged == dead:
                break
            dead = merged
        r.conflict_dropped = dead
        r.tainted = set(r.walks) - good
        best: dict = {}
        for var in good:
            w = r.walks[var]
            dd = dead.get(var, ())
            for key, _ins in w.visits:
                if key not in w.doomed and key not in dd:
                    k = (var[0], key[0])
                    best[k] = min(best.get(k, 9), _rank(key[4]))

        # certain instruction layout, used to reject misaligned uncertain decodings
        cstart = {b: set() for b in CODE_BANKS}
        ccov = {b: set() for b in CODE_BANKS}
        for var in good:
            w = r.walks[var]
            dd = dead.get(var, ())
            for key, ins in w.visits:
                if key not in w.doomed and key not in dd and not key[4]:
                    cstart[var[0]].add(key[0])
                    ccov[var[0]].update((key[0] + i) & 0xFFFF for i in range(ins.length))
        lens = {}
        for var in good:
            for key, ins in r.walks[var].visits:
                lens[(var[0], key)] = ins.length

        def keep(b, key, dm):
            if key in dm or _rank(key[4]) != best.get((b, key[0]), 9):
                return False
            if key[4]:
                pc = key[0]
                if pc in ccov[b] and pc not in cstart[b]:
                    return False                  # starts inside a certain instruction
                if any(((pc + i) & 0xFFFF) in cstart[b] for i in range(1, lens[(b, key)])):
                    return False                  # swallows a certain instruction start
            return True

        for var in sorted(good):
            b = var[0]
            w = r.walks[var]
            dm = w.doomed | dead.get(var, set())
            pc0, m0, x0, unc0 = var[1], var[2], var[3], var[4]
            r.entries[b].setdefault(pc0, set()).add((m0, x0))
            if not unc0:
                r.certain.setdefault((b, pc0), set()).add((m0, x0))
            for s in r.registrars[var]:
                if s is None or (s[0] in good and s[1] not in r.walks[s[0]].doomed
                                 and s[1] not in dead.get(s[0], ())):
                    r.reasons[b].setdefault(pc0, set()).update(r.reg_reason[(var, s)])
            for key, ins in w.visits:
                if not keep(b, key, dm):
                    continue
                pc, m, x, _php, unc = key
                r.insns[b].setdefault(pc, {})[(m, x)] = ins
                r.starts[b].add(pc)
                r.state_modes[b].setdefault(pc, set()).add((m, x))
                if not unc:
                    r.state_certain[b].setdefault(pc, set()).add((m, x))
                for i in range(ins.length):
                    r.covered[b].add((pc + i) & 0xFFFF)
            r.flow[b].extend((src, t) for key, src, t in w.flow if keep(b, key, dm))
            for key, pc, ppc, kind in w.preds:
                if key is not None and keep(b, key, dm):
                    r.preds[b].setdefault(pc, set()).add((ppc, kind))
            for key, bb, a in w.data_refs:
                if keep(b, key, dm):
                    r.data_refs[bb].add(a)
            for key, pc, mn, operand in w.indirect:
                if keep(b, key, dm):
                    r.notes.append(f"{b:02X}:{pc:04X} unresolved indirect {mn} ({operand:04X})")
            for key, pc, op, operand, m, x, unc in w.sites:
                if not keep(b, key, dm):
                    continue
                s = r.sites.setdefault((b, pc), Site(b, pc, op, operand))
                s.modes.add((m, x))
                if not unc:
                    s.certain_modes.add((m, x))
        return r


def analyse(rom: Rom, tables: dict, extra_roots=()) -> Result:
    """extra_roots: iterable of (bank, pc, m, x, reason) derived by rules
    outside the walk (e.g. derive_entries.pointer_literals)."""
    an = Analyzer(rom, tables)
    for pc in rom.vectors():
        an.summary(an.register(None, 0, pc, 1, 1, "vector"), (1, 1))
    for b, pc, m, x, why in sorted(extra_roots):
        an.summary(an.register(None, b, pc, m, x, why), (m, x))
    while True:
        pending = sorted(set(an.r.registrars) - set(an.r.exits))
        if not pending:
            break
        for var in pending:
            an.summary(var, var[2:4])
    return an.finish()
