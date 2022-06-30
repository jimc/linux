/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_DEBUG_H__
#define __NVKM_DEBUG_H__
#define NV_DBG_FATAL    0
#define NV_DBG_ERROR    1
#define NV_DBG_WARN     2
#define NV_DBG_INFO     3
#define NV_DBG_DEBUG    4
#define NV_DBG_TRACE    5
#define NV_DBG_PARANOIA 6
#define NV_DBG_SPAM     7

enum nv_cli_dbg_verbose {
	NV_CLI_DBG_OFF = 10,
	NV_CLI_DBG_INFO,
	NV_CLI_DBG_DEBUG,
	NV_CLI_DBG_TRACE,
	NV_CLI_DBG_SPAM
};
enum nv_subdev_dbg_verbose {
	NV_SUBDEV_DBG_OFF = 15,
	NV_SUBDEV_DBG_INFO,
	NV_SUBDEV_DBG_DEBUG,
	NV_SUBDEV_DBG_TRACE,
	NV_SUBDEV_DBG_SPAM
};

#endif
