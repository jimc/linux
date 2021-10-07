/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DYNAMIC_DEBUG_H
#define _DYNAMIC_DEBUG_H

#if defined(CONFIG_JUMP_LABEL)
#include <linux/jump_label.h>
#endif

/*
 * An instance of this structure is created in a special
 * ELF section at every dynamic debug callsite.  At runtime,
 * the special section is treated as an array of these.
 */
struct _ddebug {
	/*
	 * These fields are used to drive the user interface
	 * for selecting and displaying debug callsites.
	 */
	const char *modname;
	const char *function;
	const char *filename;
	const char *format;
	unsigned int lineno:18;
	/*
	 * The flags field controls the behaviour at the callsite.
	 * The bits here are changed dynamically when the user
	 * writes commands to <debugfs>/dynamic_debug/control
	 */
#define _DPRINTK_FLAGS_NONE	0
#define _DPRINTK_FLAGS_PRINT		(1<<0) /* printk() a message */
#define _DPRINTK_FLAGS_PRINT_TRACE	(1<<5) /* call (*tracefn) */

#define _DPRINTK_ENABLED (_DPRINTK_FLAGS_PRINT | _DPRINTK_FLAGS_PRINT_TRACE)

#define _DPRINTK_FLAGS_INCL_MODNAME	(1<<1)
#define _DPRINTK_FLAGS_INCL_FUNCNAME	(1<<2)
#define _DPRINTK_FLAGS_INCL_LINENO	(1<<3)
#define _DPRINTK_FLAGS_INCL_TID		(1<<4)

#define _DPRINTK_FLAGS_INCL_ANY		\
	(_DPRINTK_FLAGS_INCL_MODNAME | _DPRINTK_FLAGS_INCL_FUNCNAME |\
	 _DPRINTK_FLAGS_INCL_LINENO  | _DPRINTK_FLAGS_INCL_TID)

#if defined DEBUG
#define _DPRINTK_FLAGS_DEFAULT _DPRINTK_FLAGS_PRINT
#else
#define _DPRINTK_FLAGS_DEFAULT 0
#endif
	unsigned int flags:8;
#ifdef CONFIG_JUMP_LABEL
	union {
		struct static_key_true dd_key_true;
		struct static_key_false dd_key_false;
	} key;
#endif
} __attribute__((aligned(8)));



#if defined(CONFIG_DYNAMIC_DEBUG_CORE)

/* exported for module authors to exercise >control */
int dynamic_debug_exec_queries(const char *query, const char *modname);

int ddebug_add_module(struct _ddebug *tab, unsigned int n,
				const char *modname);
extern int ddebug_remove_module(const char *mod_name);
extern __printf(2, 3)
void __dynamic_pr_debug(struct _ddebug *descriptor, const char *fmt, ...);

extern int ddebug_dyndbg_module_param_cb(char *param, char *val,
					const char *modname);

struct device;

extern __printf(3, 4)
void __dynamic_dev_dbg(struct _ddebug *descriptor, const struct device *dev,
		       const char *fmt, ...);

struct net_device;

extern __printf(3, 4)
void __dynamic_netdev_dbg(struct _ddebug *descriptor,
			  const struct net_device *dev,
			  const char *fmt, ...);

struct ib_device;

extern __printf(3, 4)
void __dynamic_ibdev_dbg(struct _ddebug *descriptor,
			 const struct ib_device *ibdev,
			 const char *fmt, ...);

#define DEFINE_DYNAMIC_DEBUG_METADATA(name, fmt)		\
	static struct _ddebug  __aligned(8)			\
	__section("__dyndbg") name = {				\
		.modname = KBUILD_MODNAME,			\
		.function = __func__,				\
		.filename = __FILE__,				\
		.format = (fmt),				\
		.lineno = __LINE__,				\
		.flags = _DPRINTK_FLAGS_DEFAULT,		\
		_DPRINTK_KEY_INIT				\
	}

#ifdef CONFIG_JUMP_LABEL

#ifdef DEBUG

#define _DPRINTK_KEY_INIT .key.dd_key_true = (STATIC_KEY_TRUE_INIT)

