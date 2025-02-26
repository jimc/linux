// SPDX-License-Identifier: GPL-2.0-or-later

#include "drm/drm_print.h"
/*
 * if DRM_USE_DYNAMIC_DEBBUG:
 *    DYNDBG_CLASSMAP_USE(drm_debug_classes);
 *
 * dyndbg classmaps are opt-in, so modules which call drm:_*_dbg must
 * link this to authorize dyndbg to change the static-keys underneath.
 */
DRM_CLASSMAP_USE(drm_debug_classes);
