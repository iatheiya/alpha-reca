//scheduler.re — Minimal thread scheduler for flux fan-out

ROLE: Reads flux with multiple Next slots and launches each as an
OS thread via Exire(clone). This is the Aria layer over flux
parallelism — the kernel only stores N next addresses; this Aria
decides what to do with them.

DESIGN:
  Flux with multiple Next slots = fork point.
  SCHED_FORK: reads all Next addresses from flux, launches N-1
  threads via clone (current thread takes the first Next).

  Stack allocation: each new thread needs its own stack.
  Currently: allocates a fixed-size stack from flux zone.
  Thread stacks do NOT share Aether — each gets its own stack pointer.
  Aether itself IS shared (same address space via CLONE_VM).

  Synchronisation: caller's responsibility (futex-based mutex/barrier).
  This scheduler only handles fork — join/barrier is separate.

LIMITATION (current):
  Fixed stack size (SCHED_STACK_SIZE).
  No thread join / barrier (use futex manually for now).
  No thread-local storage.

DEPENDENCY: aspects.re  constants.re  registers.re  alloc.re  flux.re
            linux_generic.re  callstack.re//

── Constants ──────────────────────────────────────────────────────────────────
NEWSET SCHED_STACK_SIZE 4096   /luces per thread stack (32KB at 8 bytes/lux)

── Registers ──────────────────────────────────────────────────────────────────
NEW RA_SCHED_LUX    /IN: flux to fork from
NEW RA_SCHED_IDX    /current Next slot index during fork
NEW RA_SCHED_NEXT   /current Next address being dispatched
NEW RA_SCHED_TID    /return value from clone (0=child, >0=parent with child tid)
NEW RA_SCHED_STACK  /allocated stack base for new thread

── SCHED_FORK: launch all Next slots of flux as parallel threads ─────────
//IN:  RA_SCHED_LUX = flux with multiple Next slots
Reads each Next slot via FLUX_FIND_SLOT (FLUX_SLOT_NEXT).
For each Next after the first: clone a new thread starting there.
Current thread continues with the FIRST Next.
Non-leaf.//
NOLINK
    /Read type lux from flux word
    ITO SCHED_FORK   Move  El1=RA_SCHED_LUX  Exit=RA_FX_SRC
    RVOCA SF_RDTYPE    FLUX_READ_TYPE
    /Count Next slots
    RVOCA SF_COUNT     FLUX_COUNT_NEXT
    /If only 0 or 1 Next → nothing to fork, return 0
    JZ SF_CNT0 RA_FX_COUNT SF_SKIP
    JEQ SF_CNT1 RA_FX_COUNT C_1 SF_SKIP
    /Find first Next slot
    ITO SF_FND_INIT  Move  El1=FLUX_SLOT_NEXT Exit=RA_FX_FIND
    RVOCA SF_FIND1     FLUX_FIND_SLOT
    /RA_FX_RESULT = first Next addr → current thread will go there
    /Store it for return
    ITO SF_SAVEF     Move  El1=RA_FX_RESULT   Exit=RA_SCHED_NEXT
    /Fork remaining Next slots as new threads
    /For simplicity: scan type lux manually for all Next codes
    CLEAR SF_TIDX RA_SCHED_IDX                                   /track which Next we are on
    ITO SF_TPOS_I    Add   El1=RA_FX_TYPE     El2=C_1           Exit=RA_FX_TYPE_POS
    ITO SF_HPOS_I    Add   El1=RA_SCHED_LUX   El2=C_1           Exit=RA_FX_POS
    NOLINK
    /Scan type lux for FLUX_SLOT_NEXT entries
    ITO SF_SCAN      Read  El1=RA_FX_TYPE_POS Exit=RA_FX_CODE
    JZ SF_SCANZ RA_FX_CODE SF_DONE
    ITO SF_HVAL      Read  El1=RA_FX_POS      Exit=RA_FX_VAL
    JEQ SF_NXTCK RA_FX_CODE FLUX_SLOT_NEXT SF_IS_NEXT
    ITO SF_ADVT      Add   El1=RA_FX_TYPE_POS El2=C_1 Exit=RA_FX_TYPE_POS
    ITO SF_ADVH      Add   El1=RA_FX_POS      El2=C_1 Exit=RA_FX_POS
    ITO SF_LB        Jump  Exit=SF_SCAN
    NOLINK
    /This slot is a Next
    ITO SF_IS_NEXT   Add   El1=RA_SCHED_IDX   El2=C_1  Exit=RA_SCHED_IDX
    /First Next (idx==1 after increment): current thread takes it, skip fork
    JEQ SF_IDX1CK RA_SCHED_IDX C_1 SF_NEXT_ADV
    /Other Nexts: fork new thread
    ITO SF_PC_SAVE   Move  El1=RA_FX_VAL       Exit=RA_SCHED_NEXT
    /Allocate stack for new thread (from flux zone)
    ITO SF_STK_CNT   Move  El1=SCHED_STACK_SIZE Exit=RA_ALLOC_COUNT
    RVOCA SF_STK_ALC   ALLOC_FLUX_ZONE
    /Stack grows down: pass top of allocated region as stack ptr
    ITO SF_STK_TOP   Add   El1=RA_ALLOC_RESULT El2=SCHED_STACK_SIZE Exit=RA_SCHED_STACK
    /clone(CLONE_THREAD_FLAGS, stack_top, 0, 0, 0)
    ITO SF_CL_NR     Move  El1=SYS_CLONE       Exit=SC_NR
    ITO SF_CL_A0     Move  El1=CLONE_THREAD_FLAGS Exit=SC_A0
    ITO SF_CL_A1     Move  El1=RA_SCHED_STACK  Exit=SC_A1
    CLEAR SF_CL_A2 SC_A2
    CLEAR SF_CL_A3 SC_A3
    ITO SF_CL_XR     Exire El1=C_0 El2=C_0 Exit=C_0
    /SC_A0 = tid in parent (>0), 0 in child
    ITO SF_TID_RD    Move  El1=SC_A0            Exit=RA_SCHED_TID
    /Child: jump to the forked Next address immediately
    JZ SF_TIDJZ RA_SCHED_TID SF_CHILD_GO
    /Parent: continue scanning for more Next slots
    ITO SF_NEXT_ADV  Add   El1=RA_FX_TYPE_POS  El2=C_1 Exit=RA_FX_TYPE_POS
    ITO SF_NEXT_ADV2 Add   El1=RA_FX_POS       El2=C_1 Exit=RA_FX_POS
    ITO SF_SCAN_LB   Jump  Exit=SF_SCAN
    /Child: set pc to its Next address and go
    NOLINK
    ITO SF_CHILD_GO  Redi  El1=RA_SCHED_NEXT
    /Done scanning: current thread returns to first Next
    NOLINK
    RREDI SF_DONE
    /No fork needed (0 or 1 Next)
    NOLINK
    RREDI SF_SKIP