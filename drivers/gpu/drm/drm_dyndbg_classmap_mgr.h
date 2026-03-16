/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DRM_DYNDBG_CLASSMAP_MGR_H_
#define _DRM_DYNDBG_CLASSMAP_MGR_H_

#include <linux/dynamic_debug.h>
#include <linux/kconfig.h>

#if defined(CONFIG_DRM_USE_DYNAMIC_DEBUG)
/*
 * This header is force-included in every C file in drivers/gpu/drm/
 * via subdir-ccflags-y. This ensures that every DRM module (built-in
 * or loadable) registers its interest in the 'drm_debug_classes'
 * classmap, by calling DYNAMIC_DEBUG_CLASSMAP_USE.
 *
 * DRM_DYNDBG_CLASSMAP_SKIP is defined in drm's makefile for core and
 * quirks - it breaks the dependency loop which happens if drm-core
 * both DEFINEs and USEs the classmap.
 *
 * The most obvious evidence of the effect is the presence of the
 * 'class' column in the dyndbg 'control' file for all drm_dbg()
 * callsites in the subsystem, without requiring explicit
 * DYNAMIC_DEBUG_CLASSMAP_USE() in every module.
 */

#if IS_REACHABLE(CONFIG_DRM) && !defined(DRM_DYNDBG_CLASSMAP_SKIP)
DYNAMIC_DEBUG_CLASSMAP_USE(drm_debug_classes);
#endif
#endif

#endif /* _DRM_DYNDBG_CLASSMAP_MGR_H_ */
