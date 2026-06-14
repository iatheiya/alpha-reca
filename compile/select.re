/compile/select.re — Select: branchless conditional choice (opt layer)/

/LAYER: opt/lowering — NOT aspects.re/

/WHY NOT NATIVE:/
/Select is not universally available as a single hardware instruction:/
/ARM64: CSEL exists (TGTCAP_SELECT=1 in target/aarch64.re)/
/x86-64: CMOV exists/
/WASM: select exists/
/RISC-V base: NO native equivalent; Zicond extension adds czero.eqz/czero.nez/
/Since RISC-V base ISA requires a branch sequence, Select fails the/
/universality test for aspects.re. It belongs here, in the opt layer./

/WHAT IT MEANS:/
/Select(cond, a, b) → if cond != 0 then a else b/
/Returns one of two values based on a condition, WITHOUT branching./
/On TGTCAP_SELECT=1 targets: lowers to CSEL/CMOV/select./
/On TGTCAP_SELECT=0 targets: lowers to conditional branch sequence./

/HOW THE COMPILER USES IT:/
/yaku.re checks TGTCAP_SELECT before emitting a Select Lux:/
/if TGTCAP_SELECT == 1: emit "select i{XLEN} icmp..."/
/if TGTCAP_SELECT == 0: emit JumpIf-based branch sequence/
/Rule_Select in yaku.re implements this conditional lowering./

/USE CASES (all expressible via JumpIf, but faster with Select):/
/min(a, b)    = Select(Less(a, b), a, b)/
/max(a, b)    = Select(Less(b, a), a, b)/
/abs(a)       = Select(Less(a, C_0), Sub(C_0, a), a)/
/ternary      = Select(cond, val_true, val_false)/
/clamp(v,l,h) = Select(Less(v,l), l, Select(Less(h,v), h, v))/

/CALLING CONVENTION (DIRECT — like ops.re):/
/RA_SEL_COND.word = condition value (0 = false, nonzero = true)/
/RA_SEL_A.word    = value if condition is true/
/RA_SEL_B.word    = value if condition is false/
/RA_SEL_OUT.word  = result after return/
/RA_SEL_RET.word  = return Lux ID (set via SETREF before jump)/

/DEPENDENCY: aspects.re, core/constants.re, target/aarch64.re (TGTCAP_SELECT)/

/NOTE: currently not loaded by default; add explicit LINK or Accord to activate./
/Currently NOT loaded — add when yaku.re has Rule_Select support./

NEW Select
/Select(cond, a, b): conditional value without branching/
/In Python interpreter: Select has NO dedicated handler. The SEL_OP subroutine/
/below IS the implementation — it executes directly as Reca instructions (JumpIf)./
/In compiled binary: Rule_Select lowers based on TGTCAP_SELECT/

/── Subroutine registers ──────────────────────────────────────/
NEW RA_SEL_COND  /input: condition (0=false, nonzero=true)/
NEW RA_SEL_A     /input: value if condition is true/
NEW RA_SEL_B     /input: value if condition is false/
NEW RA_SEL_OUT   /output: selected value/
NEW RA_SEL_RET   /return Lux ID/

/── SEL_OP: software implementation (fallback for TGTCAP_SELECT=0) ───/
/cond? A : B  using JumpIf/
/On TGTCAP_SELECT=1 targets, yaku.re replaces this with CSEL/CMOV./
ITO SEL_OP   JumpIf  El1=RA_SEL_COND  Exit=SEL_TRUE
ITO SEL_B    Move   El1=RA_SEL_B     Exit=RA_SEL_OUT
ITO SEL_RB   JumpReg El1=RA_SEL_RET
ITO SEL_TRUE Move   El1=RA_SEL_A     Exit=RA_SEL_OUT
ITO SEL_RA   JumpReg El1=RA_SEL_RET
