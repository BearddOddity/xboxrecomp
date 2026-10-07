"""
RECOMP_SAVE_SANDBOX sends the hard disk's save folders elsewhere.

Run: py -3 -m pytest tools/kernel_audit/test_save_sandbox.py

A test copy of a title and the player's copy share one game folder, and so
one UDATA. An XDK title deletes a save container it judges invalid; a test
copy force-closed mid-write left one, the next test run deleted it, and the
player's save slots were gone.

Since v0.15.0 the UDATA, TDATA and CACHE rules map under one root (s_user_dir,
RECOMP_SAVE_DIR when set). The sandbox is that root when RECOMP_SAVE_DIR is
not set, and like it turns off the fallback that keeps using saves found in
the game dir, which is where the player's saves are.
"""

import os
import re

SRC = os.path.join(os.path.dirname(__file__), "..", "..", "src", "kernel", "kernel_path.c")


def source():
    with open(SRC, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def save_root_block():
    src = source()
    start = src.index('const char *ov = getenv("RECOMP_SAVE_DIR");')
    return src[start:src.index("s_initialized = TRUE;", start)]


def test_sandbox_reads_its_variable_after_save_dir():
    block = save_root_block()
    assert re.search(r'if \(!ov \|\| !ov\[0\]\)\s*ov = getenv\("RECOMP_SAVE_SANDBOX"\);', block)


def test_sandbox_disables_the_game_dir_fallback():
    # the fallback to saves in the game dir is off whenever the root is overridden
    assert "s_user_in_game[k] = !(ov && ov[0])" in save_root_block()


def test_only_user_rules_follow_the_root():
    src = source()
    assert 'Partition1\\\\UDATA", 2,' in src
    assert 'Partition1\\\\TDATA", 2,' in src
