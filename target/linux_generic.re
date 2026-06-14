/target/linux_generic.re — Linux generic syscall numbers/

/These numbers are identical across AArch64, RISC-V 64, and all/
/architectures that use the Linux generic syscall table./

/Source: Linux kernel include/uapi/asm-generic/unistd.h/

/DEPENDENCY: aspects.re  runtime/registers.re/

/USAGE: loaded by platform-specific files — do not load directly./

NEWSET SYS_OPENAT 56      /openat(dirfd, path, flags, mode) → fd/

NEWSET SYS_CLOSE 57       /close(fd) → 0/

NEWSET SYS_READ 63        /read(fd, buf_start, count) → bytes_read/

NEWSET SYS_WRITE 64       /write(fd, buf_start, count) → bytes_written; buf: 1 byte per lux/
NEWSET SYS_WRITE_PACKED 103  /write(fd, buf_start, byte_count): buf packed 8 bytes per lux, little-endian/

NEWSET SYS_EXIT 93        /exit_group(code) → does not return/

NEWSET SYS_FUTEX 98       /futex(uaddr, op, val, ...) → int/

NEWSET SYS_CLONE 220      /clone(flags, stack, ptid, tls, ctid) → tid in parent, 0 in child/

/── clone flags (common combinations) ──────────────────────────────────────/
NEWSET CLONE_VM      256       /0x00000100: share address space/
NEWSET CLONE_FS      512       /0x00000200: share filesystem/
NEWSET CLONE_FILES   1024      /0x00000400: share file descriptors/
NEWSET CLONE_SIGHAND 2048      /0x00000800: share signal handlers/
NEWSET CLONE_THREAD  65536     /0x00010000: same thread group/
/Standard new-thread flags: VM|FS|FILES|SIGHAND|THREAD = 256+512+1024+2048+65536/
NEWSET CLONE_THREAD_FLAGS 69376

/── futex op codes ──────────────────────────────────────────────────────────/
NEWSET FUTEX_WAIT    0   /wait if *uaddr == val/
NEWSET FUTEX_WAKE    1   /wake up to val waiters/
