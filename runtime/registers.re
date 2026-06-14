// runtime/registers.re — Exire register protocol (platform-agnostic)
Defines the abstract register interface for the Exire aspect.
Python's interpreter reads these by name. The Reca compiler emits
code that loads/stores through these Lux before each Exire.

WHAT THIS IS:
  The abstract contract between Reca code and the Exire aspect.
  SC_NR holds the syscall number. SC_A0..SC_A3 hold elements.
  SC_A0 also receives the return value after Exire executes.
  
WHAT THIS IS NOT:
  This is NOT the OS calling convention (that lives in aria/syscall/linux_*.re).
  This is NOT the ABI for function calls (that lives in aria/abi/*.re).
  The names SC_NR/SC_A0..SC_A3 are canonical Reca names — they do not
  imply X0/X8 (ARM64) or a0/a7 (RISC-V). Those are lowering details.

DEPENDENCY: aspects.re

These Lux hold the syscall elements as plain word values.
Set before Exire:  Move El1=VALUE Exit=SC_NR  (writes VALUE into SC_NR.word)
Python reads:      aether[SC_NR_id]  (the word just written)
No special initialisation needed — NEW is sufficient; word starts at 0.//

NEW SC_NR   /syscall number — set before Exire instruction
NEW SC_A0   /element 0 / return value
NEW SC_A1   /element 1
NEW SC_A2   /element 2
NEW SC_A3   /element 3

/Macro element registers — used during freeze only (load-time macro dispatch).
/Python writes els here before execute_aether(MACRO_ADDR); Reca reads them.
NEW RA_MA0    /macro arg 0 (first token after command, resolved to addr)
NEW RA_MA1    /macro arg 1
NEW RA_MA2    /macro arg 2
NEW RA_MA3    /macro arg 3
NEW RA_MA4    /macro arg 4
NEW RA_MA5    /macro arg 5
NEW RA_MA6    /macro arg 6
NEW RA_MA7    /macro arg 7
NEW RA_MA_RET /macro return value (addr of newly built lux, etc.)
NEWREF RA_LINK_REF RA_LINK  /word = addr(RA_LINK); use in macros via Write El2=RA_LINK_REF
NEW RA_JEQ_FLAG  /shared flag for JEQ/JZ macros — OK since Equal→JumpIf is always sequential
SETREF RA_JEQ_FLAG RA_JEQ_FLAG  /self-ref: Equal writes to this lux, JumpIf reads it
NEW RA_JEQ_FLAG_PTR  /immutable pointer: word = addr(RA_JEQ_FLAG). Written once by loader init.
NEWREF RA_C0_REF C_0   /word = addr(C_0); use in macros via Write El2=RA_C0_REF

/── _REF symbols for macro Write El2= usage ──────────────────────────────────
/word = addr(target); macros use Write El2=X_REF to write addr into a slot

NEWREF RA_SR_LUX_REF    RA_SR_LUX
NEWREF RA_SR_REL_REF    RA_SR_REL
NEWREF RA_SR_OUT_REF    RA_SR_OUT
NEWREF RA_SR_LUMEN_REF  RA_SR_LUMEN
NEWREF RA_SR_OFFSET_REF RA_SR_OFFSET
NEWREF RA_TW_LUX_REF    RA_TW_LUX
NEWREF RA_TMP2_REF      RA_TMP2
NEWREF RA_BYTE_REF      RA_BYTE

/── Loader I/O registers (used by saku.re and macro programs) ──────────────
/These are global so SWITCH/FOR/SAVE programs can access file state.
NEW RA_LOAD_FD       /current file descriptor being loaded
NEW RA_LOAD_RPOS     /read position in load buffer
NEW RA_LOAD_RLEN     /bytes available in load buffer
NEW RA_LOAD_BYTE     /last byte read from load buffer
NEW RA_LOAD_INDENT   /current indentation level (0=none, 1=one, ...)
NEW RA_LOAD_TLEN     /token length in bytes
NEW RA_LOAD_HASH     /token hash (djb2)
