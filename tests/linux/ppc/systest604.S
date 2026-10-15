# Supervisor level tests of the 604/604e emulation: the hardware search of
# the hashed page table (primary and secondary PTE groups, referenced and
# changed bits, DSI/ISI when nothing matches, protection), tlbie, BAT
# precedence, and what the 604 lacks or adds compared with the 603 (no
# TGPR, tlbld or miss SPRs, no doze; PIR, PMCs, HID1, MSR[PM]).
#
# Loaded at physical address 0 and started at 0x4000 in real mode. Every
# CHECK records { expected, actual, name } at r29; the program ends with
# 0xd0d0d0d0 at DONE and loops. tests/linux/cpu-unit-tests.cpp runs it on
# a 604e (HID1 = 0x40000000) and evaluates the records.

	.set LOG_VEC,	0x3f00
	.set LOG_SRR0,	0x3f04
	.set LOG_SRR1,	0x3f08
	.set LOG_DAR,	0x3f0c
	.set LOG_DSISR,	0x3f10
	.set COUNTS,	0x3e00		# one word per vector (vector >> 6)
	.set DONE,	0x3ffc
	.set CHECKS,	0x6000		# check records
	.set HTAB,	0x00010000	# 64 KB hashed page table (HTABMASK 0)
	.set VSID,	0x123
	.set SR2_KS0,	0x20000000 | VSID	# Kp = 1, Ks = 0
	.set SR2_KS1,	0x60000000 | VSID	# Kp = 1, Ks = 1

	.set SPRG0, 272
	.set SPRG1, 273
	.set SPRG2, 274
	.set HID0, 1008
	.set HID1, 1009
	.set PMC1, 953
	.set PIR, 1023
	.set DMISS, 976

	# PTE helpers: the group address of an effective address in segment
	# 2 (primary or secondary hash) and the first word of its PTE.
	.macro PTEG name, ea, secondary
	.if \secondary
	.set \name, HTAB + ((~(VSID ^ ((\ea >> 12) & 0xffff)) & 0x3ff) << 6)
	.else
	.set \name, HTAB + (((VSID ^ ((\ea >> 12) & 0xffff)) & 0x3ff) << 6)
	.endif
	.endm

	.macro PTE0 name, ea, secondary
	.set \name, 0x80000000 | (VSID << 7) | ((\secondary) << 6) | ((\ea >> 22) & 0x3f)
	.endm

	.macro CHECK reg, value, name
	lis	%r30, (\value)@h
	ori	%r30, %r30, (\value)@l
	stw	%r30, 0(%r29)
	stw	\reg, 4(%r29)
	lis	%r30, 98f@ha
	addi	%r30, %r30, 98f@l
	stw	%r30, 8(%r29)
	addi	%r29, %r29, 12
	.pushsection .rodata
98:	.string "\name"
	.popsection
	.endm

	.macro LOAD reg, value
	lis	\reg, (\value)@h
	ori	\reg, \reg, (\value)@l
	.endm

	# Stores a PTE: word 0 and word 1 at group + 8 * slot.
	.macro SETPTE group, slot, word0, word1
	LOAD	%r10, \group + 8 * \slot
	LOAD	%r11, \word0
	LOAD	%r12, \word1
	stw	%r11, 0(%r10)
	stw	%r12, 4(%r10)
	.endm

	# Generic handler: save r3/r4, log, count, then continue at 'next'.
	.macro VECTOR vec, next
	.org \vec
	mtspr	SPRG0, %r3
	mtspr	SPRG1, %r4
	li	%r3, \vec
	b	\next
	.endm

	.section .text
	.globl _vectors
_vectors:
	VECTOR 0x100, log_skip
	VECTOR 0x200, log_skip
	VECTOR 0x300, log_skip
	VECTOR 0x400, log_return_lr
	VECTOR 0x500, log_retry
	VECTOR 0x600, log_skip
	VECTOR 0x700, log_skip
	VECTOR 0x800, log_enable_fp
	VECTOR 0x900, log_decrementer
	VECTOR 0xc00, log_skip
	VECTOR 0xd00, log_skip
	VECTOR 0xf00, log_skip
	VECTOR 0x1000, log_skip
	VECTOR 0x1100, log_skip
	VECTOR 0x1200, log_skip
	VECTOR 0x1300, log_skip
	VECTOR 0x1400, log_retry

	.org 0x1500
	# ------------------------------------------------- handler tails
	# r3 = vector; r3 and r4 are saved in SPRG0/SPRG1.
