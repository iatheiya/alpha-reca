//
target/linux_generic.re — Linux generic syscall numbers

These numbers are identical across AArch64, RISC-V 64, and all
architectures that use the Linux generic syscall table.

Source: Linux kernel include/uapi/asm-generic/unistd.h

DEPENDENCY: aspects.re  runtime/registers.re

USAGE: loaded by platform-specific files — do not load directly.
//

SET SYS_OPENAT Value=56 /openat(dirfd, path, flags, mode) → fd

SET SYS_CLOSE Value=57 /close(fd) → 0

SET SYS_READ Value=63 /read(fd, buf_start, count) → bytes_read

SET SYS_WRITE Value=64 /write(fd, buf_start, count) → bytes_written; buf: 1 byte per lux

── Python-virtual syscall ──────────────────────────────────────────────────
//
SYS_WRITE_PACKED is NOT a real Linux syscall. Syscall 103 is syslog on Linux.
This is a Python-interpreter-only hook for packed-buffer output.
When the Reca binary runs natively, packed writes must be lowered to
SYS_WRITE with a temporary byte-unpacking pass. Number 103 is safe to
use as a virtual marker because it is otherwise unused in our syscall set.
//
SET SYS_WRITE_PACKED Value=103 /VIRTUAL (Python only): write packed buffer — 8 bytes per lux, little-endian

SET SYS_EXIT Value=93 /exit_group(code) → does not return

SET SYS_FUTEX Value=98 /futex(uaddr, op, val, ...) → int

SET SYS_CLONE Value=220 /clone(flags, stack, ptid, tls, ctid) → tid in parent, 0 in child

── clone flags (common combinations) ──────────────────────────────────────
SET CLONE_VM Value=256 /0x00000100: share address space
SET CLONE_FS Value=512 /0x00000200: share filesystem
SET CLONE_FILES Value=1024 /0x00000400: share file descriptors
SET CLONE_SIGHAND Value=2048 /0x00000800: share signal handlers
SET CLONE_THREAD Value=65536 /0x00010000: same thread group
/Standard new-thread flags: VM|FS|FILES|SIGHAND|THREAD = 256+512+1024+2048+65536
SET CLONE_THREAD_FLAGS Value=69376

── futex op codes ──────────────────────────────────────────────────────────
SET FUTEX_WAIT Value=0 /wait if *uaddr == val
SET FUTEX_WAKE Value=1 /wake up to val waiters
