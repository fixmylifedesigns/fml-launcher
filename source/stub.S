# Relocation stub. boot.c copies it to STUB_ADDR (low memory) and jumps to it
# with r3 = address of a DOL file loaded in RAM, r4 = an argv block (or 0)
# and r5 = an environment block (or 0).
# It copies every DOL section to its load address, clears BSS, hands over the
# arguments if the DOL asks for them, syncs caches and jumps to the entry point.
# Uses no stack and only relative branches, so it runs from any address.

	.text
	.globl	stub_start
	.globl	stub_end
	.balign	4

stub_start:
	mr	31, 3			# r31 = DOL header
	mr	28, 4			# r28 = argv block
	mr	27, 5			# r27 = environment block
	li	30, 0			# r30 = section index * 4
	li	29, 18			# 7 text + 11 data sections

next_section:
	add	9, 31, 30
	lwz	4, 0x00(9)		# file offset
	lwz	5, 0x48(9)		# load address
	lwz	6, 0x90(9)		# size
	cmpwi	6, 0
	beq	skip_section
	add	4, 4, 31		# source = header + offset
	mtctr	6
	addi	4, 4, -1
	addi	7, 5, -1
copy_byte:
	lbzu	0, 1(4)
	stbu	0, 1(7)
	bdnz	copy_byte
	mr	3, 5
	mr	4, 6
	bl	sync_range
skip_section:
	addi	30, 30, 4
	addic.	29, 29, -1
	bne	next_section

	lwz	5, 0xD8(31)		# bss address
	lwz	6, 0xDC(31)		# bss size
	cmpwi	6, 0
	beq	pass_args
	mtctr	6
	addi	7, 5, -1
	li	0, 0
clear_byte:
	stbu	0, 1(7)
	bdnz	clear_byte
	mr	3, 5
	mr	4, 6
	bl	sync_range

# devkitPPC/libogc programs start with "b ...; .ascii _arg" followed by a
# 24-byte argv struct that the loader fills in.
pass_args:
	cmpwi	28, 0
	beq	pass_env
	lwz	5, 0xE0(31)		# entry point
	lwz	6, 4(5)
	lis	7, 0x5f61
	ori	7, 7, 0x7267		# "_arg"
	cmpw	6, 7
	bne	pass_env
	li	8, 6
	mtctr	8
	addi	9, 28, -4
	addi	10, 5, 4
copy_word:
	lwzu	0, 4(9)
	stwu	0, 4(10)
	bdnz	copy_word
	addi	3, 5, 8
	li	4, 24
	bl	sync_range

# libogc2 programs follow it with "b ...; .ascii _env" and a second struct
# for environment variables.
pass_env:
	cmpwi	27, 0
	beq	jump_entry
	lwz	5, 0xE0(31)		# entry point
	lwz	6, 0x24(5)
	lis	7, 0x5f65
	ori	7, 7, 0x6e76		# "_env"
	cmpw	6, 7
	bne	jump_entry
	li	8, 6
	mtctr	8
	addi	9, 27, -4
	addi	10, 5, 0x24
copy_env:
	lwzu	0, 4(9)
	stwu	0, 4(10)
	bdnz	copy_env
	addi	3, 5, 0x28
	li	4, 24
	bl	sync_range

jump_entry:
	lwz	3, 0xE0(31)		# entry point
	mtctr	3
	bctr

# r3 = address, r4 = length: write back data cache, invalidate instruction cache
sync_range:
	rlwinm	5, 3, 0, 0, 26		# align down to 32 bytes
	add	4, 4, 3
sync_line:
	dcbst	0, 5
	sync
	icbi	0, 5
	addi	5, 5, 32
	cmplw	5, 4
	blt	sync_line
	sync
	isync
	blr

stub_end:
