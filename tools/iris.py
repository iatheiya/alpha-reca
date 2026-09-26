#!/usr/bin/env python3
"""iris.py — the eye: single entry point for every diagnostic and check.

iris is the hub. It owns nothing but the contract: what a command is, how
a command gets the build it needs, and how the aggregate `--checks` run
shares one build across everything. The actual commands live in theme
modules (iris_source, iris_graph, iris_trace) — iris only dispatches.

The contract, in one shape:

  A command declares NEEDS (one of "text"/"fresh"/"freeze"/"trace") and a
  run(ctx) -> int. That is the whole interface. A command that is also a
  check (participates in the aggregate `--checks` run) sets is_check=True
  and returns 0 when clean, 1 on a finding. A command that is a report
  (health, wiring, ...) just prints and returns 0.

The build hierarchy, why it matters:

  text  ⊂  fresh  ⊂  freeze

  - "text"  needs nothing built — reads .re files as source.
  - "fresh" needs fresh_loader(): thaws reca.bin, no rebuild. Gives the
            symbol table + aether, but not Wave-1 bookkeeping.
  - "freeze" needs a real freeze(): full rebuild, every wave, all
            bookkeeping. A freeze() satisfies fresh and text too.
  - "trace" needs its own patched/traced run and cannot share a build.

  So the runner builds the *single most powerful resource any selected
  command asks for*, once, and serves the weaker needs from it. If any
  check needs freeze, one freeze() covers the fresh and text checks as
  well — no extra rebuilds. This is the whole reason run_all_checks.py
  existed; it now lives here, generalised.

Usage:
    python3 iris.py                     # default health + graph overview
    python3 iris.py --checks            # run every check, one shared build
    python3 iris.py --<command> [args]  # run one command (see --list)
    python3 iris.py --list              # list every registered command
"""
import io
import sys
import os
import shutil

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

# Command need levels, ordered weakest -> strongest. A resource built for a
# stronger need satisfies every weaker one (text ⊂ fresh ⊂ freeze). "trace"
# is off this ladder: it never shares, each trace command runs its own.
_NEED_RANK = {"text": 0, "fresh": 1, "freeze": 2}


class Command:
    """One diagnostic or check. Theme modules build these and expose them
    in a module-level COMMANDS list; iris discovers them from there.

    name     CLI flag without the leading dashes (e.g. "invariants" for
             `iris --invariants`). Also its identity in --list.
    needs    "text" | "fresh" | "freeze" | "trace" — what build it consumes.
    run      callable(ctx) -> int. 0 clean / prints report; 1 on a finding.
    is_check whether it participates in the aggregate `--checks` run.
    help     one-line description for --list.
    takes_args whether the command reads extra argv (e.g. --parity FILE);
             such commands receive (ctx, args) instead of (ctx).
    """

    __slots__ = ("name", "needs", "run", "is_check", "help", "takes_args",
                 "_module")

    def __init__(self, name, needs, run, *, is_check=False, help="",
                 takes_args=False):
        assert needs in ("text", "fresh", "freeze", "trace"), needs
        self.name = name
        self.needs = needs
        self.run = run
        self.is_check = is_check
        self.help = help
        self.takes_args = takes_args


