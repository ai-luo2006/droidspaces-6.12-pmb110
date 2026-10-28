# type_info

runtime kernel struct layout for no-source kernel programming. parses
vmlinux BTF when present, captures module BTF automatically, and falls
back to anchor scanning on no-BTF kernels. all offsets are recovered at
runtime, no kernel source needed. works on GKI 5.10 through 6.12, with
and without BTF.

## requirements

- ARM64 device with GKI kernel
- KallRecon for symbol resolution (see kallrecon/README.md, only
  sprint_symbol is required from the kernel)
- CONFIG_DEBUG_INFO_BTF for the BTF path

## usage

see doc/API.md. call ti_init with a resolver once, then query struct
layouts, module BTF contexts, and module lists through the public API.
collaborative registration gives LKMs a layout source that does not
depend on kernel BTF.

## license

GPL-2.0