#define DYNAMIC_DEBUG_BRANCH(descriptor) \
	static_branch_likely(&descriptor.key.dd_key_true)
#else
#define _DPRINTK_KEY_INIT .key.dd_key_false = (STATIC_KEY_FALSE_INIT)

#define DYNAMIC_DEBUG_BRANCH(descriptor) \
	static_branch_unlikely(&descriptor.key.dd_key_false)
#endif

#else /* !CONFIG_JUMP_LABEL */

#define _DPRINTK_KEY_INIT

#ifdef DEBUG
#define DYNAMIC_DEBUG_BRANCH(descriptor) \
	likely(descriptor.flags & _DPRINTK_FLAGS_PRINT)
#else
#define DYNAMIC_DEBUG_BRANCH(descriptor) \
	unlikely(descriptor.flags & _DPRINTK_FLAGS_PRINT)
#endif

#endif /* CONFIG_JUMP_LABEL */

#define __dynamic_func_call(id, fmt, func, ...) do {	\
	DEFINE_DYNAMIC_DEBUG_METADATA(id, fmt);		\
	if (DYNAMIC_DEBUG_BRANCH(id))			\
		func(&id, ##__VA_ARGS__);		\
} while (0)

#define __dynamic_func_call_no_desc(id, fmt, func, ...) do {	\
	DEFINE_DYNAMIC_DEBUG_METADATA(id, fmt);			\
	if (DYNAMIC_DEBUG_BRANCH(id))				\
		func(__VA_ARGS__);				\
} while (0)

/*
 * "Factory macro" for generating a call to func, guarded by a
 * DYNAMIC_DEBUG_BRANCH. The dynamic debug descriptor will be
 * initialized using the fmt argument. The function will be called with
 * the address of the descriptor as first argument, followed by all
 * the varargs. Note that fmt is repeated in invocations of this
 * macro.
 */
