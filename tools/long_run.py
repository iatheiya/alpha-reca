#!/usr/bin/env python3
"""long_run.py — resumable, checkpointed long runs of self-hosted LOAD_MAIN.

Built during the session that found and fixed the `RA_HT_SLOT`-reset bug
(BUGS.md, forty-fifth round) — investigating that bug needed billions of
interpreter steps, far more than a single tool invocation's practical
wall-clock budget allows. Restarting from step 0 every time wastes all
prior progress; this lets a run be paused and resumed across separate
invocations instead.

HOW IT WORKS
    First invocation (no checkpoint file yet): does the normal Python
    bootstrap (same as `loader.freeze()`), then runs the self-hosted
    LOAD_MAIN interpreter loop for STEPS_PER_INVOCATION steps using a
    fast raw dispatch loop (no per-instruction Python-level tracing
    overhead — this is NOT `trace.py`'s traced mode, it's the same
    speed class as the real, untraced `Interpreter._run`). Then stashes
    the current PC and cumulative step count into two reserved, always-
    unused aether addresses near the very top of the address space, and
    calls `Aether.freeze()` with a watermark covering the *entire*
    aether (not just the touched/bump region — the call stack lives at
    the very top, growing down from AETHER_SIZE-1, and gets silently
    dropped by a too-low watermark; this cost real debugging time to
    discover the first time).

    Subsequent invocations (checkpoint file exists): thaw the
    checkpoint into a fresh `Interpreter`, call `update_relations()`
    with a symbol table read from `reca.sym` (addresses are
    deterministic across builds of unchanged source, so a stale-but-
    matching symbol file is fine — regenerate it with a plain
    `python3 -c "from loader import freeze; freeze()"` first if the
    source changed since the checkpoint was taken), read back the
    stashed PC, and continue the raw dispatch loop from exactly there.

USAGE
    # Start (or continue) a long run, up to --steps more steps this call:
    python3 long_run.py --steps 400000000

    # Check progress without running more (just prints current status):
    python3 long_run.py --status

    # Start over from scratch (discards any existing checkpoint):
    python3 long_run.py --fresh --steps 400000000

    # Chain multiple invocations from a shell loop (each call resumes
    # automatically once a checkpoint exists):
    for i in 1 2 3; do python3 long_run.py --steps 400000000; done

STATUS FILE
    Written to `long_run_status.txt` after every CHECKPOINT_EVERY steps
    and at the end of each invocation: this_run_step, cumulative_step,
    wall_seconds, sk_fidx, rpos, msg. `msg` is one of: "running",
    "checkpointed, not done", "FINISHED NATURALLY" (pc reached 0 --
    either genuine completion or an unexpected halt worth investigating
    with `check_graph_equivalence.py`/`trace.py`), or an "UNKNOWN OP"/
    "EXCEPTION" diagnostic if something went wrong.

CAVEATS
    - SK_FIDX_ADDR/RA_LOAD_RPOS_ADDR below are read once at import time
      from reca.sym if present (falling back to the last-known-good
      values hardcoded as a default) — these are just progress-display
      addresses, not correctness-critical, but will silently show -1 if
      genuinely stale after a structural source change.
    - The reserved PC_SLOT/CUM_STEPS_SLOT addresses are chosen near
      AETHER_SIZE's own top (see aether.py) specifically to avoid
      colliding with real bump-allocated content or the stack region
      (which grows down from AETHER_SIZE-1) -- do not lower these
      without re-checking both boundaries.
    - This intentionally reimplements the raw dispatch loop rather than
      reusing `trace.py`'s `make_patched_run` (which wraps every
      individual op with Python-level overhead for its callback
      mechanism) -- that overhead is fine for the *targeted* watches
      `make_patched_run` is built for, but far too slow to reach deep
      into a multi-billion-step run within reasonable wall time. Keep
      this loop's op-dispatch logic (SLOT_*/NOVA_SIZE, the fallthrough-
      vs-explicit-Next computation) in sync with `interpreter.py`'s own
      `_run` if that ever changes.
"""
import sys, time, io, os, argparse
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)
import loader as _ldr
import interpreter as _imod

CKPT_PATH = os.path.join(_ROOT, 'long_run_ckpt.bin')
STATUS_FILE = os.path.join(_ROOT, 'long_run_status.txt')
SYM_PATH = os.path.join(_ROOT, 'reca.sym')

PC_SLOT = 7_999_999
CUM_STEPS_SLOT = 7_999_998
AETHER_TOP_WATERMARK = 8_388_607  # AETHER_SIZE - 1 (aether.py) -- covers
                                   # everything including the stack region

CHECKPOINT_EVERY = 20_000_000

# Progress-display addresses only, not correctness-critical -- refreshed
# from reca.sym if present.
SK_FIDX_ADDR = 3926
RA_LOAD_RPOS_ADDR = 372


def _load_progress_addrs():
    global SK_FIDX_ADDR, RA_LOAD_RPOS_ADDR
    if os.path.exists(SYM_PATH):
        with open(SYM_PATH) as f:
            for line in f:
                addr, name = line.rstrip('\n').split('\t')
                if name == 'SK_FIDX':
                    SK_FIDX_ADDR = int(addr)
                elif name == 'RA_LOAD_RPOS':
                    RA_LOAD_RPOS_ADDR = int(addr)


