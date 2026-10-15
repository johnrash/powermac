# The 604's broadcast tlbie on two CPUs. Both start at 0x100 with r3 = CPU
# index and map EA 0x20003000 through the page table (set up by the test
# program: HTAB at 0x10000, VSID 0x123, the PTE pointing at 0x00203000,
# which holds 1; 0x00204000 holds 2).
#  - CPU 1 reads the page over and over and stores what it saw at 0x1200;
#    once it sees 2 it sets 0x1204.
#  - CPU 0 waits until CPU 1 saw 1, points the PTE at 0x00204000 and runs
#    tlbie + tlbsync, then sets 0x1208.
# Without the broadcast CPU 1 keeps its old translation and never sees 2.
	.set PTE_WORD1, 0x00014800 + 3 * 8 + 4	# primary group of the page

	.section .text
	.org 0x100
	.globl _start
_start:
	lis	%r5, 0x0001
	mtsdr1	%r5			# HTAB 0x10000, 64 KB
	lis	%r5, 0x2000
	ori	%r5, %r5, 0x123
	mtsr	2, %r5
	li	%r5, 0x01fe		# DBAT0: 16 MB at 0, supervisor
	li	%r6, 0x0012
	mtspr	536, %r5
	mtspr	537, %r6
	mfmsr	%r5
	ori	%r5, %r5, 0x10		# DR
	mtmsr	%r5
	isync
	lis	%r7, 0x2000
	ori	%r7, %r7, 0x3000	# the page
	cmpwi	%r3, 0
	bne	reader

	# ------------------------------------------------ CPU 0: the writer
1:	lwz	%r6, 0x1200(0)
	cmpwi	%r6, 1
	bne	1b
	lis	%r8, PTE_WORD1@h
	ori	%r8, %r8, PTE_WORD1@l
	lis	%r6, 0x0020
	ori	%r6, %r6, 0x4012	# RPN 0x00204000, M, PP 2
	stw	%r6, 0(%r8)
	sync
	tlbie	%r7
	sync
	tlbsync
	sync
	li	%r6, 1
	stw	%r6, 0x1208(0)
2:	b	2b

	# ------------------------------------------------ CPU 1: the reader
reader:
	lwz	%r6, 0(%r7)
	stw	%r6, 0x1200(0)
	cmpwi	%r6, 2
	bne	reader
	li	%r6, 1
	stw	%r6, 0x1204(0)
3:	b	3b
