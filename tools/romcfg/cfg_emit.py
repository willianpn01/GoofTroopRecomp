"""Emit snesrecomp bank CFG text from a ROM derivation.

One file per code bank:

    bank = NN / exec_bank = 8N / strict_mx
    name <logical24> CODE_<pc24>                  (mirror alias of a long target)
    indirect_dispatch <site> <count> idx:X [tables:<start> idx_bias:<bias>]
    func CODE_<pc24> <pc> [end:<next entry>] entry_mx:<m>,<x>   (one per mode)

Function names are derived from addresses only.  Each function ends where the
next entry starts.  Output depends only on its inputs (sorted, no timestamps).
"""

from __future__ import annotations

from .cpu65816 import CODE_BANKS
from .hints import pc24

HEADER = "# ROM-derived CFG (generate_cfg.py). Local build input: never commit or distribute."


def entry_modes(d, overrides: dict) -> dict:
    """bank -> pc -> sorted list of 'm,x', after project mode hints."""
    out = {b: {pc: list(ms) for pc, ms in d["modes"][b].items()} for b in CODE_BANKS}
    for (b, pc), modes in overrides.items():
        if pc not in out[b]:
            raise ValueError(f"mode hint {pc24(b, pc)} is not a derived entry")
        out[b][pc] = [f"{m},{x}" for m, x in modes]
    return out


def emit_bank(d, tables: dict, modes: dict, bank: int) -> str:
    """tables: {(bank, site): (count, bias, derivation)} from recomp_hints."""
    exec_bank = 0x80 | bank
    out = [HEADER, f"bank = {bank:02x}", f"exec_bank = {exec_bank:02x}", "strict_mx", ""]
    for logical, pb, pc in sorted(d["analysis"].far_aliases):
        if pb == bank:
            out.append(f"name {logical:06x} CODE_{exec_bank:02X}{pc:04X}")
    decisions = d["decisions"]
    for (b, site), (count, bias, _tag) in sorted(tables.items()):
        if b != bank:
            continue
        opt = "idx:X"
        if bias:
            start = (decisions[(b, site)].operand + 2 * bias) & 0xFFFF
            opt += f" tables:{start:04x} idx_bias:{bias}"
        out.append(f"indirect_dispatch {site:04x} {count} {opt}")
    out.append("")
    starts = sorted(modes[bank])
    for i, pc in enumerate(starts):
        end = f" end:{starts[i + 1]:04x}" if i + 1 < len(starts) else ""
        for mx in modes[bank][pc]:
            out.append(f"func CODE_{exec_bank:02X}{pc:04X} {pc:04x}{end} entry_mx:{mx}")
    return "\n".join(out) + "\n"
