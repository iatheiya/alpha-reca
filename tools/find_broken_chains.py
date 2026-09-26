#!/usr/bin/env python3
"""Static detector for the 'compute-then-fall-off-the-world' broken-chain bug.

Signature: a NOVA instruction whose `next` slot points at LUX_END (i.e. a
NOLINK terminated its chain) but whose op is NOT a legitimate terminator
(Jump / Redi / End). A computational op (Move/Read/Write/Add/Equal/CLEAR/...)
or a call (Voca) that terminates the chain means execution falls off after it
-- almost always a NOLINK placed after an instruction that should have
continued to the next (jump-target) label. Found the same bug 3x by hand
(BS_RT_COLLECT, BS_RT_QFLIP, RESERVE_COMBOS/REGION_LOOKUP); this finds them all.

Conditionals (JEQ/JZ/JumpIf) are reported separately (lower confidence): their
Exit is a real branch, but a next=LUX_END means the *fall-through* branch dies
-- usually a bug, occasionally intended, so review each.
"""
import os, sys
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, "aether.py")) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)
import loader

ldr = loader.freeze()
a = ldr.interp.aether.aether
sym = ldr.symbols

inv = {}
for n, ad in sym.items():
    inv.setdefault(ad, n)

OP, NEXT = 1, 5
lux_end = sym.get('LUX_END')

# Legitimate chain-terminators (op addresses).
term_names = ('Jump', 'Redi', 'End')
term_ops = {sym[n] for n in term_names if n in sym}
# Conditionals: Exit is a branch; next=LUX_END kills the fall-through.
cond_names = ('JumpIf', 'Equal')   # JEQ/JZ lower to JumpIf/Equal-family _J nodes
cond_ops = {sym[n] for n in cond_names if n in sym}

def opname(addr):
    return inv.get(a[addr + OP], f'op@{a[addr+OP]}')

strong, weak = [], []
for addr in sorted(ldr._nova_addrs_set):
    if a[addr + NEXT] != lux_end:
        continue
    op = a[addr + OP]
    if op in term_ops:
        continue
    name = inv.get(addr, f'@{addr}')
    (weak if op in cond_ops else strong).append((name, opname(addr), addr))

print(f"LUX_END addr = {lux_end}")
print(f"\n=== STRONG: computational/call op terminated at LUX_END (broken chain) : {len(strong)} ===")
for name, op, addr in strong:
    print(f"  {name:32} op={op:10} addr={addr}")
print(f"\n=== WEAK: conditional with next=LUX_END (fall-through dies -- review) : {len(weak)} ===")
for name, op, addr in weak:
    print(f"  {name:32} op={op:10} addr={addr}")
