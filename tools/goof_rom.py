"""Canonical identification of the supported Goof Troop ROM.

Every tool that consumes the user's ROM obtains it through
``load_and_validate_rom``: it applies the copier-header policy, checks the
exact canonical size and SHA-256 declared in ``config/supported_rom.json``,
and returns the canonical (headerless) bytes in memory.  The user's file is
only ever opened for reading, and nothing is written to disk.

The descriptor is the single source of truth.  No size or hash is repeated
in this module.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re


DEFAULT_DESCRIPTOR = Path(__file__).resolve().parent.parent / "config" / "supported_rom.json"

HEADER_POLICIES = {"reject", "strip_exact"}
SUPPORTED_SCHEMA = 1

# Failure kinds.  Each maps to one process exit code in validate_rom.py.
INPUT_ERROR = "input"            # missing, directory, unreadable
ROM_MISMATCH = "mismatch"        # readable file that is not the supported ROM
DESCRIPTOR_ERROR = "descriptor"  # supported_rom.json missing or malformed


class RomError(Exception):
    """A user-facing validation failure (never a programming error)."""

    def __init__(self, kind: str, code: str, message: str,
                 details: dict | None = None):
        super().__init__(message)
        self.kind = kind
        self.code = code
        self.message = message
        self.details = details or {}


@dataclass(frozen=True)
class InternalHeader:
    offset: int
    map_mode: int
    rom_size_code: int
    region_code: int
    version: int


@dataclass(frozen=True)
class RomDescriptor:
    game: str
    region: str
    revision: int
    mapping: str
    canonical_size: int
    sha256: str
    md5: str
    header_policy: str
    accepted_header_size: int
    internal_header: InternalHeader
    source: Path

    @property
    def accepted_raw_sizes(self) -> tuple[int, ...]:
        if self.header_policy == "strip_exact":
            return (self.canonical_size,
                    self.canonical_size + self.accepted_header_size)
        return (self.canonical_size,)


@dataclass(frozen=True)
class ValidatedRom:
    """The canonical ROM view plus what was observed while producing it."""

    path: Path
    canonical: bytes
    raw_size: int
    header_size: int
    canonical_sha256: str
    canonical_md5: str
    descriptor: RomDescriptor

    @property
    def header_present(self) -> bool:
        return self.header_size != 0

    def report(self) -> dict:
        d = self.descriptor
        return {
            "valid": True,
            "path": str(self.path),
            "game": d.game,
            "region": d.region,
            "revision": d.revision,
            "mapping": d.mapping,
            "raw_size": self.raw_size,
            "header_present": self.header_present,
            "header_size": self.header_size,
            "canonical_size": len(self.canonical),
            "canonical_sha256": self.canonical_sha256,
            "canonical_md5": self.canonical_md5,
            "descriptor": str(d.source),
        }


def _require(obj: dict, key: str, kind: type, where: str):
    if key not in obj:
        raise RomError(DESCRIPTOR_ERROR, "descriptor_field_missing",
                       f"{where}: missing field {key!r}")
    value = obj[key]
    # bool is an int subclass; never accept it where a number is expected.
    if not isinstance(value, kind) or (kind is int and isinstance(value, bool)):
        raise RomError(DESCRIPTOR_ERROR, "descriptor_field_type",
                       f"{where}: field {key!r} must be {kind.__name__}")
    return value


def load_descriptor(path: Path | str | None = None) -> RomDescriptor:
    source = Path(path) if path is not None else DEFAULT_DESCRIPTOR
    try:
        raw = json.loads(source.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise RomError(DESCRIPTOR_ERROR, "descriptor_missing",
                       f"ROM descriptor not found: {source}") from None
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RomError(DESCRIPTOR_ERROR, "descriptor_unreadable",
                       f"ROM descriptor unreadable: {source}: {exc}") from None
    if not isinstance(raw, dict):
        raise RomError(DESCRIPTOR_ERROR, "descriptor_field_type",
                       f"{source}: top level must be an object")
    where = str(source)
    schema = _require(raw, "schema_version", int, where)
    if schema != SUPPORTED_SCHEMA:
        raise RomError(DESCRIPTOR_ERROR, "descriptor_schema",
                       f"{where}: schema_version {schema} is not supported")
    ih = _require(raw, "internal_header", dict, where)
    ih_where = f"{where}: internal_header"
    desc = RomDescriptor(
        game=_require(raw, "game", str, where),
        region=_require(raw, "region", str, where),
        revision=_require(raw, "revision", int, where),
        mapping=_require(raw, "mapping", str, where),
        canonical_size=_require(raw, "canonical_size", int, where),
        sha256=_require(raw, "sha256", str, where),
        md5=_require(raw, "md5", str, where),
        header_policy=_require(raw, "header_policy", str, where),
        accepted_header_size=_require(raw, "accepted_header_size", int, where),
        internal_header=InternalHeader(
            offset=_require(ih, "offset", int, ih_where),
            map_mode=_require(ih, "map_mode", int, ih_where),
            rom_size_code=_require(ih, "rom_size_code", int, ih_where),
            region_code=_require(ih, "region_code", int, ih_where),
            version=_require(ih, "version", int, ih_where),
        ),
        source=source,
    )
    if not re.fullmatch(r"[0-9a-f]{64}", desc.sha256):
        raise RomError(DESCRIPTOR_ERROR, "descriptor_field_value",
                       f"{where}: sha256 must be 64 lowercase hex digits")
    if not re.fullmatch(r"[0-9a-f]{32}", desc.md5):
        raise RomError(DESCRIPTOR_ERROR, "descriptor_field_value",
                       f"{where}: md5 must be 32 lowercase hex digits")
    if desc.header_policy not in HEADER_POLICIES:
        raise RomError(DESCRIPTOR_ERROR, "descriptor_field_value",
                       f"{where}: header_policy must be one of "
                       f"{sorted(HEADER_POLICIES)}")
    if desc.mapping != "lorom":
        raise RomError(DESCRIPTOR_ERROR, "descriptor_field_value",
                       f"{where}: only mapping 'lorom' is implemented")
    if desc.canonical_size <= 0 or desc.accepted_header_size <= 0:
        raise RomError(DESCRIPTOR_ERROR, "descriptor_field_value",
                       f"{where}: sizes must be positive")
    if not 0 <= desc.internal_header.offset <= desc.canonical_size - 32:
        raise RomError(DESCRIPTOR_ERROR, "descriptor_field_value",
                       f"{where}: internal_header.offset outside the ROM")
    return desc


def _describe_internal_header(canonical: bytes, descriptor: RomDescriptor) -> dict:
    """Decode the few header bytes that tell *why* a hash differs.

    Only single numeric fields are reported; the title and any other ROM
    content are never emitted.
    """
    ih = descriptor.internal_header
    if len(canonical) < ih.offset + 32:
        return {}
    block = canonical[ih.offset:ih.offset + 32]
    observed = {
        "map_mode": block[0x15],
        "rom_size_code": block[0x17],
        "region_code": block[0x19],
        "version": block[0x1B],
    }
    expected = {name: getattr(ih, name) for name in observed}
    return {
        "internal_header_observed": {k: f"0x{v:02X}" for k, v in observed.items()},
        "internal_header_matches": observed == expected,
    }


def validate_rom_bytes(data: bytes, descriptor: RomDescriptor,
                       path: Path | str = "<memory>") -> ValidatedRom:
    """Apply the header policy and the exact identity check to raw bytes."""
    raw_size = len(data)
    if raw_size == descriptor.canonical_size:
        header = 0
    elif (descriptor.header_policy == "strip_exact"
          and raw_size == descriptor.canonical_size + descriptor.accepted_header_size):
        header = descriptor.accepted_header_size
    else:
        raise RomError(ROM_MISMATCH, "size",
                       f"file size {raw_size} bytes is not an accepted size",
                       {"raw_size": raw_size,
                        "accepted_raw_sizes": list(descriptor.accepted_raw_sizes)})
    canonical = bytes(data[header:])
    sha256 = hashlib.sha256(canonical).hexdigest()
    if sha256 != descriptor.sha256:
        details = {"raw_size": raw_size, "header_size": header,
                   "expected_sha256": descriptor.sha256,
                   "observed_sha256": sha256}
        details.update(_describe_internal_header(canonical, descriptor))
        raise RomError(ROM_MISMATCH, "sha256",
                       "canonical SHA-256 does not match the supported ROM",
                       details)
    return ValidatedRom(
        path=Path(path),
        canonical=canonical,
        raw_size=raw_size,
        header_size=header,
        canonical_sha256=sha256,
        canonical_md5=hashlib.md5(canonical).hexdigest(),
        descriptor=descriptor,
    )


def load_and_validate_rom(path: Path | str,
                          descriptor: RomDescriptor | None = None) -> ValidatedRom:
    """Read the user's ROM read-only and return its validated canonical view."""
    descriptor = descriptor or load_descriptor()
    rom_path = Path(path)
    if not rom_path.exists():
        raise RomError(INPUT_ERROR, "not_found", f"ROM file not found: {rom_path}")
    if rom_path.is_dir():
        raise RomError(INPUT_ERROR, "is_directory",
                       f"ROM path is a directory, not a file: {rom_path}")
    if not rom_path.is_file():
        raise RomError(INPUT_ERROR, "not_regular_file",
                       f"ROM path is not a regular file: {rom_path}")
    limit = max(descriptor.accepted_raw_sizes)
    try:
        # Size is decided from the bytes actually read, never trusted from
        # stat; reading one byte past the largest accepted size is enough to
        # reject oversized input without loading all of it.
        with rom_path.open("rb") as handle:
            data = handle.read(limit + 1)
    except PermissionError:
        raise RomError(INPUT_ERROR, "unreadable",
                       f"ROM file is not readable (permission denied): {rom_path}") from None
    except OSError as exc:
        raise RomError(INPUT_ERROR, "unreadable",
                       f"ROM file could not be read: {rom_path}: {exc.strerror or exc}") from None
    if len(data) > limit:
        raise RomError(ROM_MISMATCH, "size",
                       f"file is larger than any accepted size ({limit} bytes)",
                       {"raw_size_at_least": len(data),
                        "accepted_raw_sizes": list(descriptor.accepted_raw_sizes)})
    return validate_rom_bytes(data, descriptor, rom_path)