def _load_symbols():
    symbols = {}
    with open(SYM_PATH) as f:
        for line in f:
            addr, name = line.rstrip('\n').split('\t')
            symbols[name] = int(addr)
    return symbols


t0 = time.time()


def write_status(step, cumulative, fidx, rpos, msg=""):
    with open(STATUS_FILE, 'w') as f:
        f.write(f"this_run_step={step}\ncumulative_step={cumulative}\n")
        f.write(f"wall_seconds={time.time()-t0:.1f}\nsk_fidx={fidx}\nrpos={rpos}\n")
        f.write(f"msg={msg}\n")


class _Checkpoint(SystemExit):
    """Raised to unwind out of freeze()/execute_aether() cleanly once the
    step budget for this invocation is spent, without running any of
    freeze()'s own finalization (symbol-writing, stats printing)."""


def _make_patched_run(resume_pc, cumulative_start, steps_this_run):
    def patched_run(self, start, progress_every=0, sym_arg=None, use_cache=True, max_steps=0):
        a = self.aether.aether
        SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT = 1, 2, 3, 4, 5
        NOVA_SIZE = 7
        dispatch = self._dispatch
        pc = resume_pc if resume_pc else start
        steps = 0
        while pc:
            steps += 1
            if steps % CHECKPOINT_EVERY == 0:
                write_status(steps, cumulative_start + steps, int(a[SK_FIDX_ADDR]),
                             int(a[RA_LOAD_RPOS_ADDR]), "running")
            if steps >= steps_this_run:
                a[PC_SLOT] = pc
                a[CUM_STEPS_SLOT] = cumulative_start + steps
                self.aether.freeze(CKPT_PATH, AETHER_TOP_WATERMARK)
                write_status(steps, cumulative_start + steps, int(a[SK_FIDX_ADDR]),
                             int(a[RA_LOAD_RPOS_ADDR]), "checkpointed, not done")
                raise _Checkpoint()
            op_id = a[pc + SLOT_OP]
            fn = dispatch.get(op_id)
            if fn is None:
                write_status(steps, cumulative_start + steps, int(a[SK_FIDX_ADDR]),
                             int(a[RA_LOAD_RPOS_ADDR]), f"UNKNOWN OP at pc={pc}")
                break
            e1 = a[pc + SLOT_E1]; e2 = a[pc + SLOT_E2]; ex = a[pc + SLOT_EXIT]
            raw = a[pc + SLOT_NEXT]
            nxt = pc + NOVA_SIZE if raw == 0 else raw
            res = fn(e1, e2, ex, nxt, a)
            pc = res if res is not None else nxt
        write_status(steps, cumulative_start + steps, int(a[SK_FIDX_ADDR]),
                     int(a[RA_LOAD_RPOS_ADDR]), "FINISHED NATURALLY")
        return pc
    return patched_run


def run(steps_this_run: int, fresh: bool = False):
    if fresh and os.path.exists(CKPT_PATH):
        os.remove(CKPT_PATH)
    resume_mode = os.path.exists(CKPT_PATH)
    _load_progress_addrs()

    if not resume_mode:
        write_status(0, 0, -1, -1, "starting fresh bootstrap")
        _imod.Interpreter._run = _make_patched_run(None, 0, steps_this_run)
        buf = io.StringIO(); old_err = sys.stderr; sys.stderr = buf
        try:
            _ldr.freeze()
        except _Checkpoint:
            pass
        except Exception as e:
            write_status(-1, -1, -1, -1, f"EXCEPTION: {e}")
            raise
        finally:
            sys.stderr = old_err
    else:
        if not os.path.exists(SYM_PATH):
            print(f"ERROR: {SYM_PATH} not found -- need a symbol table to "
                  f"resume (run a plain freeze() once first if the source "
                  f"has changed since the checkpoint).", file=sys.stderr)
            sys.exit(1)
        write_status(0, -1, -1, -1, "resuming from checkpoint")
        symbols = _load_symbols()
        interp = _imod.Interpreter()
        interp.aether.thaw(CKPT_PATH)
        interp.update_relations(symbols)
        a = interp.aether.aether
        resume_pc = int(a[PC_SLOT])
        cumulative_start = int(a[CUM_STEPS_SLOT])
        _imod.Interpreter._run = _make_patched_run(resume_pc, cumulative_start, steps_this_run)
        try:
            interp.execute_aether(resume_pc, no_cache=True)
        except _Checkpoint:
            pass

    print("wall this invocation:", round(time.time() - t0, 1))
    if os.path.exists(STATUS_FILE):
        print(open(STATUS_FILE).read())


def print_status():
    if not os.path.exists(STATUS_FILE):
        print("No status file yet -- no run has been started.")
        return
    print(open(STATUS_FILE).read())


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--steps', type=int, default=400_000_000,
                    help='steps to run this invocation (default 400M)')
    p.add_argument('--fresh', action='store_true',
                    help='discard any existing checkpoint, start over')
    p.add_argument('--status', action='store_true',
                    help='just print current status, do not run')
    args = p.parse_args()
    if args.status:
        print_status()
    else:
        run(args.steps, fresh=args.fresh)
