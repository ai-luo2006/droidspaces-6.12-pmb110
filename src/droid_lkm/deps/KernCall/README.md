# Kerncall

kernel syscall channel library for no-source kernel programming.
hijacks a free `ni_syscall` slot in `sys_call_table` and routes
userland `syscall(nr, key, cmd, ...)` calls to a dispatch callback,
enabling custom syscalls without touching kernel source. can also
patch arbitrary existing syscall slots. all kernel layout
capabilities are pointer injected (type_info + KallRecon wrappers),
the library never links a layout source. works on ARM64 GKI.

## requirements

- ARM64 device with GKI kernel
- layout injection: `resolve` for symbols (KallRecon, only
  sprint_symbol is required from the kernel) and `pgd_off` for the
  mm_struct.pgd offset (type_info BTF or anchor scan)
- consumer supplied `find_slot` is optional, a runtime table scan
  is the library default

## usage

see doc/API.md. call sc_init with a cfg once: layout injection,
dispatch callback, optional custom find_slot. discovery and patch
APIs are compiled in by default; `KERNSC_MINIMAL=1` keeps only the
custom syscall core.

## license

GPL-2.0
