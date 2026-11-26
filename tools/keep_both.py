#!/usr/bin/env python3
"""Resolve git merge conflicts that are ADDITIVE (both sides inserted independent blocks): keep both sides.

Usage: python3 -I tools/keep_both.py FILE [FILE...]

For every conflict hunk it keeps HEAD's side followed by the other side, and prints one line per hunk. Hunks where
both sides are tiny (<= 2 lines) are flagged REVIEW: that usually means both sides edited the SAME line, where
concatenating is wrong and the lines must be merged by hand. Prints the brace/paren balance of the result so a
bad merge of C++ is noticed before it reaches the (slow, Windows-only) CI. Exit code 1 if anything needs review.
"""
import re
import sys

PAT = re.compile(r"<<<<<<< [^\n]*\n(.*?)=======\n(.*?)>>>>>>> [^\n]*\n", re.S)


def balance(text):
    return text.count("{") - text.count("}"), text.count("(") - text.count(")")


def main(paths):
    review = False
    for path in paths:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        hunks = []

        def repl(m):
            ours, theirs = m.group(1), m.group(2)
            n1, n2 = ours.count("\n"), theirs.count("\n")
            flag = "REVIEW" if n1 <= 2 and n2 <= 2 else "ok"
            hunks.append((n1, n2, flag))
            return ours + theirs

        merged = PAT.sub(repl, text)
        with open(path, "w", encoding="utf-8") as f:
            f.write(merged)
        for i, (n1, n2, flag) in enumerate(hunks, 1):
            print(f"{path}: hunk {i}: ours {n1} lines, theirs {n2} lines -> {flag}")
            review = review or flag == "REVIEW"
        b = balance(merged)
        print(f"{path}: balance after merge: braces {b[0]:+d} parens {b[1]:+d}" + ("" if b == (0, 0) else "  <-- NOT 0, check"))
        if path.endswith((".cpp", ".hpp", ".h")) and b != (0, 0):
            review = True
        if path.endswith((".yml", ".yaml", ".cmake", "CMakeLists.txt")) and hunks:
            print(f"{path}: structured file - concatenating can scramble steps/blocks, REVIEW by hand")
            review = True
    return 1 if review else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
