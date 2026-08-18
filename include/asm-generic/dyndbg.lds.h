/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_GENERIC_DYNDBG_LDS_H
#define __ASM_GENERIC_DYNDBG_LDS_H

#include <asm-generic/bounded_sections.lds.h>
#define DYNDBG_SECTIONS()						\
	BOUNDED_SECTION_BY(__dyndbg_descs, ___dyndbg_descs)		\
	BOUNDED_SECTION_BY(__dyndbg_class_maps, ___dyndbg_class_maps)	\
	BOUNDED_SECTION_BY(__dyndbg_class_users, ___dyndbg_class_users)	\
	BOUNDED_SECTION_BY(__dyndbg_strings_mod, ___dyndbg_strings_mod)	\
	BOUNDED_SECTION_BY(__dyndbg_strings_file, ___dyndbg_strings_file)

#define DYNDBG_RO_SECTIONS()						\
	BOUNDED_SECTION_BY(__dyndbg_sites, ___dyndbg_sites)

#define MOD_DYNDBG_SECTIONS()						\
	__dyndbg_descs 0 : ALIGN(8) {					\
		KEEP(*(__dyndbg_descs))					\
	}								\
	__dyndbg_class_maps 0 : ALIGN(8) {				\
		KEEP(*(__dyndbg_class_maps))				\
	}								\
	__dyndbg_class_users 0 : ALIGN(8) {				\
		KEEP(*(__dyndbg_class_users))				\
	}								\
	__dyndbg_strings_mod 0 : ALIGN(8) {				\
		KEEP(*(__dyndbg_strings_mod))				\
	}								\
	__dyndbg_strings_file 0 : ALIGN(8) {				\
		KEEP(*(__dyndbg_strings_file))				\
	}

#define MOD_DYNDBG_RO_SECTIONS()					\
	.init.__dyndbg_sites 0 : ALIGN(8) {				\
		KEEP(*(__dyndbg_sites))					\
	}

#endif /* __ASM_GENERIC_DYNDBG_LDS_H */