#define _dynamic_func_call(fmt, func, ...)				\
	__dynamic_func_call(__UNIQUE_ID(ddebug), fmt, func, ##__VA_ARGS__)
/*
 * A variant that does the same, except that the descriptor is not
 * passed as the first argument to the function; it is only called
 * with precisely the macro's varargs.
 */
#define _dynamic_func_call_no_desc(fmt, func, ...)	\
	__dynamic_func_call_no_desc(__UNIQUE_ID(ddebug), fmt, func, ##__VA_ARGS__)

#define dynamic_pr_debug(fmt, ...)				\
	_dynamic_func_call(fmt,	__dynamic_pr_debug,		\
			   pr_fmt(fmt), ##__VA_ARGS__)

#define dynamic_dev_dbg(dev, fmt, ...)				\
	_dynamic_func_call(fmt,__dynamic_dev_dbg, 		\
			   dev, fmt, ##__VA_ARGS__)

#define dynamic_netdev_dbg(dev, fmt, ...)			\
	_dynamic_func_call(fmt, __dynamic_netdev_dbg,		\
			   dev, fmt, ##__VA_ARGS__)

#define dynamic_ibdev_dbg(dev, fmt, ...)			\
	_dynamic_func_call(fmt, __dynamic_ibdev_dbg,		\
			   dev, fmt, ##__VA_ARGS__)

#define dynamic_hex_dump(prefix_str, prefix_type, rowsize,		\
			 groupsize, buf, len, ascii)			\
	_dynamic_func_call_no_desc(__builtin_constant_p(prefix_str) ? prefix_str : "hexdump", \
				   print_hex_dump,			\
				   KERN_DEBUG, prefix_str, prefix_type,	\
				   rowsize, groupsize, buf, len, ascii)

struct kernel_param;
int param_set_dyndbg(const char *instr, const struct kernel_param *kp);
int param_get_dyndbg(char *buffer, const struct kernel_param *kp);

#else /* !CONFIG_DYNAMIC_DEBUG_CORE */

#include <linux/string.h>
#include <linux/errno.h>
#include <linux/printk.h>

static inline int ddebug_add_module(struct _ddebug *tab, unsigned int n,
				    const char *modname)
{
	return 0;
}

static inline int ddebug_remove_module(const char *mod)
{
	return 0;
}

static inline int ddebug_dyndbg_module_param_cb(char *param, char *val,
						const char *modname)
{
	if (strstr(param, "dyndbg")) {
		/* avoid pr_warn(), which wants pr_fmt() fully defined */
		printk(KERN_WARNING "dyndbg param is supported only in "
			"CONFIG_DYNAMIC_DEBUG builds\n");
		return 0; /* allow and ignore */
	}
	return -EINVAL;
}

#define dynamic_pr_debug(fmt, ...)					\
	do { if (0) printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__); } while (0)
#define dynamic_dev_dbg(dev, fmt, ...)					\
	do { if (0) dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__); } while (0)
#define dynamic_hex_dump(prefix_str, prefix_type, rowsize,		\
			 groupsize, buf, len, ascii)			\
	do { if (0)							\
		print_hex_dump(KERN_DEBUG, prefix_str, prefix_type,	\
				rowsize, groupsize, buf, len, ascii);	\
	} while (0)

static inline int dynamic_debug_exec_queries(const char *query, const char *modname)
{
	pr_warn("kernel not built with CONFIG_DYNAMIC_DEBUG_CORE\n");
	return 0;
}

struct kernel_param;
static inline int param_set_dyndbg(const char *instr, const struct kernel_param *kp)
{ return 0; }
static inline int param_get_dyndbg(char *buffer, const struct kernel_param *kp)
{ return 0; }

#endif /* !CONFIG_DYNAMIC_DEBUG_CORE */

struct dyndbg_bitdesc {
	const char *match;	/* search format for this substr */
};

struct dyndbg_bitmap_param {
	unsigned long *bits;		/* ref to shared state */
	struct dyndbg_bitdesc map[];	/* indexed by bitpos */
};

#if defined(CONFIG_DYNAMIC_DEBUG) || \
	(defined(CONFIG_DYNAMIC_DEBUG_CORE) && defined(DYNAMIC_DEBUG_MODULE))
/**
 * DEFINE_DYNAMIC_DEBUG_CATEGORIES() - bitmap control of categorized prdbgs
 * @fsname: parameter basename under /sys
 * @_var:    C-identifier holding bitmap
 * @desc:  string summarizing the controls provided
 * @...:    list of struct dyndbg_bitdesc initializations
 *
 * Intended for modules with substantial use of "categorized" prdbgs
 * (those with some systematic prefix in the format string), this lets
 * modules using pr_debug to control them in groups according to their
 * format prefixes, and map them to bits 0-N of a sysfs control point.
 * Each @... gives the index and prefix map.
 */
#define DEFINE_DYNAMIC_DEBUG_CATEGORIES(fsname, _var, desc, ...)	\
	MODULE_PARM_DESC(fsname, desc);					\
	static struct dyndbg_bitmap_param ddcats_##_var =		\
	{ .bits = &(_var), .map = { __VA_ARGS__, { .match = NULL }}};	\
	module_param_cb(fsname, &param_ops_dyndbg, &ddcats_##_var, 0644)

extern const struct kernel_param_ops param_ops_dyndbg;

#else /* no dyndbg configured, throw error on macro use */

#if (defined(CONFIG_DYNAMIC_DEBUG_CORE) && !defined(DYNAMIC_DEBUG_MODULE))
#define DEFINE_DYNAMIC_DEBUG_CATEGORIES(fsname, var, bitmap_desc, ...)	\
	BUILD_BUG_ON_MSG(1, "you need -DDYNAMIC_DEBUG_MODULE in compile")
#else
#define DEFINE_DYNAMIC_DEBUG_CATEGORIES(fsname, var, bitmap_desc, ...)	\
	BUILD_BUG_ON_MSG(1, "CONFIG_DYNAMIC_DEBUG needed to use this macro: " #var)
#endif
#endif /* DYNDBG || _CORE &&_MODULE */

void dynamic_debug_register_tracer(struct module *mod,
				   void (*tracefn)(const char *lbl, struct va_format *vaf));
void dynamic_debug_unregister_tracer(struct module *mod,
				     void (*tracefn)(const char *lbl, struct va_format *vaf));

#endif
