/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_GENERIC_MODULE_LDS_H
#define __ASM_GENERIC_MODULE_LDS_H

/*
 * <asm/module.lds.h> can specify arch-specific sections for linking modules.
 *
 * For loadable modules with CONFIG_DYNAMIC_DEBUG, we need to keep the
 * 2 __dyndbg* ELF sections, which are loaded by module.c
 *
 * Pack the 2 __dyndbg* input sections with their respective
 * .gnu.linkonce. header records into 2 output sections, with those
 * header records in the 0th element.
 */
SECTIONS {
__dyndbg_sites	: ALIGN(8) { *(.gnu.linkonce.dyndbg_site) *(__dyndbg_sites) }
__dyndbg	: ALIGN(8) { *(.gnu.linkonce.dyndbg)	  *(__dyndbg) }
}

#endif /* __ASM_GENERIC_MODULE_LDS_H */
