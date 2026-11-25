"""Unit checks for tools/check_content.py. Standard library only."""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import check_content as cc  # noqa: E402


def check(name: str, content: bytes, expect_problem: bool) -> bool:
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / name
        p.write_bytes(content)
        got = bool(cc.problems_for(p))
    ok = got == expect_problem
    print(f"{'ok  ' if ok else 'FAIL'} {name}: problem={got}, expected={expect_problem}")
    return ok


def main() -> int:
    results = [
        check("notes.md", b"# hello\n", False),
        check("game.nsp", b"x", True),
        check("pakchunk0.pak", b"x", True),
        check("Pikmin4.utoc", b"x", True),
        check("main", b"x", True),
        check("prod.txt", b"header_key = " + b"0" * 64 + b"\n", True),
        check("gen.c", ("/* " + cc.GENERATED_MARKER + " */").encode(), True),
        check("big.bin", b"\0" * (cc.MAX_BYTES + 1), True),
    ]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
