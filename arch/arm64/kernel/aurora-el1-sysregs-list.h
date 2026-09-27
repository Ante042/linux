/* SPDX-License-Identifier: GPL-2.0-only */
static const u32 probe_opcodes[] = {
	0xd5384240, /* CurrentEL */
	0x00000000, /* UNDEFINED_INSTRUCTION */
	0xd5380000, /* MIDR_EL1 */
	0xd53800a0, /* MPIDR_EL1 */
	0xd5380420, /* ID_AA64PFR1_EL1 */
	0xd5381000, /* SCTLR_EL1 */
	0xd5382040, /* TCR_EL1 */
	0xd5382000, /* TTBR0_EL1 */
	0xd5381020, /* ACTLR_EL1 */
	0xd53be000, /* CNTFRQ_EL0 */
	0xd53be040, /* CNTVCT_EL0 */
	0xd53be320, /* CNTV_CTL_EL0 */
	0xd53be220, /* CNTP_CTL_EL0 */
	0xd539f1a0, /* AGTCNTRDIR_EL1 */
	0xd538f000, /* HID0_EL1 */
	0xd538f020, /* EHID0_EL1 */
	0xd53df500, /* CYC_OVRD_EL1 */
	0xd53ff080, /* UPMCR0_EL1 */
};
