#!/usr/bin/env python3
"""Apply declarative entry metadata to generated snesrecomp CFG files.

The merger keys entries by physical bank plus local PC.  It parses CFG
directives into records; comments and line positions are never identities.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import shlex


BOUNDARY_KINDS = {"yield", "terminate"}


@dataclass
class Entry:
    line: int
    kind: str
    name: str
    address: int
    tokens: list[str]

    @property
    def options(self) -> dict[str, str]:
        result: dict[str, str] = {}
        for token in self.tokens[3:]:
            if ":" not in token:
                continue
            key, value = token.split(":", 1)
            if key in result:
                raise ValueError(
                    f"CFG entry ${self.address:04X} repeats attribute {key!r}")
            result[key] = value
        return result


def parse_hex(value: object, field: str, maximum: int) -> int:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty hexadecimal string")
    try:
        parsed = int(value, 16)
    except ValueError as exc:
        raise ValueError(f"{field} has invalid hexadecimal value {value!r}") from exc
    if not 0 <= parsed <= maximum:
        raise ValueError(f"{field} is out of range: {value!r}")
    return parsed


def parse_cfg(text: str, source: str) -> tuple[list[str], int, list[Entry]]:
    lines = text.splitlines()
    bank: int | None = None
    entries: list[Entry] = []
    for number, raw in enumerate(lines):
        try:
            tokens = shlex.split(raw, comments=True, posix=True)
        except ValueError as exc:
            raise ValueError(f"{source}:{number + 1}: malformed directive: {exc}") from exc
        if not tokens:
            continue
        if tokens[0] == "bank":
            if len(tokens) != 3 or tokens[1] != "=" or bank is not None:
                raise ValueError(f"{source}:{number + 1}: invalid or duplicate bank directive")
            bank = parse_hex(tokens[2], "CFG bank", 0xFF)
        elif tokens[0] in ("func", "continuation_entry"):
            if len(tokens) < 3:
                raise ValueError(f"{source}:{number + 1}: incomplete entry")
            entries.append(Entry(number, tokens[0], tokens[1],
                                 parse_hex(tokens[2], "CFG entry address", 0xFFFF),
                                 tokens))
    if bank is None:
        raise ValueError(f"{source}: missing bank directive")
    seen: set[tuple[str, str, int, str | None]] = set()
    for entry in entries:
        identity = (entry.kind, entry.name, entry.address,
                    entry.options.get("entry_mx"))
        if identity in seen:
            raise ValueError(
                f"{source}: duplicate entry identity at ${bank:02X}:{entry.address:04X}")
        seen.add(identity)
        if ("aot_boundary_pc" in entry.options and
                "aot_boundary" not in entry.options):
            raise ValueError(
                f"{source}: existing aot_boundary_pc has no boundary at "
                f"${bank:02X}:{entry.address:04X}")
    return lines, bank, entries


def load_manifest(path: Path) -> dict:
    def reject_duplicate(pairs: list[tuple[str, object]]) -> dict:
        result: dict = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key!r}")
            result[key] = value
        return result

    try:
        data = json.loads(path.read_text(encoding="utf-8"),
                          object_pairs_hook=reject_duplicate)
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema") != 1:
        raise ValueError(f"{path}: expected object with schema 1")
    if set(data) != {"schema", "entries"} or not isinstance(data["entries"], list):
        raise ValueError(f"{path}: expected only schema and entries array")
    return data


def desired_line(item: dict, bank: int, address: int) -> str:
    allowed = {"bank", "address", "kind", "name", "end", "entry_mx",
               "aot_boundary", "aot_boundary_pc", "aot_entry_pc"}
    unknown = set(item) - allowed
    if unknown:
        raise ValueError(f"metadata has unknown field(s): {', '.join(sorted(unknown))}")
    kind = item.get("kind")
    name = item.get("name")
    if kind not in ("func", "continuation_entry") or not isinstance(name, str) or not name:
        raise ValueError("metadata kind/name must identify func or continuation_entry")
    tokens = [kind, name, f"{address:04x}"]
    if kind == "continuation_entry":
        if "aot_boundary" in item or "aot_boundary_pc" in item:
            raise ValueError("continuation_entry conflicts with AOT boundary attributes")
        end = parse_hex(item.get("end"), "continuation end", 0xFFFF)
        if end <= address:
            raise ValueError("continuation end must be greater than its address")
        mx = item.get("entry_mx")
        if (not isinstance(mx, list) or len(mx) != 2 or
                any(type(bit) is not int or bit not in (0, 1) for bit in mx)):
            raise ValueError("continuation entry_mx must be [M, X] with bits 0 or 1")
        tokens.extend((f"end:{end:04x}", f"entry_mx:{mx[0]},{mx[1]}"))
    else:
        if "entry_mx" in item:
            mx = item["entry_mx"]
            if (not isinstance(mx, list) or len(mx) != 2 or
                    any(type(bit) is not int or bit not in (0, 1) for bit in mx)):
                raise ValueError("func entry_mx must be [M, X] with bits 0 or 1")
            tokens.append(f"entry_mx:{mx[0]},{mx[1]}")
        if "aot_boundary" in item:
            boundary = item["aot_boundary"]
            if boundary not in BOUNDARY_KINDS:
                raise ValueError(f"invalid aot_boundary {boundary!r}; expected yield or terminate")
            tokens.append(f"aot_boundary:{boundary}")
            if "aot_boundary_pc" in item:
                pc = parse_hex(item["aot_boundary_pc"], "aot_boundary_pc", 0xFFFFFF)
                tokens.append(f"aot_boundary_pc:{pc:06x}")
        elif "aot_boundary_pc" in item:
            raise ValueError("aot_boundary_pc requires aot_boundary")
        elif "aot_entry_pc" not in item and "entry_mx" not in item:
            raise ValueError(
                "func metadata requires an entry_mx override, AOT boundary, "
                "or aot_entry_pc")
    if "aot_entry_pc" in item:
        if "aot_boundary" in item:
            raise ValueError("aot_entry_pc conflicts with AOT boundary attributes")
        pc = parse_hex(item["aot_entry_pc"], "aot_entry_pc", 0xFFFFFF)
        if pc == 0:
            raise ValueError("aot_entry_pc must be nonzero")
        tokens.append(f"aot_entry_pc:{pc:06x}")
    return " ".join(tokens)


def apply_metadata(cfg_text: str, manifest: dict, source: str = "<cfg>") -> str:
    lines, bank, entries = parse_cfg(cfg_text, source)
    selected: list[tuple[int, dict]] = []
    identities: set[tuple[int, int]] = set()
    for number, item in enumerate(manifest["entries"], 1):
        if not isinstance(item, dict):
            raise ValueError(f"metadata entry {number} must be an object")
        item_bank = parse_hex(item.get("bank"), "metadata bank", 0xFF)
        address = parse_hex(item.get("address"), "metadata address", 0xFFFF)
        identity = (item_bank, address)
        if identity in identities:
            raise ValueError(f"duplicate metadata for ${item_bank:02X}:{address:04X}")
        identities.add(identity)
        if item_bank == bank:
            selected.append((address, item))

    by_address: dict[int, list[Entry]] = {}
    funcs = [entry for entry in entries if entry.kind == "func"]
    for entry in entries:
        by_address.setdefault(entry.address, []).append(entry)

    additions: list[tuple[int, str]] = []
    replacements: dict[int, str] = {}
    continuation_addresses = [
        address for address, item in selected
        if item.get("kind") == "continuation_entry"
    ]
    continuation_insertion = (
        next((entry.line for entry in funcs
              if entry.address > max(continuation_addresses)), len(lines))
        if continuation_addresses else len(lines)
    )
    for address, item in selected:
        wanted = desired_line(item, bank, address)
        matches = by_address.get(address, [])
        if item["kind"] == "func":
            if len(matches) != 1 or matches[0].kind != "func":
                raise ValueError(f"metadata func does not resolve uniquely at ${bank:02X}:{address:04X}")
            entry = matches[0]
            if entry.name != item["name"]:
                raise ValueError(f"metadata name conflicts at ${bank:02X}:{address:04X}")
            opts = entry.options
            if "aot_boundary_pc" in opts and "aot_boundary" not in opts:
                raise ValueError("existing aot_boundary_pc has no boundary")
            for token in wanted.split()[3:]:
                key, value = token.split(":", 1)
                if key == "entry_mx" and key in opts and opts[key] != value:
                    entry.tokens = [
                        f"entry_mx:{value}" if old.startswith("entry_mx:")
                        else old for old in entry.tokens
                    ]
                    opts[key] = value
                elif key in opts and opts[key] != value:
                    raise ValueError(f"metadata conflicts with existing {key} at ${bank:02X}:{address:04X}")
                if key not in opts:
                    entry.tokens.append(token)
            replacements[entry.line] = " ".join(entry.tokens)
        else:
            if matches:
                if len(matches) != 1 or " ".join(matches[0].tokens) != wanted:
                    raise ValueError(f"metadata conflicts with existing entry at ${bank:02X}:{address:04X}")
                continue
            owners = []
            for func in funcs:
                end_raw = func.options.get("end")
                if end_raw is not None:
                    end = parse_hex(end_raw, "function end", 0xFFFF)
                    if func.address < address < end:
                        owners.append(func)
            if len(owners) != 1:
                raise ValueError(f"continuation address is not inside exactly one base func at ${bank:02X}:{address:04X}")
            end = parse_hex(item["end"], "continuation end", 0xFFFF)
            if end > parse_hex(owners[0].options["end"], "owner end", 0xFFFF):
                raise ValueError(f"continuation exceeds owner at ${bank:02X}:{address:04X}")
            additions.append((continuation_insertion, wanted))

    for line, value in replacements.items():
        lines[line] = value
    for line, value in sorted(additions, key=lambda pair: (pair[0], pair[1]), reverse=True):
        lines.insert(line, value)
    return "\n".join(lines) + ("\n" if cfg_text.endswith("\n") else "")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--cfg", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        manifest = load_manifest(args.metadata)
        original = args.cfg.read_text(encoding="utf-8")
        result = apply_metadata(original, manifest, str(args.cfg))
        output = args.output or args.cfg
        output.parent.mkdir(parents=True, exist_ok=True)
        if not output.exists() or output.read_text(encoding="utf-8") != result:
            output.write_text(result, encoding="utf-8", newline="\n")
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
