/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_GENERIC_DYNDBG_LDS_H
#define __ASM_GENERIC_DYNDBG_LDS_H

#include <asm-generic/bounded_sections.lds.h>
#define DYNDBG_SECTIONS()						\
	. = ALIGN(8);							\
	BOUNDED_SECTION_BY(__dyndbg_descriptors, ___dyndbg_descs)	\
	BOUNDED_SECTION_BY(__dyndbg_class_maps, ___dyndbg_class_maps)	\
	BOUNDED_SECTION_BY(.gnu.linkonce.d.__dyndbg_class_users.*,	\
			   ___dyndbg_class_users)

#define MOD_DYNDBG_SECTIONS()                                           \
	__dyndbg_descriptors : {					\
		BOUNDED_SECTION_BY(__dyndbg_descriptors,		\
				   ___dyndbg_descs)			\
	}								\
	__dyndbg_class_maps : {						\
		BOUNDED_SECTION_BY(__dyndbg_class_maps,			\
				   ___dyndbg_class_maps)		\
	}								\
	__dyndbg_class_users : {					\
		BOUNDED_SECTION_BY(.gnu.linkonce.d.__dyndbg_class_users.*, \
				   ___dyndbg_class_users)		\
	}

#endif /* __ASM_GENERIC_DYNDBG_LDS_H */
