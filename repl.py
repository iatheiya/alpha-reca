"""
repl.py — Reca entry point

Python's only job: boot the Aether from reca.bin, find the entry Lux, run it.

Boot paths:
  FAST: reca.bin exists → thaw raw bits, run
  COLD: reca.bin missing → call loader.freeze() first, then run

Runtime .re parsing is handled entirely by parser.re (PS_MAIN).
To load a .re file at runtime:
  python3 repl.py --load my_program.re   # Python opens fd, parser.re reads it
  python3 repl.py < my_program.re        # pipe via stdin
"""
import sys
import os
import tempfile

from interpreter import Interpreter
from loader import load_symbols, freeze as do_freeze, _BIN, _SYM
from symphony import ITO_SIZE

_HERE = os.path.dirname(os.path.abspath(__file__))


def _capture_fd1(fn) -> str:
    """Capture fd-1 (stdout) writes from Reca's SYS_WRITE syscalls."""
    with tempfile.NamedTemporaryFile(mode='wb', suffix='.ll', delete=False) as tf:
        tmp = tf.name
    try:
        real = os.dup(1)
        fd   = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.dup2(fd, 1); os.close(fd)
        fn()
        os.dup2(real, 1); os.close(real)
        with open(tmp, 'rb') as f:
            return f.read().decode('utf-8', errors='replace')
    finally:
        try: os.unlink(tmp)
        except OSError: pass


def boot() -> tuple:
    """
    Boot the Aether. Fast path if reca.bin exists, slow path (freeze) otherwise.
    Returns (interp, symbols).
    Zero .re syntax knowledge — Python reads raw Aether bits from .bin
    on the fast path; on the cold path it reuses the freshly built loader
    state in aether (no redundant thaw + .sym re-parse).
    """
    if os.path.exists(_BIN) and os.path.exists(_SYM):
        interp = Interpreter()
        interp.aether.thaw(_BIN)
        symbols = load_symbols()
    else:
        loader = do_freeze()
        interp  = loader.interp
        symbols = loader.symbols
    interp.update_relations(symbols)
    interp._symbol_names = {v: k for k, v in symbols.items()}
    return interp, symbols


def _find_entry(interp, name: str | None = None) -> int:
    """Find entry point: by name, or scan for lux with Entry lumen → Yaku.

    Entry points are marked via: LINK MyFunc Entry Yaku
    ITO luces store extra lumens (from LINK commands) starting at slot 7 (ITO_SIZE).
    Data luces store lumens starting at slot 1.
    Lux kind: slot 1 != 0 → ITO (op field set); slot 1 == 0 → Data.
    Checks (rel=Entry, exit=Yaku) in the appropriate lumen region.
    """
    if name:
        addr = interp.R.get(name, 0)
        if addr: return addr
    entry_addr = interp.R.get("Entry", 0)
    yaku_addr  = interp.R.get("Yaku", 0)
    if not entry_addr or not yaku_addr:
        return 0
    a    = interp.aether.aether
    alen = len(a)

    def _scan_lumens(addr: int, start: int) -> bool:
        i = start
        while addr + i + 1 < alen:
            rel = a[addr + i]
            if not rel:
                break
            if rel == entry_addr and a[addr + i + 1] == yaku_addr:
                return True
            i += 2
        return False

    for addr in interp.R.values():
        if not addr or addr >= alen or not a[addr]:
            continue
        is_ito = addr + 1 < alen and a[addr + 1] != 0
        start  = ITO_SIZE if is_ito else 1
        if _scan_lumens(addr, start):
            return addr
    return 0


def _redirect_stdin_to(filepath: str) -> int:
    """
    Open filepath and redirect fd 0 (stdin) to it.
    Returns the saved real stdin fd so the caller can restore if needed.
    Python only opens the file descriptor — it never reads or parses the content.
    parser.re (PS_MAIN) will read the bytes through its normal sys_read path.
    """
    if not os.path.exists(filepath):
        print(f"Error: '{filepath}' not found", file=sys.stderr)
        sys.exit(1)
    real_stdin = os.dup(0)
    fd = os.open(filepath, os.O_RDONLY)
    os.dup2(fd, 0)
    os.close(fd)
    return real_stdin


def _inject_reca_init(ir: str) -> str:
    """Inject reca_init.ll before @main in LLVM IR, if the file exists.

    Used by both --self-compile and --combine paths.
    """
    ri_path = os.path.join(_HERE, 'reca_init.ll')
    if not os.path.exists(ri_path):
        return ir
    with open(ri_path) as f:
        ri_text = f.read().strip() + '\n\n'
    marker = 'define i64 @main()'
    pos = ir.find(marker)
    if pos < 0:
        return ir
    return ir[:pos] + ri_text + ir[pos:]


