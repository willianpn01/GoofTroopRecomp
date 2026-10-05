"""Entry M/X derivation.

Entry modes are the modes with which each entry is actually reached by the
callee-exit-aware propagation in analysis.py.  A mode reached only through a
data-dependent callee exit (a flag that can come back either way) is
*uncertain*; an entry keeps its certain modes whenever it has any and falls
back to the uncertain set otherwise.  The engine M/X solver remains the
authority that materialises further variants.
"""

from __future__ import annotations

def entry_modes(res, entries):
    """bank -> pc -> sorted list of 'm,x' strings."""
    out = {}
    for b, e in entries.items():
        out[b] = {}
        for pc, ms in e.items():
            cert = res.certain.get((b, pc))
            if cert is None and pc not in res.entries[b]:       # shared-flow entry
                cert = res.state_certain[b].get(pc)
            chosen = (set(cert) & set(ms)) if cert else set()
            out[b][pc] = sorted(f"{m},{x}" for m, x in (chosen or ms))
    return out
