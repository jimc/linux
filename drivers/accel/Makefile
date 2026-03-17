# SPDX-License-Identifier: GPL-2.0-only

ccflags-$(CONFIG_DRM_USE_DYNAMIC_DEBUG)		+= -DDYNAMIC_DEBUG_MODULE -include $(srctree)/drivers/gpu/drm/drm_dyndbg_classmap_mgr.h
subdir-ccflags-$(CONFIG_DRM_USE_DYNAMIC_DEBUG)	+= -DDYNAMIC_DEBUG_MODULE -include $(srctree)/drivers/gpu/drm/drm_dyndbg_classmap_mgr.h

obj-$(CONFIG_DRM_ACCEL_AMDXDNA)		+= amdxdna/
obj-$(CONFIG_DRM_ACCEL_ARM_ETHOSU)	+= ethosu/
obj-$(CONFIG_DRM_ACCEL_HABANALABS)	+= habanalabs/
obj-$(CONFIG_DRM_ACCEL_IVPU)		+= ivpu/
obj-$(CONFIG_DRM_ACCEL_QAIC)		+= qaic/
obj-$(CONFIG_DRM_ACCEL_ROCKET)		+= rocket/
