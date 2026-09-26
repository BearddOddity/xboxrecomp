"""
Self-check: the jcc after `repe cmpsd` tests the comparison, at every width.

MSVC compares GUIDs with

    xor  edx, edx
    repe cmpsd            ; ZF = the 16 bytes matched
    jne  not_this_one

Only cmpsb and scasb counted as flag setters, so the jne resolved against the
`xor` before it: "not equal" was a constant false, and the XDK WMA decoder's
ASF walker took every header object for the first GUID it tested, then failed
to parse. The game built its audio format from a stream with 0 channels and
DirectSound divided by zero.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _translate(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="string-cmp-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def _check(opcode, name):
    #   +0  xor edx, edx
    #   +2  repe <opcode>
    #   +4  jne +1      -> ret at +7
    #   +6  nop
    #   +7  ret
    image = b"\x31\xD2" + b"\xF3" + opcode + b"\x75\x01\x90\xC3"
    code = _translate(image)
    jne = [l for l in code.splitlines() if "jne" in l]
    assert jne, code
    assert "_flags == 0" in jne[0], (name, jne[0])
    assert "_fa" not in jne[0], (name, jne[0])


def test_every_width_sets_the_flags_the_jcc_reads():
    _check(b"\xA6", "cmpsb")
    _check(b"\x66\xA7", "cmpsw")  # 66 prefix: 16-bit
    _check(b"\xA7", "cmpsd")
    _check(b"\xAF", "scasd")


if __name__ == "__main__":
    test_every_width_sets_the_flags_the_jcc_reads()
    print("ok")
