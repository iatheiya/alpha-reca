#!/usr/bin/env python3
"""check_undef_targets.py -- undefined call/jump targets.

Catches a whole class of latent bug that stays invisible until the exact
control-flow edge is first taken: a symbol used as a *call/jump target*
(`Body=` on RVOCA/RREDI, or `Full=`/`Exhausted=` on PACK_BYTE-style
jumps) that is referenced but never given a body, so its word stays 0. A
`RVOCA Body=X` reads a[X] as the jump target; when a[X]==0 the very first
call to it Voca's to address 0 and the interpreter faults.

These sat dormant for a long time because the self-hosted load wave --
the only thing that actually *executes* these targets during a build --
did not run to completion, so the edges were never taken. Once it did,
they surfaced one crash at a time: GL_IMPL, PGB_EB_FLUSH, LINK_IMPL,
SAVE_EMIT_MOVE_LUX -- each a section whose entry alias was simply never
defined (the first node of the body carried a different label, and no
`SET <entry> Value=<first-node>` tied the name to it). This check finds
them all at once, statically, from the built word table -- so a new one
is caught at build time instead of as a mid-self-hosting fault.

Scope note: only the unambiguous call/jump-target keywords are scanned
(`Body=`/`Full=`/`Exhausted=`). Positional jump targets (a JEQ/JZ
destination, a `Jump`/`JumpIf` `Exit=`) are deliberately NOT scanned: those
routinely name a *register* whose value is filled at runtime (RA_MA1,
SK_ARG_*, ...), which has word 0 at build time for a legitimate reason, so
including them turns a zero-false-positive check into a noisy one. The
Body=/Full=/Exhausted= class caught every real instance of this bug.

    python3 check_undef_targets.py        # standalone
    python3 iris.py --undef-targets       # via the hub
    python3 iris.py --checks              # part of the aggregate run
"""
import sys, os, re

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

# Comment-free target extraction. A '/' begins a line comment in .re, so cut
# there first; the keywords we want never contain one.
_KEY = re.compile(r'\b(?:Body|Full|Exhausted)=([A-Za-z_][A-Za-z0-9_]*)')


def collect_targets(re_files):
    """name -> list of "file:line" where it is used as a call/jump target."""
    targets = {}
    for fp in re_files:
        try:
            lines = open(fp, encoding='utf-8', errors='replace').readlines()
        except OSError:
            continue
        base = os.path.basename(fp)
        for i, line in enumerate(lines, 1):
            src = line.split('/', 1)[0]
            for m in _KEY.finditer(src):
                targets.setdefault(m.group(1), []).append(f"{base}:{i}")
    return targets


def find_undefined(targets, symbols, aether):
    """Return [(name, why, where[:3])] for every target with no real body."""
    bad = []
    for name in sorted(targets):
        ad = symbols.get(name, 0)
        if ad == 0:
            bad.append((name, "NO SYMBOL", targets[name][:3]))
        elif int(aether[ad]) == 0:
            bad.append((name, f"word=0 @{ad}", targets[name][:3]))
    return bad


def report(targets, bad):
    print(f"Scanned {len(targets)} distinct Body=/Full=/Exhausted= targets.")
    if not bad:
        print("No undefined call/jump targets ✓")
        return 0
    print(f"\n✗ {len(bad)} UNDEFINED call/jump target(s) "
          f"(referenced but word=0 -- would fault the first time reached):")
    for name, why, where in bad:
        print(f"  {name:<28} {why:<16} {', '.join(where)}")
    print("\nFix each like the entry-alias idiom "
          "`SET <target> Value=<first-node-of-its-body>` (cf. GL_IMPL).")
    return 1


# ── iris hub integration ────────────────────────────────────────────────────
from iris import Command


def _cmd_undef_targets(ctx) -> int:
    targets = collect_targets(ctx.re_files)
    bad = find_undefined(targets, ctx.symbols, ctx.aether)
    return report(targets, bad)


COMMANDS = [
    Command("undef-targets", "fresh", _cmd_undef_targets, is_check=True,
            help="find call/jump targets (Body=/Full=/Exhausted=) that are "
                 "referenced but never defined (word=0)"),
]


def main(argv):
    # Standalone: build the Python side and scan it.
    import io
    import loader as _ldr
    _buf, _old = io.StringIO(), sys.stderr
    sys.stderr = _buf
    try:
        loader, _all_files, _flb = _ldr._freeze_prepare()
    finally:
        sys.stderr = _old
    a = loader.interp.aether.aether
    re_files = __import__("loader").re_sources(_ROOT)
    targets = collect_targets(re_files)
    bad = find_undefined(targets, loader.symbols, a)
    return report(targets, bad)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