if __name__ == "__main__":
    els = sys.argv[1:]

    def _arg(flag, default=None):
        """Return value after flag, or default if flag absent."""
        if flag in els:
            i = els.index(flag)
            if i + 1 < len(els):
                return els[i + 1]
        return default

    def _int_arg(flag, default=0):
        """Return integer value after flag, or default."""
        v = _arg(flag)
        if v is None: return default
        try: return int(v)
        except ValueError: return default

    if "--rebuild" in els:
        do_freeze()
        interp, symbols = boot()
        kc = symbols.get("K_CURSOR", 0)
        cur_lux = interp.aether.aether[kc] if kc else 0
        used = interp.aether.aether[cur_lux] if cur_lux else 0
        print(f"Rebuilt: {used} luces used", file=sys.stderr)
        sys.exit(0)

    # ── --combine: inject reca_init + patch P0_NID into raw IR from stdin ────
    # Usage: ./reca_binary | python3 repl.py --combine > combined.ll
    # This is the binary→binary self-hosting step, replacing combine.py.
    if "--combine" in els:
        import re as _re
        raw_ir = sys.stdin.read()
        _, symbols = boot()
        p0_nid = symbols.get("P0_NID", 0)
        combined = _inject_reca_init(raw_ir)
        if combined == raw_ir:
            print("ERROR: reca_init.ll not found or @main() missing", file=sys.stderr)
            sys.exit(1)
        if p0_nid:
            combined = _re.sub(
                r'(call void @reca_init\(\)\n\s*br label %L)\d+',
                lambda m: m.group(1) + str(p0_nid),
                combined, count=1
            )
        sys.stdout.write(combined)
        sys.exit(0)

    # ── --self-compile: run the Reca compiler on the loaded Aether graph ──────
    if "--self-compile" in els:
        prog = 5_000_000
        prog = _int_arg("--progress", prog)
        interp, symbols = boot()
        # Compiler entry is always P0_NID (Entry→SelfYaku, becomes @main in IR).
        entry = symbols.get("P0_NID", 0) or _find_entry(interp)
        if not entry:
            print("Error: no compiler entry point (P0_NID)", file=sys.stderr)
            sys.exit(1)
        sym = {v: k for k, v in symbols.items()}
        kc = symbols.get("K_CURSOR", 0)
        cur_lux = interp.aether.aether[kc] if kc else 0
        used = interp.aether.aether[cur_lux] if cur_lux else 0
        print(f"Aether: {used} luces used  entry: {sym.get(entry, entry)}",
              file=sys.stderr)
        captured = [0]
        ver_path = os.path.join(_HERE, 'version')
        ver = open(ver_path).read().strip() if os.path.exists(ver_path) else '0'
        ir = _capture_fd1(lambda: captured.__setitem__(
            0, interp.execute_aether(entry, progress_every=prog, sym=sym)))
        ir = ir.replace('__VER__', ver)
        ir = _inject_reca_init(ir)
        sys.stdout.write(ir)
        sys.stdout.flush()
        print(f"Done: {captured[0]:,} steps  {len(ir)} bytes", file=sys.stderr)
        sys.exit(0)

    # ── --run filepath [--entry NAME]: load .re via Python, execute, exit ────
    if "--run" in els:
        from loader import fresh_loader, _BIN, _SYM, freeze as _freeze, load_symbols as _ls

        run_path = _arg("--run")
        if not run_path:
            print("Error: --run requires a filepath", file=sys.stderr)
            sys.exit(1)
        if not os.path.exists(run_path):
            print(f"Error: '{run_path}' not found", file=sys.stderr)
            sys.exit(1)

        entry_name = _arg("--entry", "MAIN")

        if not (os.path.exists(_BIN) and os.path.exists(_SYM)):
            _freeze()

        ldr = fresh_loader()
        ldr.load_file(os.path.abspath(run_path))

        entry = ldr.symbols.get(entry_name, 0)
        if not entry:
            base_syms = set(_ls().keys())
            available = [k for k in ldr.symbols if k not in base_syms][:10]
            print(f"Error: entry point '{entry_name}' not found.", file=sys.stderr)
            if available:
                print(f"  Defined in file: {available}", file=sys.stderr)
            sys.exit(1)

        max_steps = _int_arg("--steps", 10_000_000)

        steps = ldr.interp.execute_aether(entry, max_steps=max_steps)
        if steps >= max_steps:
            print(f"Stopped: step limit reached ({max_steps:,}).", file=sys.stderr)
        else:
            print(f"Done. ({steps} steps)", file=sys.stderr)
        sys.exit(0)


    interp, symbols = boot()

    # No flags → interactive REPL: read symbol names from stdin, execute each.
    if not els or (len(els) == 1 and els[0] in ("--rebuild",)):
        sym_by_addr = {v: k for k, v in symbols.items()}
        print(f"Reca REPL — {len(symbols)} symbols loaded. Type a symbol name to run it, Ctrl+D to exit.")
        while True:
            try:
                line = input("> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if not line:
                continue
            addr = symbols.get(line, 0)
            if not addr:
                # fuzzy: show close matches
                matches = [k for k in symbols if k.startswith(line)][:5]
                if matches:
                    print(f"  Unknown symbol. Did you mean: {', '.join(matches)}")
                else:
                    print(f"  Unknown symbol '{line}'.")
                continue
            steps = interp.execute_aether(addr, max_steps=10_000_000)
            print(f"Done. ({steps} steps)")
        sys.exit(0)

    # --load filepath: redirect fd 0 to the file before running PS_MAIN.
    # Python only opens the fd — content is read and parsed by parser.re.
    if "--load" in els:
        load_path = _arg("--load")
        if not load_path:
            print("Error: --load requires a filepath", file=sys.stderr)
            sys.exit(1)
        _redirect_stdin_to(load_path)

    # --entry name: jump to a named Lux instead of PS_MAIN.
    entry_name = _arg("--entry")

    entry = (_find_entry(interp, entry_name) if entry_name
             else interp.R.get("PS_MAIN", 0) or _find_entry(interp))

    if not entry:
        print("Error: no entry point found", file=sys.stderr)
        sys.exit(1)

    # Execute — Reca handles everything from here.
    interp.execute_aether(entry)
