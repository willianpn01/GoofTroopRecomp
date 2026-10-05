"""JSL must push the live logical PB, not the CFG's physical bank."""
from v2.codegen import _emit_return_frame_push
from v2.ir import Call


def test_jsl_return_frame_uses_live_guest_pb_not_physical_source_bank():
    # Arbitrary physical site/target. At runtime cpu->PB may be a logical
    # mirror (for example $A7) and must not be baked in from this $23 site.
    op = Call(target=0x45CDEF, long=True, source_pc24=0x23A6B9)
    src = "\n".join(_emit_return_frame_push(op))
    assert "cpu->S, cpu->PB" in src, src
    assert "cpu->S, 0x23" not in src.splitlines()[1], src
    assert "cpu->S, 0xa6" in src and "cpu->S, 0xbc" in src, src
