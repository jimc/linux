/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_GENERIC_DYNDBG_LDS_H
#define __ASM_GENERIC_DYNDBG_LDS_H

#include <asm-generic/bounded_sections.lds.h>
#define DYNDBG_SECTIONS()					\
	. = ALIGN(8);						\
	BOUNDED_SECTION_BY(__dyndbg, ___dyndbg)			\
	BOUNDED_SECTION_BY(__dyndbg_classes, ___dyndbg_classes)

#define MOD_DYNDBG_SECTIONS()                                           \
	__dyndbg : {							\
		BOUNDED_SECTION_BY(__dyndbg, ___dyndbg)			\
	}								\
	__dyndbg_classes : {						\
		BOUNDED_SECTION_BY(__dyndbg_classes, ___dyndbg_classes)	\
	}

#endif /* __ASM_GENERIC_DYNDBG_LDS_H */
