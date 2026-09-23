/*
 * Import slots for cellSysutilAvconfExt -- RESOLVED BUT NEVER CALLED.
 *
 * The 24p question comes down to one function: cellVideoOutConfigure2, the
 * only firmware entry point that could plausibly carry a refresh rate (the v1
 * configuration struct has no field for one).  Its argument layout is not
 * public -- RPCS3 stubs it as taking no arguments -- so the next step is to
 * read the real code, and the simplest way to get the decrypted 4.92 code is
 * to ask the loader where it put it.  See avconf_capture.cpp.
 *
 * This is the same loader mechanism as source/audio/audio_out_stub.S, minus
 * the trampolines: there is deliberately NO callable symbol here, so nothing
 * in the app can call a function whose ABI is a guess.  The loader fills each
 * slot with the address of the function's descriptor (OPD) when the library is
 * loaded (sysModuleLoad(0x31)), and avconf_capture.cpp only READS the slots.
 *
 * FNIDs are sha1(name + suffix), first four bytes little-endian -- the
 * algorithm that reproduces PSL1GHT's own cellVideoOutConfigure (0x0bae8772)
 * and cellVideoOutGetState (0x887572d5).  Names are those RPCS3 registers for
 * this library (Modules/cellAvconfExt.cpp), which bind real games by the same
 * hash, so a wrong name would show up as an unresolved slot, not a crash.
 *
 * ORDER MATTERS: entry N of the FNID table pairs with slot N.
 */

	.section ".rodata.sceFNID","a"
	.align 2
jf_avx_fnids:
	.long 0x9faa12be	/* 0 cellVideoOutConfigure2                 */
	.long 0x21036f4e	/* 1 cellVideoOutGetResolutionAvailability2 */
	.long 0x269ffedd	/* 2 cellVideoOutSetupDisplay               */
	.long 0xfaa275a4	/* 3 cellVideoOutGetScreenSize (known ABI: a control) */

/* Placeholder the loader overwrites.  A slot that still holds it after the
 * module loaded is an FNID the firmware does not export. */
	.section ".data.sceFStub.cellSysutilAvconfExt","aw"
	.align 2
	.globl jf_avx_stubs
jf_avx_stubs:
	.long jf_avx_unresolved
	.long jf_avx_unresolved
	.long jf_avx_unresolved
	.long jf_avx_unresolved

	.section ".data","aw"
	.align 3
	.globl jf_avx_unresolved
jf_avx_unresolved:
	.quad 0

	.section ".rodata.sceResident","a"
jf_avx_libname:
	.asciz "cellSysutilAvconfExt"

/* 44-byte import header, 32-bit pointer fields -- see audio_out_stub.S for why
 * this is written in assembly.  Header words are the ones PSL1GHT uses for
 * its sysmodule-loaded libraries (cellPngDec, cellJpgDec). */
	.section ".lib.stub","aw"
	.align 2
	.long  0x2c000001
	.short 0x0009
	.short 0		/* imports -- patched by sprxlinker */
	.long  0
	.long  0
	.long  jf_avx_libname
	.long  jf_avx_fnids
	.long  jf_avx_stubs
	.long  0
	.long  0
	.long  0
	.long  0
