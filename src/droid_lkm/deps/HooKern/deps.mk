# HooKern, a hook library whose parts a consumer compiles in by choice.
#
# DEPS_LIB_OBJS below is everything, which is what a consumer that wants the whole
# library links. a consumer that wants less links the core plus the groups it uses,
# and passes -DHK_NO_<GROUP> for every group it leaves out, which turns the calls
# the core makes into them into nothing. the groups are:
#
#   core            lib/hk.o lib/hk_ksym.o
#   patch           lib/hk_patch.o lib/hk_flush.o
#   kprobe          lib/hk_kprobe.o
#   kretprobe       lib/hk_kretprobe.o
#   inline          lib/hk_inline.o and patch
#   ptr             lib/hk_ptr.o and patch
#   cfi             lib/hk_cfi.o and patch
#   lsm             lib/hk_lsm.o and patch
#   sighook         lib/hk_sighook.o and kprobe
#   binder          lib/hk_binder.o and inline
#   redir           lib/hk_redir.o, kprobe and kretprobe, and inline with patch for
#                   the text route, which a build without inline does not have
#
# every group has a flag: HK_NO_PATCH HK_NO_KPROBE HK_NO_KRETPROBE HK_NO_INLINE
# HK_NO_PTR HK_NO_SIGHOOK HK_NO_REDIR. cfi, lsm and binder are not called by the
# core at all, so they need no flag: leaving their object out is enough. the flags
# only remove the calls the core makes, never an entry point of the group itself:
# calling one of those without its object is a link error and not a silent no op.
#
# a kernelcall sized consumer that wants the kprobe route, the kretprobe pairing and
# the table writes links core, patch, kprobe, kretprobe and redir with
# -DHK_NO_INLINE -DHK_NO_PTR -DHK_NO_SIGHOOK, which measured 19 percent smaller
# than the same consumer with the whole library in it.
DEPS_LIB_NAME := HooKern
DEPS_LIB_OBJS := lib/hk.o lib/hk_ksym.o lib/hk_patch.o lib/hk_flush.o lib/hk_ptr.o lib/hk_inline.o lib/hk_kprobe.o lib/hk_redir.o lib/hk_kretprobe.o lib/hk_sighook.o lib/hk_cfi.o lib/hk_binder.o lib/hk_lsm.o
DEPS_LIB_INCS := lib
DEPS_LIB_DEPS := KallRecon
