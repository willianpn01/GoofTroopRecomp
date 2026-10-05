"""65816 opcode model and LoROM access for ROM-only CFG derivation.

Pure ROM decoding over the canonical (headerless) bytes returned by
goof_rom.load_and_validate_rom.  The opcode matrix is the public WDC 65C816
instruction set.
"""

from __future__ import annotations

from dataclasses import dataclass

CODE_BANKS = (0, 1, 2)          # physical LoROM banks holding CPU code (executed at $80-$82)

# mode -> operand length (None: depends on M or X flag)
MODE_LEN = {
    "imp": 0, "acc": 0, "imm8": 1, "immM": None, "immX": None,
    "dp": 1, "dpx": 1, "dpy": 1, "dpi": 1, "dpix": 1, "dpiy": 1, "dpil": 1, "dpily": 1,
    "sr": 1, "sriy": 1, "abs": 2, "absx": 2, "absy": 2, "absi": 2, "absix": 2, "absil": 2,
    "long": 3, "longx": 3, "rel8": 1, "rel16": 2, "blk": 2,
}

_ALU = ("ORA", "AND", "EOR", "ADC", "STA", "LDA", "CMP", "SBC")
_ALU_MODES = {0x1: "dpix", 0x3: "sr", 0x5: "dp", 0x7: "dpil", 0x9: "immM", 0xD: "abs", 0xF: "long",
              0x11: "dpiy", 0x12: "dpi", 0x13: "sriy", 0x15: "dpx", 0x17: "dpily", 0x19: "absy",
              0x1D: "absx", 0x1F: "longx"}

OPS: list[tuple[str, str]] = [("???", "imp")] * 256
for _i, _mn in enumerate(_ALU):
    for _lo, _mode in _ALU_MODES.items():
        OPS[(_i << 5) + _lo] = (_mn, _mode)
OPS[0x89] = ("BIT", "immM")          # STA #imm does not exist; slot is BIT #

_REST = {
    0x00: ("BRK", "imm8"), 0x02: ("COP", "imm8"), 0x04: ("TSB", "dp"), 0x06: ("ASL", "dp"),
    0x08: ("PHP", "imp"), 0x0A: ("ASL", "acc"), 0x0B: ("PHD", "imp"), 0x0C: ("TSB", "abs"),
    0x0E: ("ASL", "abs"), 0x10: ("BPL", "rel8"), 0x14: ("TRB", "dp"), 0x16: ("ASL", "dpx"),
    0x18: ("CLC", "imp"), 0x1A: ("INC", "acc"), 0x1B: ("TCS", "imp"), 0x1C: ("TRB", "abs"),
    0x1E: ("ASL", "absx"), 0x20: ("JSR", "abs"), 0x22: ("JSL", "long"), 0x24: ("BIT", "dp"),
    0x26: ("ROL", "dp"), 0x28: ("PLP", "imp"), 0x2A: ("ROL", "acc"), 0x2B: ("PLD", "imp"),
    0x2C: ("BIT", "abs"), 0x2E: ("ROL", "abs"), 0x30: ("BMI", "rel8"), 0x34: ("BIT", "dpx"),
    0x36: ("ROL", "dpx"), 0x38: ("SEC", "imp"), 0x3A: ("DEC", "acc"), 0x3B: ("TSC", "imp"),
    0x3C: ("BIT", "absx"), 0x3E: ("ROL", "absx"), 0x40: ("RTI", "imp"), 0x42: ("WDM", "imm8"),
    0x44: ("MVP", "blk"), 0x46: ("LSR", "dp"), 0x48: ("PHA", "imp"), 0x4A: ("LSR", "acc"),
    0x4B: ("PHK", "imp"), 0x4C: ("JMP", "abs"), 0x4E: ("LSR", "abs"), 0x50: ("BVC", "rel8"),
    0x54: ("MVN", "blk"), 0x56: ("LSR", "dpx"), 0x58: ("CLI", "imp"), 0x5A: ("PHY", "imp"),
    0x5B: ("TCD", "imp"), 0x5C: ("JML", "long"), 0x5E: ("LSR", "absx"), 0x60: ("RTS", "imp"),
    0x62: ("PER", "rel16"), 0x64: ("STZ", "dp"), 0x66: ("ROR", "dp"), 0x68: ("PLA", "imp"),
    0x6A: ("ROR", "acc"), 0x6B: ("RTL", "imp"), 0x6C: ("JMP", "absi"), 0x6E: ("ROR", "abs"),
    0x70: ("BVS", "rel8"), 0x74: ("STZ", "dpx"), 0x76: ("ROR", "dpx"), 0x78: ("SEI", "imp"),
    0x7A: ("PLY", "imp"), 0x7B: ("TDC", "imp"), 0x7C: ("JMP", "absix"), 0x7E: ("ROR", "absx"),
    0x80: ("BRA", "rel8"), 0x82: ("BRL", "rel16"), 0x84: ("STY", "dp"), 0x86: ("STX", "dp"),
    0x88: ("DEY", "imp"), 0x8A: ("TXA", "imp"), 0x8B: ("PHB", "imp"), 0x8C: ("STY", "abs"),
    0x8E: ("STX", "abs"), 0x90: ("BCC", "rel8"), 0x94: ("STY", "dpx"), 0x96: ("STX", "dpy"),
    0x98: ("TYA", "imp"), 0x9A: ("TXS", "imp"), 0x9B: ("TXY", "imp"), 0x9C: ("STZ", "abs"),
    0x9E: ("STZ", "absx"), 0xA0: ("LDY", "immX"), 0xA2: ("LDX", "immX"), 0xA4: ("LDY", "dp"),
    0xA6: ("LDX", "dp"), 0xA8: ("TAY", "imp"), 0xAA: ("TAX", "imp"), 0xAB: ("PLB", "imp"),
    0xAC: ("LDY", "abs"), 0xAE: ("LDX", "abs"), 0xB0: ("BCS", "rel8"), 0xB4: ("LDY", "dpx"),
    0xB6: ("LDX", "dpy"), 0xB8: ("CLV", "imp"), 0xBA: ("TSX", "imp"), 0xBB: ("TYX", "imp"),
    0xBC: ("LDY", "absx"), 0xBE: ("LDX", "absy"), 0xC0: ("CPY", "immX"), 0xC2: ("REP", "imm8"),
    0xC4: ("CPY", "dp"), 0xC6: ("DEC", "dp"), 0xC8: ("INY", "imp"), 0xCA: ("DEX", "imp"),
    0xCB: ("WAI", "imp"), 0xCC: ("CPY", "abs"), 0xCE: ("DEC", "abs"), 0xD0: ("BNE", "rel8"),
    0xD4: ("PEI", "dp"), 0xD6: ("DEC", "dpx"), 0xD8: ("CLD", "imp"), 0xDA: ("PHX", "imp"),
    0xDB: ("STP", "imp"), 0xDC: ("JML", "absil"), 0xDE: ("DEC", "absx"), 0xE0: ("CPX", "immX"),
    0xE2: ("SEP", "imm8"), 0xE4: ("CPX", "dp"), 0xE6: ("INC", "dp"), 0xE8: ("INX", "imp"),
    0xEA: ("NOP", "imp"), 0xEB: ("XBA", "imp"), 0xEC: ("CPX", "abs"), 0xEE: ("INC", "abs"),
    0xF0: ("BEQ", "rel8"), 0xF4: ("PEA", "abs"), 0xF6: ("INC", "dpx"), 0xF8: ("SED", "imp"),
    0xFA: ("PLX", "imp"), 0xFB: ("XCE", "imp"), 0xFC: ("JSR", "absix"), 0xFE: ("INC", "absx"),
}
for _op, _v in _REST.items():
    OPS[_op] = _v