log_common:
	stw	%r3, LOG_VEC(0)
	mfsrr0	%r4
	stw	%r4, LOG_SRR0(0)
	mfsrr1	%r4
	stw	%r4, LOG_SRR1(0)
	mfdar	%r4
	stw	%r4, LOG_DAR(0)
	mfdsisr	%r4
	stw	%r4, LOG_DSISR(0)
	srwi	%r3, %r3, 6
	lwz	%r4, COUNTS(%r3)
	addi	%r4, %r4, 1
	stw	%r4, COUNTS(%r3)
	blr

log_skip:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	mfsrr0	%r4
	addi	%r4, %r4, 4
	mtsrr0	%r4
	b	restore

log_retry:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	b	restore

log_return_lr:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	mflr	%r4
	mtsrr0	%r4
	b	restore

log_enable_fp:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	mfsrr1	%r4
	ori	%r4, %r4, 0x2000	# MSR[FP]
	mtsrr1	%r4
	b	restore

log_decrementer:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	LOAD	%r4, 0x7fffffff
	mtdec	%r4
	b	restore

restore:
	mfspr	%r4, SPRG1
	mfspr	%r3, SPRG0
	rfi

	# Clears the logged vector (so a test can see that none occurred).
clear_log:
	li	%r4, 0
	stw	%r4, LOG_VEC(0)
	stw	%r4, LOG_DSISR(0)
	stw	%r4, LOG_SRR1(0)
	blr

	# The code copied into a page mapped through the page table.
mapped_code:
	li	%r3, 0x77
	blr
mapped_code_end:

	# =============================================================== tests
	.org 0x4000
	.globl _start
_start:
	LOAD	%r29, CHECKS
	li	%r0, 0
	# clear the logs
	li	%r3, 0
	li	%r4, 0x3e00
1:	stw	%r3, 0(%r4)
	addi	%r4, %r4, 4
	cmpwi	%r4, 0x4000
	blt	1b
	# clear the page table
	LOAD	%r4, HTAB
	LOAD	%r5, HTAB + 0x10000
