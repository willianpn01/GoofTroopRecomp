"""recomp_hints.json: schema v1 validation, serialisation and comparison.

The file holds machine-needed facts only (addresses, counts, biases, flags).
Every jump-table row is re-derivable from the ROM; the file is the pinned,
reviewed expected result, and generation fails closed when a fresh
derivation disagrees with it.

Schema v1 (all fields required, no others allowed):

    schema_version  1
    rom_sha256      64 lowercase hex digits; must equal supported_rom.json
    derived_by      derivation algorithm tag (string)
    code_banks      physical LoROM banks holding CPU code (list of int)
    jump_tables     [{"site": "80XXXX", "count": n, "bias"?: b, "derivation": d}]
    entry_hints     [{"pc": "80XXXX", "mx": [m, x], "kind": "entry"}]
    mode_hints      [{"pc": "80XXXX", "mx": [[m, x], ...]}]
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from .cpu65816 import CODE_BANKS

SCHEMA_VERSION = 1
# Tag of the derivation algorithm.  Unchanged since the reviewed hints were
# produced: the rules and their output are identical.
ALGORITHM = "p2-hints-0.1"

DERIVATION_TAG = {"J1": "static", "J2": "static", "J3": "runtime", "J4": "project", "J5": "unresolved"}
DERIVATIONS = ("static", "runtime", "project")
TOP_KEYS = ("schema_version", "rom_sha256", "derived_by", "code_banks",
            "jump_tables", "entry_hints", "mode_hints")
MAX_COUNT = 128
MAX_BIAS = 3


class HintsError(Exception):
    """recomp_hints.json is missing, malformed or disagrees with the ROM."""


def pc24(bank: int, pc: int) -> str:
    return f"{0x80 | bank:02X}{pc:04X}"


def _site(value, where: str, code_banks) -> tuple[int, int]:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9A-F]{6}", value):
        raise HintsError(f"{where}: must be 6 uppercase hex digits (24-bit PC), got {value!r}")
    v = int(value, 16)
    bank, pc = (v >> 16) & 0x7F, v & 0xFFFF
    if v >> 16 != 0x80 | bank or bank not in code_banks:
        raise HintsError(f"{where}: {value} is not in an executed code bank "
                         f"{[f'{0x80 | b:02X}' for b in code_banks]}")
    if pc < 0x8000:
        raise HintsError(f"{where}: {value} is not in ROM space")
    return bank, pc


def _int(obj: dict, key: str, where: str, lo: int, hi: int) -> int:
    v = obj.get(key)
    if type(v) is not int or not lo <= v <= hi:
        raise HintsError(f"{where}: {key!r} must be an integer in [{lo}, {hi}], got {v!r}")
    return v


def _mx(v, where: str) -> tuple[int, int]:
    if (not isinstance(v, list) or len(v) != 2
            or any(type(b) is not int or b not in (0, 1) for b in v)):
        raise HintsError(f"{where}: mx must be [M, X] with bits 0 or 1, got {v!r}")
    return v[0], v[1]


def _no_duplicate_keys(pairs):
    out: dict = {}
    for k, v in pairs:
        if k in out:
            raise HintsError(f"duplicate JSON key {k!r}")
        out[k] = v
    return out


def validate(raw, expected_sha256: str) -> dict:
    """Validate a parsed hints object.  Returns a normalised view:
    tables {(bank, site): (count, bias, derivation)}, entry_roots, mode_overrides."""
    if not isinstance(raw, dict):
        raise HintsError("top level must be a JSON object")
    missing = [k for k in TOP_KEYS if k not in raw]
    unknown = sorted(set(raw) - set(TOP_KEYS))
    if missing:
        raise HintsError(f"missing field(s): {', '.join(missing)}")
    if unknown:
        raise HintsError(f"unknown field(s): {', '.join(unknown)}")
    if type(raw["schema_version"]) is not int or raw["schema_version"] != SCHEMA_VERSION:
        raise HintsError(f"schema_version must be {SCHEMA_VERSION}, got {raw['schema_version']!r}")
    if not isinstance(raw["rom_sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", raw["rom_sha256"]):
        raise HintsError("rom_sha256 must be 64 lowercase hex digits")
    if raw["rom_sha256"] != expected_sha256:
        raise HintsError("rom_sha256 does not match the supported ROM descriptor")
    if not isinstance(raw["derived_by"], str) or not raw["derived_by"]:
        raise HintsError("derived_by must be a non-empty string")
    banks = raw["code_banks"]
    if (not isinstance(banks, list) or any(type(b) is not int for b in banks)
            or tuple(banks) != CODE_BANKS):
        raise HintsError(f"code_banks must be {list(CODE_BANKS)} (the banks this tool analyses), got {banks!r}")
    for key in ("jump_tables", "entry_hints", "mode_hints"):
        if not isinstance(raw[key], list) or any(not isinstance(r, dict) for r in raw[key]):
            raise HintsError(f"{key} must be a list of objects")

    tables: dict = {}
    for i, row in enumerate(raw["jump_tables"]):
        where = f"jump_tables[{i}]"
        extra = sorted(set(row) - {"site", "count", "bias", "derivation"})
        if extra:
            raise HintsError(f"{where}: unknown field(s): {', '.join(extra)}")
        for k in ("site", "count", "derivation"):
            if k not in row:
                raise HintsError(f"{where}: missing field {k!r}")
        key = _site(row["site"], f"{where}.site", CODE_BANKS)
        count = _int(row, "count", where, 1, MAX_COUNT)
        bias = _int(row, "bias", where, 1, MAX_BIAS) if "bias" in row else 0
        if row["derivation"] not in DERIVATIONS:
            raise HintsError(f"{where}: derivation must be one of {list(DERIVATIONS)}")
        if key in tables:
            raise HintsError(f"{where}: duplicate site {row['site']}")
        tables[key] = (count, bias, row["derivation"])

    roots = set()
    seen = set()
    for i, row in enumerate(raw["entry_hints"]):
        where = f"entry_hints[{i}]"
        if set(row) != {"pc", "mx", "kind"}:
            raise HintsError(f"{where}: fields must be exactly pc, mx, kind")
        if row["kind"] != "entry":
            raise HintsError(f"{where}: kind must be 'entry'")
        b, pc = _site(row["pc"], f"{where}.pc", CODE_BANKS)
        m, x = _mx(row["mx"], where)
        if (b, pc, m, x) in seen:
            raise HintsError(f"{where}: duplicate entry hint")
        seen.add((b, pc, m, x))
        roots.add((b, pc, m, x, "entry_hint"))

    overrides = {}
    for i, row in enumerate(raw["mode_hints"]):
        where = f"mode_hints[{i}]"
        if set(row) != {"pc", "mx"}:
            raise HintsError(f"{where}: fields must be exactly pc, mx")
        key = _site(row["pc"], f"{where}.pc", CODE_BANKS)
        if not isinstance(row["mx"], list) or not row["mx"]:
            raise HintsError(f"{where}: mx must be a non-empty list of [M, X]")
        modes = {_mx(v, where) for v in row["mx"]}
        if key in overrides:
            raise HintsError(f"{where}: duplicate mode hint")
        overrides[key] = sorted(modes)
    return {"tables": tables, "entry_roots": sorted(roots), "mode_overrides": overrides,
            "derived_by": raw["derived_by"]}


def load(path: Path, expected_sha256: str) -> tuple[dict, dict, bytes]:
    """(raw, validated view, file bytes).  Raises HintsError."""
    try:
        data = Path(path).read_bytes()
    except FileNotFoundError:
        raise HintsError(f"recomp hints not found: {path}") from None
    except OSError as exc:
        raise HintsError(f"recomp hints unreadable: {path}: {exc.strerror or exc}") from None
    try:
        raw = json.loads(data.decode("utf-8"), object_pairs_hook=_no_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HintsError(f"{path}: invalid JSON: {exc}") from None
    try:
        view = validate(raw, expected_sha256)
    except HintsError as exc:
        raise HintsError(f"{path}: {exc}") from None
    return raw, view, data


def derived_tables(d) -> dict:
    """{(bank, site): (count, bias, derivation)} from a derivation result."""
    out = {}
    for (b, site), dec in sorted(d["decisions"].items()):
        if dec.cls == "J5" or not dec.count:
            continue
        out[(b, site)] = (dec.count, dec.bias, DERIVATION_TAG[dec.cls])
    return out


def unresolved_sites(d) -> list[str]:
    return [pc24(b, s) for (b, s), dec in sorted(d["decisions"].items())
            if dec.cls == "J5" or not dec.count]


def diff_tables(pinned: dict, derived: dict) -> list[str]:
    """Human-readable differences between pinned and freshly derived tables.
    Every row must match exactly (count, bias, derivation tag).  This
    generator derives static rows only, so a pinned runtime/project row is
    reported as a difference rather than trusted silently."""
    out = []
    for key in sorted(set(pinned) | set(derived)):
        p, d = pinned.get(key), derived.get(key)
        name = pc24(*key)
        if p is None:
            out.append(f"{name}: derived (count {d[0]}, bias {d[1]}) but absent from recomp_hints.json")
        elif d is None:
            out.append(f"{name}: pinned ({p[2]}, count {p[0]}, bias {p[1]}) but not derived")
        elif p != d:
            out.append(f"{name}: pinned {p[2]} count {p[0]} bias {p[1]}, "
                       f"derived {d[2]} count {d[0]} bias {d[1]}")
    return out


def build(d, rom_sha256: str) -> dict:
    tables = []
    for (b, site), (count, bias, tag) in derived_tables(d).items():
        row = {"site": pc24(b, site), "count": count}
        if bias:
            row["bias"] = bias
        row["derivation"] = tag
        tables.append(row)
    return {
        "schema_version": SCHEMA_VERSION,
        "rom_sha256": rom_sha256,
        "derived_by": ALGORITHM,
        "code_banks": list(CODE_BANKS),
        "jump_tables": tables,
        "entry_hints": [],
        "mode_hints": [],
    }


def dumps(hints: dict) -> str:
    """Stable, review-friendly JSON: one jump-table row per line."""
    lines = ["{"]
    items = list(hints.items())
    for i, (k, v) in enumerate(items):
        comma = "," if i < len(items) - 1 else ""
        if isinstance(v, list) and v and isinstance(v[0], dict):
            rows = [" " * 4 + json.dumps(r, separators=(", ", ": ")) for r in v]
            lines.append(f'  "{k}": [\n' + ",\n".join(rows) + f"\n  ]{comma}")
        else:
            lines.append(f'  "{k}": {json.dumps(v, separators=(", ", ": "))}{comma}')
    lines.append("}")
    return "\n".join(lines) + "\n"
