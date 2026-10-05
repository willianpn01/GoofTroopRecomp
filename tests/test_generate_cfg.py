"""Tests for tools/generate_cfg.py, tools/derive_recomp_hints.py and romcfg.

Synthetic tests use a tiny hand-assembled 65816 program (no game bytes) with
matching temporary supported_rom.json / recomp_hints.json / aot_metadata.json.
Integration tests read the user's own ROM when GOOF_TEST_ROM points to it and
are skipped otherwise:

    GOOF_TEST_ROM=/path/to/GOOFT_USA.sfc python3 -m unittest discover -s tests -v
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

from romcfg import cfg_emit, derive, hints  # noqa: E402
from romcfg.cpu65816 import Rom  # noqa: E402

GENERATE = TOOLS / "generate_cfg.py"
DERIVE_HINTS = TOOLS / "derive_recomp_hints.py"
BANK = 0x8000
OUTPUTS = ("bank00.cfg", "bank01.cfg", "bank02.cfg", "generation_manifest.json")

# bank 0 at $8000: native mode, a JSR, a JSL into bank 1 (through the $91
# mirror), and an indexed JSR through a 4-slot table whose index is masked.
PROGRAM = {
    0x8000: "78 18 FB C2 30",        # SEI CLC XCE REP #$30
    0x8005: "20 40 80",              # JSR $8040
    0x8008: "22 00 80 91",           # JSL $918000 (mirror of $818000)
    0x800C: "A5 00",                 # LDA $00
    0x800E: "29 03 00",              # AND #$0003
    0x8011: "0A AA",                 # ASL A ; TAX
    0x8013: "FC 20 80",              # JSR ($8020,X)
    0x8016: "80 FE",                 # BRA $8016
    0x8020: "30 80 34 80 38 80 3C 80",
    0x8030: "E8 60", 0x8034: "C8 60", 0x8038: "CA 60", 0x803C: "88 60",
    0x8040: "60",                    # RTS
}


def synthetic_rom() -> bytes:
    rom = bytearray(3 * BANK)
    for pc, text in PROGRAM.items():
        code = bytes.fromhex(text)
        rom[pc - 0x8000:pc - 0x8000 + len(code)] = code
    rom[BANK] = 0x6B                           # bank 1 $8000: RTL
    rom[0x7FFC:0x7FFE] = b"\x00\x80"           # emulation RESET -> $8000
    rom[0x7FC0 + 0x15] = 0x30
    rom[0x7FC0 + 0x17] = 0x07
    rom[0x7FC0 + 0x19] = 0x01
    return bytes(rom)


def run_tool(tool: Path, *args, env=None) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(tool), *map(str, args)], capture_output=True, text=True,
                          env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1", **(env or {})))


def digests(d: Path) -> dict:
    return {n: hashlib.sha256((d / n).read_bytes()).hexdigest() for n in OUTPUTS}


class Derivation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d = derive.run(Rom(synthetic_rom()))

    def test_table(self):
        self.assertEqual(hints.derived_tables(self.d), {(0, 0x8013): (4, 0, "static")})
        self.assertEqual(self.d["decisions"][(0, 0x8013)].cls, "J1")   # proven by the AND mask

    def test_entries_and_modes(self):
        m = self.d["modes"]
        self.assertEqual(sorted(m[0]), [0x8000, 0x8030, 0x8034, 0x8038, 0x803C, 0x8040])
        self.assertEqual(m[0][0x8000], ["1,1"])
        self.assertEqual(m[0][0x8030], ["0,0"])
        self.assertEqual(sorted(m[1]), [0x8000])
        self.assertEqual(m[1][0x8000], ["0,0"])

    def test_emit(self):
        tables = hints.derived_tables(self.d)
        modes = cfg_emit.entry_modes(self.d, {})
        b0 = cfg_emit.emit_bank(self.d, tables, modes, 0)
        self.assertIn("indirect_dispatch 8013 4 idx:X\n", b0)
        self.assertIn("func CODE_808000 8000 end:8030 entry_mx:1,1\n", b0)
        self.assertIn("func CODE_808040 8040 entry_mx:0,0\n", b0)
        b1 = cfg_emit.emit_bank(self.d, tables, modes, 1)
        self.assertIn("name 918000 CODE_818000\n", b1)
        self.assertEqual(b0, cfg_emit.emit_bank(derive.run(Rom(synthetic_rom())), tables, modes, 0))

    def test_hints_roundtrip(self):
        text = hints.dumps(hints.build(self.d, "0" * 64))
        view = hints.validate(json.loads(text), "0" * 64)
        self.assertEqual(view["tables"], hints.derived_tables(self.d))


class Fixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="goof_cfg_test_")
        self.tmp = Path(self._tmp.name)
        self.rom_bytes = synthetic_rom()
        self.rom = self.tmp / "rom.sfc"
        self.rom.write_bytes(self.rom_bytes)
        sha = hashlib.sha256(self.rom_bytes).hexdigest()
        self.descriptor = self.write_json("supported_rom.json", {
            "schema_version": 1, "game": "synthetic", "region": "none", "revision": 0,
            "mapping": "lorom", "canonical_size": len(self.rom_bytes), "sha256": sha,
            "md5": hashlib.md5(self.rom_bytes).hexdigest(), "header_policy": "strip_exact",
            "accepted_header_size": 512,
            "internal_header": {"offset": 0x7FC0, "map_mode": 0x30, "rom_size_code": 7,
                                "region_code": 1, "version": 0}})
        self.hints_obj = hints.build(derive.run(Rom(self.rom_bytes)), sha)
        self.hints = self.tmp / "recomp_hints.json"
        self.hints.write_text(hints.dumps(self.hints_obj))
        self.meta_obj = {"schema": 1, "entries": [
            {"bank": "00", "address": "8040", "kind": "func", "name": "CODE_808040",
             "aot_entry_pc": "808040"},
            {"bank": "00", "address": "8003", "kind": "continuation_entry", "name": "CODE_808003",
             "end": "8005", "entry_mx": [1, 1]}]}
        self.meta = self.write_json("aot_metadata.json", self.meta_obj)

    def tearDown(self):
        self._tmp.cleanup()

    def write_json(self, name, obj) -> Path:
        p = self.tmp / name
        p.write_text(json.dumps(obj))
        return p

    def gen(self, out, rom=None, hints_path=None, meta=None, extra=()):
        return run_tool(GENERATE, "--rom", rom or self.rom, "--output", self.tmp / out,
                        "--descriptor", self.descriptor, "--hints", hints_path or self.hints,
                        "--aot-metadata", meta or self.meta, *extra)


class Generate(Fixture):
    def test_pass_and_merge(self):
        r = self.gen("out")
        self.assertEqual(r.returncode, 0, r.stderr)
        b0 = (self.tmp / "out/bank00.cfg").read_text()
        self.assertIn("func CODE_808040 8040 entry_mx:0,0 aot_entry_pc:808040\n", b0)
        self.assertIn("continuation_entry CODE_808003 8003 end:8005 entry_mx:1,1\n", b0)
        man = json.loads((self.tmp / "out/generation_manifest.json").read_text())
        self.assertEqual(man["summary"]["jump_tables"], 1)
        self.assertEqual(man["summary"]["continuation_entries"], 1)
        self.assertNotIn(str(self.tmp), json.dumps(man))       # no paths, no timestamps

    def test_deterministic_and_header_invariant(self):
        headered = self.tmp / "headered.sfc"
        headered.write_bytes(bytes(512) + self.rom_bytes)
        self.assertEqual(self.gen("a").returncode, 0)
        self.assertEqual(self.gen("b").returncode, 0)
        r = self.gen("h", rom=headered)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("512-byte copier header ignored", r.stdout)
        self.assertEqual(digests(self.tmp / "a"), digests(self.tmp / "b"))
        self.assertEqual(digests(self.tmp / "a"), digests(self.tmp / "h"))
        self.assertEqual(headered.read_bytes()[:512], bytes(512))   # input untouched

    def test_invalid_rom_rejected(self):
        bad = self.tmp / "bad.sfc"
        bad.write_bytes(bytes(len(self.rom_bytes)))
        self.assertEqual(self.gen("o", rom=bad).returncode, 1)
        self.assertEqual(self.gen("o", rom=self.tmp / "missing.sfc").returncode, 3)
        self.assertFalse((self.tmp / "o").exists())

    def test_missing_hints_rejected(self):
        r = self.gen("o", hints_path=self.tmp / "none.json")
        self.assertEqual(r.returncode, 5)
        self.assertIn("not found", r.stderr)

    def test_malformed_hints_rejected(self):
        cases = {
            "json": "{",
            "unknown": {**self.hints_obj, "extra": 1},
            "missing": {k: v for k, v in self.hints_obj.items() if k != "mode_hints"},
            "sha": {**self.hints_obj, "rom_sha256": "0" * 64},
            "schema": {**self.hints_obj, "schema_version": 2},
            "banks": {**self.hints_obj, "code_banks": [0, 1]},
            "site": {**self.hints_obj, "jump_tables": [{"site": "008013", "count": 4, "derivation": "static"}]},
            "count": {**self.hints_obj, "jump_tables": [{"site": "808013", "count": True, "derivation": "static"}]},
            "dup": {**self.hints_obj, "jump_tables": self.hints_obj["jump_tables"] * 2},
            "entry_hint": {**self.hints_obj, "entry_hints": [{"pc": "808040", "mx": [1]}]},
        }
        for name, obj in cases.items():
            p = self.tmp / f"h_{name}.json"
            p.write_text(obj if isinstance(obj, str) else json.dumps(obj))
            with self.subTest(name):
                self.assertEqual(self.gen(f"o_{name}", hints_path=p).returncode, 5)

    def test_hints_disagreeing_with_rom_rejected(self):
        for rows in ([{"site": "808013", "count": 5, "derivation": "static"}], [],
                     [{"site": "808013", "count": 4, "derivation": "project"}]):
            p = self.write_json("h.json", {**self.hints_obj, "jump_tables": rows})
            with self.subTest(rows=rows):
                r = self.gen("o", hints_path=p)
                self.assertEqual(r.returncode, 7)
                self.assertIn("disagrees with recomp_hints.json", r.stderr)

    def test_mode_hint_applied(self):
        p = self.write_json("h.json", {**self.hints_obj, "mode_hints": [{"pc": "808040", "mx": [[1, 0]]}]})
        self.assertEqual(self.gen("o", hints_path=p, meta=self.write_json("m.json", {"schema": 1, "entries": []})).returncode, 0)
        self.assertIn("func CODE_808040 8040 entry_mx:1,0\n", (self.tmp / "o/bank00.cfg").read_text())

    def test_malformed_metadata_rejected(self):
        e = self.meta_obj["entries"][0]
        cases = {
            "name": [{**e, "name": "SomeLabel"}],
            "field": [{**e, "comment": "x"}],
            "bank": [{**e, "bank": "05"}],
            "unresolved": [{**e, "address": "8041", "name": "CODE_808041"}],
        }
        for name, entries in cases.items():
            with self.subTest(name):
                r = self.gen(f"o_{name}", meta=self.write_json(f"m_{name}.json", {"schema": 1, "entries": entries}))
                self.assertEqual(r.returncode, 6, r.stderr)
        self.assertEqual(self.gen("o_missing", meta=self.tmp / "none.json").returncode, 6)

    def test_output_must_be_empty(self):
        (self.tmp / "busy").mkdir()
        (self.tmp / "busy/x").write_text("x")
        self.assertEqual(self.gen("busy").returncode, 8)
        self.assertEqual((self.tmp / "busy/x").read_text(), "x")

    def test_cli_has_no_source_inputs(self):
        r = run_tool(GENERATE, "--help")
        self.assertEqual(r.returncode, 0)
        for word in ("--source", "--asm", "--cfg-dir", "--disassembly"):
            self.assertNotIn(word, r.stdout)

    def test_opens_only_declared_inputs(self):
        """Every file the generator opens is the ROM, a declared config file,
        a tools/ module, the output directory or the Python runtime."""
        import sysconfig
        trace = self.tmp / "trace.json"
        code = ("import json, os, runpy, sys\n"
                "log = []\n"
                "sys.addaudithook(lambda e, a: log.append(os.fsdecode(a[0])) if e == 'open' and a and "
                "isinstance(a[0], (str, bytes, os.PathLike)) else None)\n"
                "sys.argv = sys.argv[1:]\n"
                "try:\n    runpy.run_path(sys.argv[0], run_name='__main__')\n"
                "except SystemExit as e:\n    code = e.code\n"
                "json.dump({'code': code, 'log': log}, open(os.environ['TRACE'], 'w'))\n")
        r = subprocess.run([sys.executable, "-c", code, str(GENERATE), "--rom", str(self.rom),
                            "--output", str(self.tmp / "o"), "--descriptor", str(self.descriptor),
                            "--hints", str(self.hints), "--aot-metadata", str(self.meta)],
                           env=dict(os.environ, TRACE=str(trace), PYTHONDONTWRITEBYTECODE="1"),
                           capture_output=True, text=True)
        t = json.loads(trace.read_text())
        self.assertEqual(t["code"], 0, r.stderr)
        allowed = [TOOLS, self.tmp, Path(sys.prefix).resolve(), Path(sys.base_prefix).resolve(),
                   Path(sysconfig.get_paths()["stdlib"]).resolve()]
        stray = [f for f in t["log"] if not any(Path(f).resolve() == a or a in Path(f).resolve().parents
                                                for a in allowed)]
        self.assertEqual(stray, [])
        self.assertIn(str(self.rom), t["log"])


@unittest.skipUnless(os.environ.get("GOOF_TEST_ROM"), "GOOF_TEST_ROM not set")
class SupportedRom(unittest.TestCase):
    """The user's own ROM with the project's candidate config."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="goof_cfg_rom_")
        self.tmp = Path(self._tmp.name)
        self.rom = Path(os.environ["GOOF_TEST_ROM"])

    def tearDown(self):
        self._tmp.cleanup()

    def test_generate(self):
        r = run_tool(GENERATE, "--rom", self.rom, "--output", self.tmp / "a")
        self.assertEqual(r.returncode, 0, r.stderr)
        man = json.loads((self.tmp / "a/generation_manifest.json").read_text())
        self.assertEqual(man["summary"]["jump_tables"], 397)
        self.assertEqual(man["summary"]["continuation_entries"], 8)
        self.assertEqual(run_tool(GENERATE, "--rom", self.rom, "--output", self.tmp / "b").returncode, 0)
        self.assertEqual(digests(self.tmp / "a"), digests(self.tmp / "b"))
        headered = self.tmp / "headered.sfc"             # temporary; removed with the directory
        headered.write_bytes(bytes(512) + self.rom.read_bytes())
        self.assertEqual(run_tool(GENERATE, "--rom", headered, "--output", self.tmp / "h").returncode, 0)
        self.assertEqual(digests(self.tmp / "a"), digests(self.tmp / "h"))

    def test_hints_reproduced(self):
        r = run_tool(DERIVE_HINTS, "--rom", self.rom, "--check", TOOLS.parent / "config/recomp_hints.json")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