2:	stw	%r3, 0(%r4)
	addi	%r4, %r4, 4
	cmpw	%r4, %r5
	blt	2b

	# Memory: segment 2 through the page table; the low 16 MB 1:1 through
	# IBAT0/DBAT0 (supervisor, read/write, coherent).
	LOAD	%r5, HTAB
	mtsdr1	%r5
	LOAD	%r5, SR2_KS0
	mtsr	2, %r5
	LOAD	%r5, 0x000001fe		# BEPI 0, 16 MB, Vs
	LOAD	%r6, 0x00000012		# BRPN 0, M, PP 2
	mtspr	528, %r5		# IBAT0U
	mtspr	529, %r6		# IBAT0L
	mtspr	536, %r5		# DBAT0U
	mtspr	537, %r6		# DBAT0L

	# Page A: EA 0x20003000 -> 0x00203000, read/write, primary group.
	PTEG	PTEG_A, 0x20003000, 0
	PTE0	PTE0_A, 0x20003000, 0
	SETPTE	PTEG_A, 3, PTE0_A, 0x00203012
	LOAD	%r5, 0x00203010
	LOAD	%r6, 0xcafe0001
	stw	%r6, 0(%r5)
	# Page B: EA 0x20005000 -> 0x00205000, read/write, secondary group.
	PTEG	PTEG_B, 0x20005000, 1
	PTE0	PTE0_B, 0x20005000, 1
	SETPTE	PTEG_B, 7, PTE0_B, 0x00205012
	LOAD	%r5, 0x00205020
	LOAD	%r6, 0xbeef0002
	stw	%r6, 0(%r5)
	# Page C: EA 0x20009000 -> 0x00209000, read only (PP 3).
	PTEG	PTEG_C, 0x20009000, 0
	PTE0	PTE0_C, 0x20009000, 0
	SETPTE	PTEG_C, 0, PTE0_C, 0x00209013
	# Page D: EA 0x2000b000 -> 0x0020b000, PP 0 (no access with key 1).
	PTEG	PTEG_D, 0x2000b000, 0
	PTE0	PTE0_D, 0x2000b000, 0
	SETPTE	PTEG_D, 1, PTE0_D, 0x0020b010
	# Page F: EA 0x2000f000 -> 0x0020f000, code.
	PTEG	PTEG_F, 0x2000f000, 0
	PTE0	PTE0_F, 0x2000f000, 0
	SETPTE	PTEG_F, 5, PTE0_F, 0x0020f012
	LOAD	%r5, mapped_code
	LOAD	%r6, 0x0020f000
	lwz	%r7, 0(%r5)
	stw	%r7, 0(%r6)
	lwz	%r7, 4(%r5)
	stw	%r7, 4(%r6)
	# a decoy in the primary group of page B with H = 0: must not match
	PTEG	PTEG_B0, 0x20005000, 0
	PTE0	PTE0_B0, 0x20005000, 0
	SETPTE	PTEG_B0, 2, PTE0_B0 | 0x40, 0x00206012

	# data translation on
	mfmsr	%r5
	ori	%r5, %r5, 0x10		# DR
	mtmsr	%r5
	isync

	# ---------------------------------------------------- primary group
	LOAD	%r5, 0x20003010
	lwz	%r6, 0(%r5)
	CHECK	%r6, 0xcafe0001, "read through the primary PTEG"
	LOAD	%r3, PTEG_A + 3 * 8 + 4
	lwz	%r6, 0(%r3)
	andi.	%r7, %r6, 0x180
	CHECK	%r7, 0x100, "read sets R, not C"
	LOAD	%r6, 0x12345678
	stw	%r6, 0(%r5)
	lwz	%r6, 0(%r3)
	andi.	%r7, %r6, 0x180
	CHECK	%r7, 0x180, "store sets C"
	LOAD	%r4, 0x00203010
	lwz	%r6, 0(%r4)
	CHECK	%r6, 0x12345678, "store reached the physical page"

	# ---------------------------------------------------- secondary group
	LOAD	%r5, 0x20005020
	lwz	%r6, 0(%r5)
	CHECK	%r6, 0xbeef0002, "read through the secondary PTEG"
	LOAD	%r3, PTEG_B + 7 * 8 + 4
	lwz	%r6, 0(%r3)
	andi.	%r7, %r6, 0x100
	CHECK	%r7, 0x100, "secondary PTE referenced"
	LOAD	%r3, PTEG_B0 + 2 * 8 + 4
	lwz	%r6, 0(%r3)
	andi.	%r7, %r6, 0x180
	CHECK	%r7, 0, "the H=1 decoy in the primary group is untouched"

	# ---------------------------------------------------- page faults
	bl	clear_log
	LOAD	%r5, 0x20007000
	lwz	%r6, 0(%r5)
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0x300, "load fault: DSI"
	lwz	%r4, LOG_DSISR(0)
	CHECK	%r4, 0x40000000, "load fault: DSISR not found"
	lwz	%r4, LOG_DAR(0)
	CHECK	%r4, 0x20007000, "load fault: DAR"
	bl	clear_log
	stw	%r6, 4(%r5)
	lwz	%r4, LOG_DSISR(0)
	CHECK	%r4, 0x42000000, "store fault: DSISR not found, store"
	lwz	%r4, LOG_DAR(0)
	CHECK	%r4, 0x20007004, "store fault: DAR"
	lwz	%r4, COUNTS + (0x1100 >> 6)(0)
	CHECK	%r4, 0, "no 603 TLB miss vectors"

	# ---------------------------------------------------- protection
	LOAD	%r5, 0x20009000
	lwz	%r6, 0(%r5)		# allowed
	bl	clear_log
	stw	%r6, 0(%r5)		# read only
	lwz	%r4, LOG_DSISR(0)
	CHECK	%r4, 0x0a000000, "store to read only page: DSISR"
	LOAD	%r3, PTEG_C + 4
	lwz	%r6, 0(%r3)
	andi.	%r7, %r6, 0x180
	CHECK	%r7, 0x100, "denied store sets R but not C"
	# key 1 (Ks = 1) with PP 0: no access at all
	LOAD	%r6, SR2_KS1
	mtsr	2, %r6
	isync
	bl	clear_log
	LOAD	%r5, 0x2000b000
	lwz	%r6, 0(%r5)
	lwz	%r4, LOG_DSISR(0)
	CHECK	%r4, 0x08000000, "key 1, PP 0: load denied"
	LOAD	%r6, SR2_KS0
	mtsr	2, %r6
	isync
	bl	clear_log
	lwz	%r6, 0(%r5)
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0, "key 0, PP 0: load allowed"

	# ---------------------------------------------------- tlbie
	# Page A now points at 0x00204000 (holding a different value).
	LOAD	%r4, 0x00204010
	LOAD	%r6, 0x0000abcd
	stw	%r6, 0(%r4)
	LOAD	%r3, PTEG_A + 3 * 8 + 4
	LOAD	%r6, 0x00204012
	stw	%r6, 0(%r3)
	sync
	LOAD	%r5, 0x20003010
	tlbie	%r5
	sync
	lwz	%r6, 0(%r5)
	CHECK	%r6, 0x0000abcd, "after tlbie the new PTE is used"

	# ---------------------------------------------------- BAT precedence
	# DBAT1: EA 0x20000000 (128 KB) -> 0x00300000.
	LOAD	%r4, 0x00303010
	LOAD	%r6, 0x00ba7000
	stw	%r6, 0(%r4)
	LOAD	%r5, 0x20000002		# BEPI 0x20000000, 128 KB, Vs
	LOAD	%r6, 0x00300012
	mtspr	538, %r5		# DBAT1U
	mtspr	539, %r6		# DBAT1L
	isync
	LOAD	%r5, 0x20003010
	lwz	%r6, 0(%r5)
	CHECK	%r6, 0x00ba7000, "a BAT wins over the page table"
	li	%r5, 0
	mtspr	538, %r5
	isync

	# ---------------------------------------------------- instruction side
	mfmsr	%r5
	ori	%r5, %r5, 0x20		# IR (low memory through IBAT0)
	mtmsr	%r5
	isync
	li	%r3, 0
	LOAD	%r5, 0x2000f000
	mtctr	%r5
	bctrl
	CHECK	%r3, 0x77, "code fetched through the page table"
	LOAD	%r3, PTEG_F + 5 * 8 + 4
	lwz	%r6, 0(%r3)
	andi.	%r7, %r6, 0x180
	CHECK	%r7, 0x100, "fetch sets R"
	bl	clear_log
	LOAD	%r5, 0x2000d000
	mtctr	%r5
	bctrl				# ISI returns to LR
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0x400, "fetch fault: ISI"
	lwz	%r4, LOG_SRR1(0)
	rlwinm	%r4, %r4, 0, 0, 15
	CHECK	%r4, 0x40000000, "fetch fault: SRR1 not found"
	lwz	%r4, LOG_SRR0(0)
	CHECK	%r4, 0x2000d000, "fetch fault: SRR0"
	mfmsr	%r5
	rlwinm	%r5, %r5, 0, 27, 25	# clear IR
	mtmsr	%r5
	isync

	# ---------------------------------------------------- 603 features
	li	%r0, 0x5a
	mfmsr	%r5
	oris	%r5, %r5, 0x0002	# MSR[TGPR]
	mtmsr	%r5
	mfmsr	%r4
	andis.	%r4, %r4, 0x0002
	CHECK	%r4, 0, "no MSR[TGPR] on the 604"
	CHECK	%r0, 0x5a, "GPR0 not swapped"
	bl	clear_log
	tlbld	%r5
	lwz	%r4, LOG_SRR1(0)
	rlwinm	%r4, %r4, 0, 0, 15
	CHECK	%r4, 0x00080000, "tlbld illegal"
	bl	clear_log
	mfspr	%r4, DMISS
	lwz	%r4, LOG_SRR1(0)
	rlwinm	%r4, %r4, 0, 0, 15
	CHECK	%r4, 0x00080000, "DMISS illegal"
	# MSR[POW] with HID0[DOZE] does not stop the 604
	LOAD	%r5, 0x00800000
	mtspr	HID0, %r5
	li	%r4, 0
	mfmsr	%r5
	oris	%r5, %r5, 0x0004	# POW
	sync
	mtmsr	%r5
	isync
	li	%r4, 1
	CHECK	%r4, 1, "MSR[POW] does not doze"
	mfmsr	%r5
	rlwinm	%r5, %r5, 0, 14, 12	# clear POW
	mtmsr	%r5

	# ---------------------------------------------------- 604 registers
	mfspr	%r4, PIR
	CHECK	%r4, 0, "PIR is the processor number"
	LOAD	%r5, 0x00001234
	mtspr	PMC1, %r5
	mfspr	%r4, PMC1
	CHECK	%r4, 0x1234, "PMC1"
	mfspr	%r4, HID1
	CHECK	%r4, 0x40000000, "HID1 PLL configuration"
	LOAD	%r5, 0x8000cc84
	mtspr	HID0, %r5
	mfspr	%r4, HID0
	CHECK	%r4, 0x8000c084, "HID0 ICIA/DCIA do not stick"
	# rfi restores MSR[PM]
	mfmsr	%r5
	ori	%r5, %r5, 0x4		# PM
	mtsrr1	%r5
	LOAD	%r5, 3f
	mtsrr0	%r5
	rfi
3:	mfmsr	%r4
	andi.	%r4, %r4, 0x4
	CHECK	%r4, 4, "rfi restores MSR[PM]"

	# ---------------------------------------------------- done
	stw	%r29, 0x3ff8(0)
	LOAD	%r4, 0xd0d0d0d0
	stw	%r4, DONE(0)
15:	b	15b