class Context:
    """The build a command runs against, produced lazily and once.

    A command asks only for what its NEEDS entitles it to:
      ctx.re_files  — sorted list of .re source paths (always available)
      ctx.loader    — a built Loader (fresh or freeze, per the run's level)
      ctx.aether    — ctx.loader.interp.aether.aether
      ctx.symbols   — ctx.loader.symbols (name -> addr)
      ctx.rev       — reverse map (addr -> name), built once
      ctx.errs      — build-time error lines captured on stderr (Pass2/Error)

    `level` is the resource actually built for this run ("text"/"fresh"/
    "freeze"). A text-only run never builds a loader; touching ctx.loader
    then is a programming error (a command asked for more than it declared).

    "fresh" uses loader.load_or_freeze() (thaw reca.bin when fresh, rebuild
    when a .re source is newer) rather than a blind fresh_loader() thaw —
    so a command never silently runs against a stale build after an edit.
    """

    def __init__(self, level, root=_ROOT):
        self._level = level
        self._root = root
        self._loader = None
        self._rev = None
        self._errs = []

    @property
    def level(self):
        return self._level

    @property
    def re_files(self):
        """Every .re source in the project, sorted — including subdirectories
        (generated/). Recursive on purpose: the old per-check globs were
        split, most using a root-only '*.re' that silently skipped the three
        files under generated/, while diag and check_nova_sizing scanned
        recursively. One definition here keeps every command looking at the
        same 42 files the loader itself builds."""
        from loader import re_sources
        return re_sources(self._root)

    @property
    def loader(self):
        if self._loader is None:
            import loader as _loader_mod
            if self._level == "freeze":
                self._loader, self._errs = _build_quiet(_loader_mod.freeze)
            elif self._level == "fresh":
                def _fresh():
                    interp, symbols = _loader_mod.load_or_freeze()
                    return _BuiltLoader(interp, symbols)
                self._loader, self._errs = _build_quiet(_fresh)
            else:
                raise RuntimeError(
                    f"command asked for ctx.loader but this run only built "
                    f"'{self._level}' — its NEEDS understates what it uses")
        return self._loader

    @property
    def aether(self):
        return self.loader.interp.aether.aether

    @property
    def symbols(self):
        return self.loader.symbols

    @property
    def rev(self):
        if self._rev is None:
            self._rev = {v: k for k, v in self.symbols.items()}
        return self._rev

    @property
    def errs(self):
        self.loader  # ensure build ran so errs are populated
        return self._errs


class _BuiltLoader:
    """Minimal Loader stand-in exposing only .interp/.symbols, for the
    fresh (thaw) path where load_or_freeze() returns (interp, symbols)
    rather than a full Loader. Commands that only read the frozen graph
    need nothing more; a full freeze() gives a real Loader with build-time
    bookkeeping for the few that do."""
    __slots__ = ("interp", "symbols")

    def __init__(self, interp, symbols):
        self.interp = interp
        self.symbols = symbols


def _build_quiet(build_fn):
    """Run a loader build with freeze() noise suppressed, returning
    (result, error_lines). Error lines are the Pass2/Error stderr messages
    a build emits — surfaced via ctx.errs for reports like health that
    show them, hidden from everything else."""
    buf = io.StringIO()
    old = sys.stderr
    sys.stderr = buf
    try:
        result = build_fn()
    finally:
        sys.stderr = old
    errs = [l for l in buf.getvalue().splitlines() if 'Pass2' in l or 'Error' in l]
    return result, errs


# ── Command registry ──────────────────────────────────────────────────────
# Two sources of commands, both collected the same way (each exposes a
# module-level COMMANDS list):
#   theme modules   — small related commands grouped by theme (source/graph/
#                     trace); the command's logic lives in the module.
#   standalone      — large, self-contained tools that keep their own file
#                     (comment analyser, macro tests) but plug into the hub.
# Kept as explicit lists (not auto-glob) so the live set is visible here.
_THEME_MODULES = ("iris_source", "iris_graph", "iris_trace")
_STANDALONE_MODULES = ("check_comments", "test_macros", "check_htable_reentrancy",
                       "check_undef_targets")


def _load_commands():
    """Import every registered module and collect its COMMANDS into one dict
    keyed by name. A duplicate name across modules is a real bug (two
    commands claiming the same flag) and fails loudly."""
    registry = {}
    for modname in _THEME_MODULES + _STANDALONE_MODULES:
        try:
            mod = __import__(modname)
        except ModuleNotFoundError as e:
            # Skip only a module that genuinely does not exist. A module that
            # exists but fails to import one of ITS dependencies (e.g. loader
            # not on sys.path) must fail loudly -- swallowing that silently
            # made iris show 6 commands instead of all of them.
            if e.name != modname:
                raise
            continue  # module not written yet — iris still runs the rest
        for cmd in getattr(mod, "COMMANDS", ()):
            if cmd.name in registry:
                raise RuntimeError(
                    f"duplicate command name {cmd.name!r}: "
                    f"{modname} and {registry[cmd.name]._module}")
            cmd._module = modname
            registry[cmd.name] = cmd
    return registry


