/*
 * Safe-memory trampoline for shim -> TWRP chainloading.
 *
 * The shim runs at 0x80080000, which is exactly where the TWRP kernel has to
 * be copied.  All remaining work therefore has to run from a copy outside the
 * kernel/ramdisk/dtb destination ranges.  chain_launch() copies this code and
 * its 88-byte argument block to that safe address and branches to the copy.
 */
	.section .text.chain, "ax"
	.balign	8
	.globl	chain_trampoline
chain_trampoline:
	/* x0 = argument block (also in safe memory) */
	mov	x19, x0
	ldp	x1, x2, [x19, #0]	/* kernel src, dst */
	ldp	x3, x4, [x19, #16]	/* kernel size, ramdisk src */
	ldp	x5, x6, [x19, #32]	/* ramdisk dst, size */
	ldp	x7, x8, [x19, #48]	/* dtb src, dst */
	ldp	x9, x10, [x19, #64]	/* dtb size, entry */
	ldr	x11, [x19, #80]		/* x0 for the kernel */

1:	cbz	x3, 2f
	ldr	x12, [x1], #8
	str	x12, [x2], #8
	sub	x3, x3, #8
	b	1b
2:
1:	cbz	x6, 2f
	ldr	x12, [x4], #8
	str	x12, [x5], #8
	sub	x6, x6, #8
	b	1b
2:
1:	cbz	x9, 2f
	ldr	x12, [x7], #8
	str	x12, [x8], #8
	sub	x9, x9, #8
	b	1b
2:
	mov	x0, x11
	dsb	sy
	ic	iallu
	dsb	sy
	isb
	br	x10
chain_trampoline_end:

	/* x0 = safe destination, x1 = source argument block in shim memory */
	.globl	chain_launch
chain_launch:
	mov	x25, x0
	mov	x21, x1
	adr	x22, chain_trampoline
	adr	x26, chain_trampoline_end
	sub	x26, x26, x22		/* code size in bytes */
	lsr	x23, x26, #3
	mov	x20, x25
1:	ldr	x24, [x22], #8
	str	x24, [x20], #8
	subs	x23, x23, #1
	b.ne	1b
	mov	x23, #11		/* 11 argument words = 88 bytes */
2:	ldr	x24, [x21], #8
	str	x24, [x20], #8
	subs	x23, x23, #1
	b.ne	2b
	add	x0, x25, x26		/* argument block follows the code */
	br	x25
	.section .note.GNU-stack,"",%progbits