assert all(mn != "???" for mn, _ in OPS), "opcode matrix incomplete"

BRANCHES = {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0}
RETURNS = {0x40, 0x60, 0x6B}                  # RTI RTS RTL
HALTS = {0x00, 0x02, 0xDB, 0x42}              # BRK COP STP WDM: never expected in real flow here


@dataclass(frozen=True)
class Insn:
    pc: int          # 16-bit PC
    op: int
    mn: str
    mode: str
    length: int
    operand: int     # little-endian operand value (0 if none)


def lorom(bank: int, pc: int) -> int:
    return (bank & 0x7F) * 0x8000 + (pc & 0x7FFF)


class Rom:
    """Read-only view of canonical LoROM bytes.  Identity checks belong to
    goof_rom; this class only decodes."""

    def __init__(self, data: bytes):
        if len(data) < 0x8000 * (max(CODE_BANKS) + 1):
            raise ValueError("ROM image too small for the configured code banks")
        self.d = bytes(data)

    def b(self, bank: int, pc: int) -> int:
        return self.d[lorom(bank, pc)]

    def w(self, bank: int, pc: int) -> int:
        return self.b(bank, pc) | self.b(bank, (pc + 1) & 0xFFFF) << 8

    def decode(self, bank: int, pc: int, m: int, x: int) -> Insn:
        op = self.b(bank, pc)
        mn, mode = OPS[op]
        n = MODE_LEN[mode]
        if n is None:
            n = (1 if m else 2) if mode == "immM" else (1 if x else 2)
        val = 0
        for i in range(n):
            val |= self.b(bank, (pc + 1 + i) & 0xFFFF) << (8 * i)
        return Insn(pc, op, mn, mode, 1 + n, val)

    def vectors(self) -> list[int]:
        out = []
        for off in (0x7FE4, 0x7FE6, 0x7FE8, 0x7FEA, 0x7FEE, 0x7FF4, 0x7FF8, 0x7FFA, 0x7FFC, 0x7FFE):
            t = self.d[off] | self.d[off + 1] << 8
            if t >= 0x8000:
                out.append(t)
        return out


def branch_target(ins: Insn) -> int:
    if ins.mode == "rel8":
        return (ins.pc + 2 + ((ins.operand ^ 0x80) - 0x80)) & 0xFFFF
    return (ins.pc + 3 + ((ins.operand ^ 0x8000) - 0x8000)) & 0xFFFF