def _run_one(cmd, args):
    """Build the single resource `cmd` needs, then run it. For a lone
    command this is just its own level; the shared-build optimisation only
    matters for the aggregate `--checks` path (see run_checks)."""
    level = "text" if cmd.needs in ("text", "trace") else cmd.needs
    ctx = Context(level)
    if cmd.takes_args:
        return cmd.run(ctx, args)
    return cmd.run(ctx)


def run_checks(registry):
    """Aggregate run: every is_check command, sharing one build.

    Groups the checks by need, builds the single strongest shared resource
    (freeze if any check needs it, else fresh, else nothing) once, and runs
    every text/fresh/freeze check against it. Trace checks each run their
    own patched build (they cannot share) after the shared group.
    """
    checks = [c for c in registry.values() if c.is_check]
    shared = [c for c in checks if c.needs in ("text", "fresh", "freeze")]
    traced = [c for c in checks if c.needs == "trace"]

    exit_code = 0

    if shared:
        top = max((_NEED_RANK[c.needs] for c in shared), default=0)
        level = {0: "text", 1: "fresh", 2: "freeze"}[top]
        note = {"text": "no build needed",
                "fresh": "one shared fresh_loader()",
                "freeze": "one shared freeze()"}[level]
        print(f"Shared build: {note}\n")
        ctx = Context(level)
        for cmd in shared:
            exit_code = _run_check(cmd, ctx) or exit_code

    for cmd in traced:
        # Each trace check owns its patched run; give it a fresh text-level
        # ctx (it builds its own traced loader internally).
        exit_code = _run_check(cmd, Context("text")) or exit_code

    print("=" * 70)
    print("✓ All checks clean." if not exit_code
          else "✗ One or more checks reported findings -- see above.")
    print("=" * 70)
    return exit_code


def _run_check(cmd, ctx):
    print("=" * 70)
    print(cmd.name)
    print("=" * 70)
    rc = cmd.run(ctx, []) if cmd.takes_args else cmd.run(ctx)
    print()
    return rc


def _print_list(registry):
    print("iris commands:\n")
    order = {"text": 0, "fresh": 1, "freeze": 2, "trace": 3}
    for cmd in sorted(registry.values(), key=lambda c: (order[c.needs], c.name)):
        tag = "check" if cmd.is_check else "report"
        print(f"  --{cmd.name:<22} [{cmd.needs:<6} {tag}]  {cmd.help}")


def _clean(_HERE=None) -> int:
    """Remove every build artifact this project accumulates: reca.bin,
    reca.sym, and every __pycache__ directory. A nuclear option --
    load_or_freeze()'s own staleness check (_bin_is_stale, mtime-based
    against every .re/.py source) already handles the common case
    correctly on its own; this exists for when something's actually
    wrong (a manually-mutated .bin from a debug script, a suspected
    stale .pyc) and you want to rule out "the cache" as a variable
    entirely rather than track down which specific file is stale.
    """
    import loader as _loader_mod
    removed = []
    for p in (_loader_mod._BIN, _loader_mod._SYM):
        if os.path.exists(p):
            os.remove(p)
            removed.append(p)
    for root, dirs, _files in os.walk(_loader_mod._HERE):
        if '__pycache__' in dirs:
            shutil.rmtree(os.path.join(root, '__pycache__'))
            removed.append(os.path.join(root, '__pycache__'))
            dirs.remove('__pycache__')
    if removed:
        print(f"Removed {len(removed)} item(s):")
        for r in removed:
            print(f"  {r}")
    else:
        print("Already clean -- nothing to remove.")
    return 0


def main(argv):
    if "--clean" in argv:
        return _clean()

    registry = _load_commands()

    if "--list" in argv:
        _print_list(registry)
        return 0

    if "--checks" in argv:
        return run_checks(registry)

    # Single command: first --flag that names a registered command.
    for i, tok in enumerate(argv):
        if tok.startswith("--"):
            name = tok[2:]
            if name in registry:
                return _run_one(registry[name], argv[i + 1:])

    # Default: health overview, then lint (the old bare `diag.py` showed
    # health + graph; graph's checks now live inside lint).
    if "health" in registry and "lint" in registry:
        ctx = Context("freeze")
        registry["health"].run(ctx)
        print()
        registry["lint"].run(ctx, [])
        return 0

    _print_list(registry)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
