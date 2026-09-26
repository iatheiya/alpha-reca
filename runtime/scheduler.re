//
scheduler.re — Minimal thread scheduler for flux fan-out

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
            linux_generic.re  callstack.re
            //

── Constants ──────────────────────────────────────────────────────────────────
SET SCHED_STACK_SIZE Value=4096 /luces per thread stack (32KB at 8 bytes/lux)

── Registers ──────────────────────────────────────────────────────────────────
NEW RA_SCHED_LUX  /IN: flux to fork from
NEW RA_SCHED_IDX  /current Next slot index during fork
NEW RA_SCHED_NEXT  /current Next address being dispatched
NEW RA_SCHED_TID  /return value from clone (0=child, >0=parent with child tid)
NEW RA_SCHED_STACK  /allocated stack base for new thread

── SCHED_FORK: launch all Next slots of flux as parallel threads ─────────
//
IN:  RA_SCHED_LUX = flux with multiple Next slots
Walks every Next value via FLUX_NEXT_ITER_INIT/STEP (flux.re) -- no
separate type lux to scan since flux's descriptor lives in its own
slot 0 now; this replaced an inline manual scan that duplicated
flux.re's own traversal logic.
For each Next after the first: clone a new thread starting there.
Current thread continues with the FIRST Next.
Non-leaf.
//
NOLINK
    NOVA SCHED_FORK Op=Move El1=RA_SCHED_LUX Exit=RA_FX_SRC
    /Count Next occurrences
    RVOCA SF_COUNT Body=FLUX_COUNT_NEXT
    /If only 0 or 1 Next → nothing to fork, return 0
    JZ SF_CNT0 El1=RA_FX_COUNT Exit=SF_SKIP
    JEQ SF_CNT1 El1=RA_FX_COUNT El2=C_1 Exit=SF_SKIP
    /Start the Next iterator/
    NOVA SF_ITC Op=Move El1=FLUX_SLOT_NEXT Exit=RA_FX_ITER_CODE
    RVOCA SF_ITINIT Body=FLUX_NEXT_ITER_INIT
    CLEAR SF_TIDX El1=RA_SCHED_IDX /track which Next we are on
    /Walk every Next value/
    NOVA SF_SCAN Op=Jump Exit=SF_STEP
    NOLINK
    RVOCA SF_STEP Body=FLUX_NEXT_ITER_STEP
    JZ SF_SCANZ El1=RA_FLAG Exit=SF_DONE /iterator exhausted/
    NOVA SF_IS_NEXT Op=Add El1=RA_SCHED_IDX El2=C_1 Exit=RA_SCHED_IDX
    /First Next (idx==1 after increment): current thread takes it, skip fork/
    JEQ SF_IDX1CK El1=RA_SCHED_IDX El2=C_1 Exit=SF_FIRST
    /Other Nexts: fork new thread/
    NOVA SF_PC_SAVE Op=Move El1=RA_FX_RESULT Exit=RA_SCHED_NEXT
    /Allocate stack for new thread (from flux zone)
    NOVA SF_STK_CNT Op=Move El1=SCHED_STACK_SIZE Exit=RA_ALLOC_COUNT
    RVOCA SF_STK_ALC Body=ALLOC_FLUX_ZONE
    /Stack grows down: pass top of allocated region as stack ptr
    NOVA SF_STK_TOP Op=Add El1=RA_ALLOC_RESULT El2=SCHED_STACK_SIZE Exit=RA_SCHED_STACK
    /clone(CLONE_THREAD_FLAGS, stack_top, 0, 0, 0)
    NOVA SF_CL_NR Op=Move El1=SYS_CLONE Exit=SC_NR
    NOVA SF_CL_A0 Op=Move El1=CLONE_THREAD_FLAGS Exit=SC_A0
    NOVA SF_CL_A1 Op=Move El1=RA_SCHED_STACK Exit=SC_A1
    CLEAR SF_CL_A2 El1=SC_A2
    CLEAR SF_CL_A3 El1=SC_A3
    NOVA SF_CL_XR Op=Exire El1=C_0 El2=C_0 Exit=C_0
    /SC_A0 = tid in parent (>0), 0 in child
    NOVA SF_TID_RD Op=Move El1=SC_A0 Exit=RA_SCHED_TID
    /Child: jump to the forked Next address immediately
    JZ SF_TIDJZ El1=RA_SCHED_TID Exit=SF_CHILD_GO
    /Parent: continue scanning for more Next values
    NOVA SF_SCAN_LB Op=Jump Exit=SF_SCAN
    NOLINK
    /First Next: current thread's own return value, keep scanning for the rest/
    NOVA SF_FIRST Op=Move El1=RA_FX_RESULT Exit=RA_SCHED_NEXT
    NOVA SF_FIRST_LB Op=Jump Exit=SF_SCAN
    /Child: set pc to its Next address and go
    NOLINK
    NOVA SF_CHILD_GO Op=Redi El1=RA_SCHED_NEXT
    /Done scanning: current thread returns to first Next
    NOLINK
    RREDI SF_DONE
    /No fork needed (0 or 1 Next)
    NOLINK
    RREDI SF_SKIP
