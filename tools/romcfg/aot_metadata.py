"""aot_metadata.json: validation of the project's AOT scheduler metadata.

The file is merged into the generated CFG by apply_cfg_metadata.py.  It holds
scheduler contracts (yield/terminate boundaries, host re-entry points and
guest continuation entries), addressed by bank + PC only.  This module checks
it before any merge so a malformed file fails with a clear message:

  * the merger's own schema (schema 1, entries array, per-entry fields);
  * every bank is a code bank this generator emits;
  * every name is the address-derived CODE_<pc24> of its entry.
"""

from __future__ import annotations

from pathlib import Path

from .cpu65816 import CODE_BANKS
from .hints import pc24


class MetadataError(Exception):
    """aot_metadata.json is missing or malformed."""


def load(path: Path, merger) -> dict:
    """Load and validate.  `merger` is the apply_cfg_metadata module."""
    path = Path(path)
    if not path.is_file():
        raise MetadataError(f"AOT metadata not found: {path}")
    try:
        manifest = merger.load_manifest(path)
    except (OSError, ValueError) as exc:
        raise MetadataError(str(exc)) from None
    for number, item in enumerate(manifest["entries"], 1):
        where = f"{path}: entry {number}"
        if not isinstance(item, dict):
            raise MetadataError(f"{where}: must be an object")
        try:
            bank = merger.parse_hex(item.get("bank"), "bank", 0xFF)
            address = merger.parse_hex(item.get("address"), "address", 0xFFFF)
            merger.desired_line(item, bank, address)
        except ValueError as exc:
            raise MetadataError(f"{where}: {exc}") from None
        if bank not in CODE_BANKS:
            raise MetadataError(f"{where}: bank {bank:02X} is not a generated code bank")
        if address < 0x8000:
            raise MetadataError(f"{where}: address {address:04X} is not in ROM space")
        if item["name"] != f"CODE_{pc24(bank, address)}":
            raise MetadataError(f"{where}: name must be CODE_{pc24(bank, address)} (address-derived)")
    return manifest
