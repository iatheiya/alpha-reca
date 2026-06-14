//aria/io.re — File I/O constants

Standard file descriptors, open flags, permission masks, AT_FDCWD.
Exire numbers live in runtime/registers.re — not redeclared here.

DEPENDENCY: aspects.re//

── Standard file descriptors ─────────────────────────────────
NEWSET STDIN 0

NEWSET STDOUT 1

NEWSET STDERR 2

── Open flags (ARM64 Linux) ──────────────────────────────────
NEWSET O_RDONLY 0

NEWSET O_WRONLY 1

NEWSET O_RDWR 2

NEWSET O_CREAT 64        /0100 octal

NEWSET O_TRUNC 512       /01000 octal

NEWSET O_APPEND 1024     /02000 octal

── File permission masks ─────────────────────────────────────
NEWSET PERM_RW_R_R 420   /0644: owner rw, group r, others r

NEWSET PERM_RWX_R_R 493  /0755: owner rwx, group rx, others rx

/── AT_FDCWD ─────────────────────────────────────────────────
//openat(AT_FDCWD, path, flags, mode) == open(path, flags, mode)
-100 as uint64://
NEWSET AT_FDCWD 18446744073709551516
