# C++ exceptions on this toolchain: the unwinder finds frame tables only through __register_frame_info, which the PS3 start-up
# never calls (the .init code that would is not run), so the first throw aborts inside _Unwind_RaiseException.  sp_eh_init()
# registers the table itself; this object marks where it starts.  It holds no data of its own: the label is the address of the
# next object's .eh_frame entries, and the Makefile links this object first among ours, so everything from here to the end of
# the section (our code, libstdc++, libgcc) is covered.  The entries before it are the C start-up files', which never throw.
	.section .eh_frame,"a",@progbits
	.balign 4
	.globl sp_eh_anchor
sp_eh_anchor:
