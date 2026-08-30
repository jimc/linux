/*
 * lib/dynamic_debug.c
 *
 * make pr_debug()/dev_dbg() calls runtime configurable based upon their
 * source module.
 *
 * Copyright (C) 2008 Jason Baron <jbaron@redhat.com>
 * By Greg Banks <gnb@melbourne.sgi.com>
 * Copyright (c) 2008 Silicon Graphics Inc.  All Rights Reserved.
 * Copyright (C) 2011 Bart Van Assche.  All Rights Reserved.
 * Copyright (C) 2013 Du, Changbin <changbin.du@gmail.com>
 */

#define pr_fmt(fmt) "dyndbg: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/types.h>
#include <linux/bonsai_tree.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/list.h>
#include <linux/sysctl.h>
#include <linux/ctype.h>
#include <linux/string.h>

#include <linux/parser.h>
#include <linux/string_helpers.h>
#include <linux/uaccess.h>
#include <linux/dynamic_debug.h>

#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/jump_label.h>
#include <linux/hardirq.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/netdevice.h>
#include <linux/zstd.h>
#include <linux/vmalloc.h>
#include <linux/firmware.h>

#include <rdma/ib_verbs.h>

extern struct _ddebug __start___dyndbg_descs[];
extern struct _ddebug __stop___dyndbg_descs[];
extern const struct _ddebug_site __start___dyndbg_sites[];
extern const struct _ddebug_site __stop___dyndbg_sites[];
extern const char __start___dyndbg_strings_mod[];
extern const char __stop___dyndbg_strings_mod[];
extern const char __start___dyndbg_strings_file[];
extern const char __stop___dyndbg_strings_file[];
extern struct ddebug_class_map __start___dyndbg_class_maps[];
extern struct ddebug_class_map __stop___dyndbg_class_maps[];
extern struct ddebug_class_user __start___dyndbg_class_users[];
extern struct ddebug_class_user __stop___dyndbg_class_users[];

struct ddebug_table {
	struct list_head link;
	struct _ddebug_info info;
	struct bonsai_tree site_map;
	void *compressed_sites;
	unsigned long compressed_len;
};

struct ddebug_query {
	const char *filename;
	const char *module;
	const char *function;
	const char *format;
	const char *class_string;
	unsigned int first_lineno, last_lineno;
};

struct ddebug_iter {
	struct ddebug_table *table;
	int idx;
};

struct flag_settings {
	unsigned int flags;
	unsigned int mask;
};

struct _dd_prefix_key_range {
	unsigned long start;
	unsigned long end;
};

static DEFINE_PER_CPU(unsigned long, ddebug_call_count);

void ddebug_increment_call_count(void)
{
	this_cpu_inc(ddebug_call_count);
}
EXPORT_SYMBOL(ddebug_increment_call_count);

static bool ddebug_class_map_in_range(const int class_id,
				      const struct ddebug_class_map *map);
static bool ddebug_class_user_in_range(const int class_id,
				       const struct ddebug_class_user *user);
static DEFINE_MUTEX(ddebug_lock);
static LIST_HEAD(ddebug_tables);
static int verbose;
module_param(verbose, int, 0644);
MODULE_PARM_DESC(verbose, " dynamic_debug/control processing "
		 "( 0 = off (default), 1 = module add/rm, 2 = >control summary, 3 = parsing, 4 = per-site changes)");

/*
 * during (mod)_init, fill these from __dyndbg_sites data.  They
 * deduplicate the column values, and remember their (nested,
 * non-overlapping) ranges intrinsically.  At runtime, they provide
 * values for use in `cat control` & `echo $cmd >control`
 */
static struct bonsai_tree dd_builtin_site_map = BONSAI_TREE_INIT;
static void *dd_builtin_compressed_sites;
static unsigned long dd_builtin_compressed_len;

static inline struct bonsai_tree *ddebug_get_site_map(struct ddebug_table *dt)
{
	return dt && dt->site_map.num_segments ? &dt->site_map : &dd_builtin_site_map;
}

#define DD_KEY_ALIGN_MASK   7UL

/* Key offsets from the 8-byte aligned descriptor address */
#define DD_KEY_FILE_OFFSET  1UL
#define DD_KEY_MOD_OFFSET   2UL

/* Expected key alignment remainders (addr & DD_KEY_ALIGN_MASK) */
#define DD_KEY_FUNC_ALIGN   0UL
#define DD_KEY_FILE_ALIGN   7UL
#define DD_KEY_MOD_ALIGN    6UL

/* Site tag types for growing/condensing */
#define DD_TAG_MOD          0UL
#define DD_TAG_FILE         1UL
#define DD_TAG_FUNC         2UL

/* cache of composed prefixes for enabled and invoked pr_debugs */
static DEFINE_MTREE(pr_prefixes);
static unsigned int pr_prefixes_count;

static unsigned long ddebug_prefix_key(const struct _ddebug *desc);

//static void ddebug_drop_cached_prefix(const struct _ddebug *dp);
//static void ddebug_prefix_range(const struct _ddebug *desc, struct _dd_prefix_key_range *range);

static void ddebug_add_cached_prefix(struct _ddebug *dp);
static void __maybe_unused ddebug_drop_all_cached_prefixes(const struct _ddebug_info *di);
static void ddebug_prefix_range(const struct _ddebug *desc,
				struct _dd_prefix_key_range *range);

static int ddebug_reconstruct_site_map(struct ddebug_table *dt);

#define prefix_flags(flags)  (flags & _DPRINTK_FLAGS_INCL_LOOKUP)

/* Return the path relative to source root */
static inline const char *trim_prefix(const char *path)
{
	int skip = strlen(__FILE__) - strlen("lib/dynamic_debug.c");

	if (strncmp(path, __FILE__, skip))
		skip = 0; /* prefix mismatch, don't skip */

	return path + skip;
}

static const struct { unsigned flag:8; char opt_char; } opt_array[] = {
	{ _DPRINTK_FLAGS_PRINT, 'p' },
	{ _DPRINTK_FLAGS_INCL_MODNAME, 'm' },
	{ _DPRINTK_FLAGS_INCL_FUNCNAME, 'f' },
	{ _DPRINTK_FLAGS_INCL_SOURCENAME, 's' },
	{ _DPRINTK_FLAGS_INCL_LINENO, 'l' },
	{ _DPRINTK_FLAGS_INCL_TID, 't' },
	{ _DPRINTK_FLAGS_INCL_STACK, 'd' },
	{ _DPRINTK_FLAGS_COUNT, 'c' },
	{ _DPRINTK_FLAGS_NONE, '_' },
};

struct flagsbuf { char buf[ARRAY_SIZE(opt_array)+1]; };

/* format a string into buf[] which describes the _ddebug's flags */
static char *ddebug_describe_flags(unsigned int flags, struct flagsbuf *fb)
{
	char *p = fb->buf;
	int i;

	for (i = 0; i < ARRAY_SIZE(opt_array); ++i)
		if (flags & opt_array[i].flag)
			*p++ = opt_array[i].opt_char;
	if (p == fb->buf)
		*p++ = '_';
	*p = '\0';

	return fb->buf;
}

#define vnpr_info(lvl, fmt, ...)				\
do {								\
	if (verbose >= lvl)					\
		pr_info(fmt, ##__VA_ARGS__);			\
} while (0)

#define vpr_info(fmt, ...)	vnpr_info(1, fmt, ##__VA_ARGS__)
#define v2pr_info(fmt, ...)	vnpr_info(2, fmt, ##__VA_ARGS__)
#define v3pr_info(fmt, ...)	vnpr_info(3, fmt, ##__VA_ARGS__)
#define v4pr_info(fmt, ...)	vnpr_info(4, fmt, ##__VA_ARGS__)
#define v5pr_info(fmt, ...)	vnpr_info(5, fmt, ##__VA_ARGS__)

static void v3pr_info_dq(const struct ddebug_query *query, const char *msg)
{
	/* trim any trailing newlines */
	int fmtlen = 0;

	if (query->format) {
		fmtlen = strlen(query->format);
		while (fmtlen && query->format[fmtlen - 1] == '\n')
			fmtlen--;
	}

	v3pr_info("%s: func=\"%s\" file=\"%s\" module=\"%s\" format=\"%.*s\" lineno=%u-%u class=%s\n",
		  msg,
		  query->function ?: "",
		  query->filename ?: "",
		  query->module ?: "",
		  fmtlen, query->format ?: "",
		  query->first_lineno, query->last_lineno, query->class_string);
}

/*
 * simplify a repeated for-loop pattern walking N steps in a T _vec
 * member inside a struct _box.  It expects int i and T *_sp to be
 * declared in the caller.
 * @_i:  caller provided counter.
 * @_sp: cursor into _vec, to examine each item.
 * @_box: ptr to a struct containing @_vec member
 * @_vec: name of a member in @_box
 */
#define for_subvec(_i, _sp, _box, _vec)			\
	for ((_i) = 0, (_sp) = (_box)->_vec.start;	\
	     (_i) < (_box)->_vec.len;			\
	     (_i)++, (_sp)++)		/* { block } */

#define v2pr_di_info(di_p, msg_p, ...)					\
({									\
	struct _ddebug_info const *_di = di_p;				\
	v2pr_info(msg_p "module:%s nd:%d nc:%d nu:%d\n", ##__VA_ARGS__, \
		  _di->mod_name, _di->descs.len, _di->maps.len,         \
		  _di->users.len);                                      \
})

static struct ddebug_class_map *ddebug_find_valid_class(struct _ddebug_info const *di,
							 const char *query_class,
							 int *class_id)
{
	struct ddebug_class_map *map;
	struct ddebug_class_user *cli;
	int i, idx;

	for_subvec(i, map, di, maps) {
		idx = match_string(map->class_names, map->length, query_class);
		if (idx >= 0) {
			v2pr_di_info(di, "good-class: %s.%s ", map->mod_name, query_class);
			*class_id = idx + map->base;
			return map;
		}
	}
	for_subvec(i, cli, di, users) {
		idx = match_string(cli->map->class_names, cli->map->length, query_class);
		if (idx >= 0) {
			v2pr_di_info(di, "class-ref: %s -> %s.%s ",
				    cli->mod_name, cli->map->mod_name, query_class);
			*class_id = idx + cli->map->base + cli->offset;
			return cli->map;
		}
	}
	*class_id = -ENOENT;
	return NULL;
}



static struct ddebug_class_map *
ddebug_find_map_by_class_id(struct _ddebug_info *di, int class_id)
{
	struct ddebug_class_map *map;
	struct ddebug_class_user *cli;
	int i;

	for_subvec(i, map, di, maps)
		if (ddebug_class_map_in_range(class_id, map))
			return map;

	for_subvec(i, cli, di, users)
		if (ddebug_class_user_in_range(class_id, cli))
			return cli->map;

	return NULL;
}

/*
 * classmaps-V1 protected classes from changes by legacy commands
 * (those selecting _DPRINTK_CLASS_DFLT by omission).  This had the
 * downside that saying "class FOO" for every change can get tedious.
 *
 * V2 is smarter, it protects class-maps if the defining module also
 * calls DYNAMIC_DEBUG_CLASSMAP_PARAM to create a sysfs parameter.
 * Since the author wants the knob, we should assume they intend to
 * use it (in preference to "class FOO +p" >control), and want to
 * trust its settings.  This gives protection when its useful, and not
 * when its just tedious.
 */
static inline bool ddebug_class_has_param(const struct ddebug_class_map *map)
{
	return !!(map->controlling_param);
}

/* re-framed as a policy choice */
#define ddebug_class_wants_protection(map) (ddebug_class_has_param(map))

static inline unsigned long ddebug_site_tag_key(unsigned long addr, unsigned long tag)
{
	return (tag << 60) | (addr & 0x0fffffffffffffffUL);
}

static void ddebug_resolve_site(struct bonsai_tree *bt, const struct _ddebug *dp,
				const char **mod, const char **file, const char **func)
{
	unsigned long addr = (unsigned long)dp;

	if (mod)  *mod  = bonsai_lookup(bt, ddebug_site_tag_key(addr, DD_TAG_MOD));
	if (file) *file = bonsai_lookup(bt, ddebug_site_tag_key(addr, DD_TAG_FILE));
	if (func) *func = bonsai_lookup(bt, ddebug_site_tag_key(addr, DD_TAG_FUNC));

	if (mod  && !*mod)  *mod  = "unknown";
	if (file && !*file) *file = "unknown";
	if (func && !*func) *func = "unknown";
}

static const char *desc_function(struct _ddebug const *dp)
{
	const char *func;
	struct ddebug_table *dt;
	struct bonsai_tree *site_map = &dd_builtin_site_map;
	list_for_each_entry(dt, &ddebug_tables, link) {
		if (dp >= dt->info.descs.start &&
		    dp < dt->info.descs.start + dt->info.descs.len) {
			site_map = ddebug_get_site_map(dt);
			break;
		}
	}
	ddebug_resolve_site(site_map, dp, NULL, NULL, &func);
	return func;
}

static const char *desc_filename(struct _ddebug const *dp)
{
	const char *file;
	struct ddebug_table *dt;
	struct bonsai_tree *site_map = &dd_builtin_site_map;
	list_for_each_entry(dt, &ddebug_tables, link) {
		if (dp >= dt->info.descs.start &&
		    dp < dt->info.descs.start + dt->info.descs.len) {
			site_map = ddebug_get_site_map(dt);
			break;
		}
	}
	ddebug_resolve_site(site_map, dp, NULL, &file, NULL);
	return file;
}

/*
 * Search the tables for _ddebug's which match the given `query' and
 * apply the `flags' and `mask' to them.  Returns number of matching
 * callsites, normally the same as number of changes.  If verbose,
 * logs the changes.  Takes ddebug_lock.
 */
static bool ddebug_match_desc(const struct ddebug_query *query,
			      struct _ddebug *dp,
			      struct _ddebug_info *di,
			      int selected_class)
{
	struct ddebug_class_map *class_map;
	struct ddebug_table *dt = container_of(di, struct ddebug_table, info);
	const char *dp_modname = NULL, *dp_filename = NULL, *dp_function = NULL;

	/* get site vals needed to match this query */
	ddebug_resolve_site(ddebug_get_site_map(dt), dp,
			    (query->module && !strcmp(di->mod_name, "vmlinux")) ? &dp_modname : NULL,
			    query->filename ? &dp_filename : NULL,
			    query->function ? &dp_function : NULL);

	/* match against module for stripped fallback tables */
	if (query->module && !strcmp(di->mod_name, "vmlinux") &&
	    (!dp_modname ||
	     (!match_wildcard_hyphen(query->module, dp_modname) &&
	      !match_wildcard_hyphen(query->module, kbasename(dp_modname)))))
		return false;

	/* match against the source filename */
	if (query->filename &&
	    !match_wildcard(query->filename, dp_filename) &&
	    !match_wildcard(query->filename,
			    kbasename(dp_filename)) &&
	    !match_wildcard(query->filename,
			    trim_prefix(dp_filename)))
		return false;

	/* match against the function */
	if (query->function &&
	    !match_wildcard(query->function, dp_function))
		return false;

	/* match against the format */
	if (query->format) {
		if (!dp->format) {
			pr_err_ratelimited("ddebug: NULL format string at %s:%s:%u\n",
					   desc_filename(dp) ? desc_filename(dp) : "?",
					   desc_function(dp) ? desc_function(dp) : "?",
					   dp->lineno);
			return false;
		}
		if (*query->format == '^') {
			char *p;
			/* anchored search. match must be at beginning */
			p = strstr(dp->format, query->format + 1);
			if (p != dp->format)
				return false;
		} else if (!strstr(dp->format, query->format)) {
			return false;
		}
	}

	/* match against the line number range */
	if (query->first_lineno &&
	    dp->lineno < query->first_lineno)
		return false;
	if (query->last_lineno &&
	    dp->lineno > query->last_lineno)
		return false;

	/*
	 * above are all satisfied, so we can make final decisions:
	 * 1- class FOO or implied class __DEFAULT__
	 * 2- site.is_classed or not
	 */
	if (query->class_string) {
		/* class FOO given, exact match required */
		return (dp->class_id == selected_class);
	}
	/* query class __DEFAULT__ by omission. */
	if (dp->class_id == _DPRINTK_CLASS_DFLT) {
		/* un-classed site */
		return true;
	}
	/* site is class'd */
	class_map = ddebug_find_map_by_class_id(di, dp->class_id);
	if (!class_map) {
		pr_warn_ratelimited("unknown class_id %d, check %s's CLASSMAP definitions\n",
			  dp->class_id, di->mod_name);
		return false;
	}
	/* module(-param) decides protection */
	return !ddebug_class_wants_protection(class_map);
}

static int ddebug_change(const struct ddebug_query *query, struct flag_settings *modifiers)
{
	int i;
	struct ddebug_table *dt;
	unsigned int newflags;
	unsigned int nfound = 0;
	struct flagsbuf fbuf, nbuf;
	int selected_class;

	/* search for matching ddebugs */
	mutex_lock(&ddebug_lock);

	/* Reconstruct the global built-in site map if it was shrunk */
	if (ddebug_reconstruct_site_map(NULL))
		pr_warn("Failed to reconstruct built-in site map\n");

	list_for_each_entry(dt, &ddebug_tables, link) {
		struct _ddebug_info *di = &dt->info;
		struct ddebug_class_map *mods_map;

		/* Reconstruct the module's site map if it was shrunk */
		if (ddebug_reconstruct_site_map(dt))
			pr_warn("Failed to reconstruct site map for module %s\n", di->mod_name);

		/* match against the module name */
		if (query->module &&
		    strcmp(di->mod_name, "vmlinux") != 0 &&
		    !match_wildcard_hyphen(query->module, di->mod_name) &&
		    !match_wildcard_hyphen(query->module, kbasename(di->mod_name)))
			continue;

		selected_class = _DPRINTK_CLASS_DFLT;
		if (query->class_string) {
			mods_map = ddebug_find_valid_class(di, query->class_string,
							   &selected_class);
			if (!mods_map)
				continue;
		}

		for (i = 0; i < di->descs.len; i++) {
			struct _ddebug *dp = &di->descs.start[i];

			if (!ddebug_match_desc(query, dp, di, selected_class))
				continue;

			nfound++;

			newflags = (dp->flags & modifiers->mask) | modifiers->flags;
			if (newflags == dp->flags)
				continue;

#ifdef CONFIG_JUMP_LABEL
			if (dp->flags & _DPRINTK_FLAGS_ENABLED) {
				if (!(newflags & _DPRINTK_FLAGS_ENABLED))
					static_branch_disable(&dp->key.dd_key_true);
			} else if (newflags & _DPRINTK_FLAGS_ENABLED) {
				static_branch_enable(&dp->key.dd_key_true);
			}
#endif
			v4pr_info("changed %s:%d [%s]%s %s => %s\n",
				  trim_prefix(desc_filename(dp)), dp->lineno,
				  di->mod_name, desc_function(dp),
				  ddebug_describe_flags(dp->flags, &fbuf),
				  ddebug_describe_flags(newflags, &nbuf));
			dp->flags = newflags;
			if (prefix_flags(newflags))
				ddebug_add_cached_prefix(dp);
		}
	}
	mutex_unlock(&ddebug_lock);

	return nfound;
}

static char *skip_spaces_and_commas(const char *str)
{
	str = skip_spaces(str);
	while (*str == ',')
		str = skip_spaces(++str);
	return (char *)str;
}

/*
 * Split the buffer `buf' into space-separated words.
 * Handles simple " and ' quoting, i.e. without nested,
 * embedded or escaped \".  Return the number of words
 * or <0 on error.
 */
static int ddebug_tokenize(char *buf, char *words[], int maxwords)
{
	int nwords = 0;

	while (*buf) {
		char *end;

		/* Skip leading whitespace and comma */
		buf = skip_spaces_and_commas(buf);
		if (!*buf)
			break;	/* oh, it was trailing whitespace */
		if (*buf == '#')
			break;	/* token starts comment, skip rest of line */

		/* find `end' of word, whitespace separated or quoted */
		if (*buf == '"' || *buf == '\'') {
			int quote = *buf++;
			for (end = buf; *end && *end != quote; end++)
				;
			if (!*end) {
				pr_err("unclosed quote: %s\n", buf);
				return -EINVAL;	/* unclosed quote */
			}
		} else {
			for (end = buf; *end && !isspace(*end) && *end != ','; end++)
				;
			if (end == buf) {
				pr_err("parse err after word:%d=%s\n", nwords,
				       nwords ? words[nwords - 1] : "<none>");
				return -EINVAL;
			}
		}

		/* `buf' is start of word, `end' is one past its end */
		if (nwords == maxwords) {
			pr_err("too many words, legal max <=%d\n", maxwords);
			return -EINVAL;	/* ran out of words[] before bytes */
		}
		if (*end)
			*end++ = '\0';	/* terminate the word */
		words[nwords++] = buf;
		buf = end;
	}

	if (verbose >= 3) {
		int i;
		pr_info("split into words:");
		for (i = 0; i < nwords; i++)
			pr_cont(" \"%s\"", words[i]);
		pr_cont("\n");
	}

	return nwords;
}

/*
 * Parse a single line number.  Note that the empty string ""
 * is treated as a special case and converted to zero, which
 * is later treated as a "don't care" value.
 */
static inline int parse_lineno(const char *str, unsigned int *val)
{
	BUG_ON(str == NULL);
	if (*str == '\0') {
		*val = 0;
		return 0;
	}
	if (kstrtouint(str, 10, val) < 0) {
		pr_err("bad line-number: %s\n", str);
		return -EINVAL;
	}
	return 0;
}

static int parse_linerange(struct ddebug_query *query, const char *first)
{
	char *last = strchr(first, '-');

	if (query->first_lineno || query->last_lineno) {
		pr_err("match-spec: line used 2x\n");
		return -EINVAL;
	}
	if (last)
		*last++ = '\0';
	if (parse_lineno(first, &query->first_lineno) < 0)
		return -EINVAL;
	if (last) {
		/* range <first>-<last> */
		if (parse_lineno(last, &query->last_lineno) < 0)
			return -EINVAL;

		/* special case for last lineno not specified */
		if (query->last_lineno == 0)
			query->last_lineno = UINT_MAX;

		if (query->last_lineno < query->first_lineno) {
			pr_err("last-line:%d < 1st-line:%d\n",
			       query->last_lineno,
			       query->first_lineno);
			return -EINVAL;
		}
	} else {
		query->last_lineno = query->first_lineno;
	}
	v3pr_info("parsed line %d-%d\n", query->first_lineno,
		 query->last_lineno);
	return 0;
}

static int check_set(const char **dest, char *src, char *name)
{
	int rc = 0;

	if (*dest) {
		rc = -EINVAL;
		pr_err("match-spec:%s val:%s overridden by %s\n",
		       name, *dest, src);
	}
	*dest = src;
	return rc;
}

/*
 * Parse words[] as a ddebug query specification, which is a series
 * of (keyword, value) pairs chosen from these possibilities:
 *
 * func <function-name>
 * file <full-pathname>
 * file <base-filename>
 * module <module-name>
 * format <escaped-string-to-find-in-format>
 * line <lineno>
 * line <first-lineno>-<last-lineno> // where either may be empty
 *
 * Only 1 of each type is allowed.
 * Returns 0 on success, <0 on error.
 */
static int ddebug_parse_query(char *words[], int nwords,
			struct ddebug_query *query, const char *modname)
{
	unsigned int i;
	int rc = 0;
	char *fline;

	/* check we have an even number of words */
	if (nwords % 2 != 0) {
		pr_err("expecting pairs of match-spec <value>\n");
		return -EINVAL;
	}

	for (i = 0; i < nwords; i += 2) {
		char *keyword = words[i];
		char *arg = words[i+1];

		if (!strcmp(keyword, "func")) {
			rc = check_set(&query->function, arg, "func");
		} else if (!strcmp(keyword, "file")) {
			if (check_set(&query->filename, arg, "file"))
				return -EINVAL;

			/* tail :$info is function or line-range */
			fline = strchr(query->filename, ':');
			if (!fline)
				continue;
			*fline++ = '\0';
			if (isalpha(*fline) || *fline == '*' || *fline == '?') {
				/* take as function name */
				if (check_set(&query->function, fline, "func"))
					return -EINVAL;
			} else {
				if (parse_linerange(query, fline))
					return -EINVAL;
			}
		} else if (!strcmp(keyword, "module")) {
			rc = check_set(&query->module, arg, "module");
		} else if (!strcmp(keyword, "format")) {
			string_unescape_inplace(arg, UNESCAPE_SPACE |
							    UNESCAPE_OCTAL |
							    UNESCAPE_SPECIAL);
			rc = check_set(&query->format, arg, "format");
		} else if (!strcmp(keyword, "line")) {
			if (parse_linerange(query, arg))
				return -EINVAL;
		} else if (!strcmp(keyword, "class")) {
			rc = check_set(&query->class_string, arg, "class");
		} else {
			pr_err("unknown keyword \"%s\"\n", keyword);
			return -EINVAL;
		}
		if (rc)
			return rc;
	}
	if (!query->module && modname)
		/*
		 * support $modname.dyndbg=<multiple queries>, when
		 * not given in the query itself
		 */
		query->module = modname;

	return 0;
}

/*
 * Parse `str' as a flags specification, format [-+=][p]+.
 * Sets up *maskp and *flagsp to be used when changing the
 * flags fields of matched _ddebug's.  Returns 0 on success
 * or <0 on error.
 */
static int ddebug_parse_flags(const char *str, struct flag_settings *modifiers)
{
	int op, i;

	switch (*str) {
	case '+':
	case '-':
	case '=':
		op = *str++;
		break;
	default:
		pr_err("bad flag-op %c, at start of %s\n", *str, str);
		return -EINVAL;
	}

	for (; *str ; ++str) {
		for (i = ARRAY_SIZE(opt_array) - 1; i >= 0; i--) {
			if (*str == opt_array[i].opt_char) {
				modifiers->flags |= opt_array[i].flag;
				break;
			}
		}
		if (i < 0) {
			pr_err("unknown flag '%c'\n", *str);
			return -EINVAL;
		}
	}

	/* calculate final flags, mask based upon op */
	switch (op) {
	case '=':
		/* modifiers->flags already set */
		modifiers->mask = 0;
		break;
	case '+':
		modifiers->mask = ~0U;
		break;
	case '-':
		modifiers->mask = ~modifiers->flags;
		modifiers->flags = 0;
		break;
	}
	v3pr_info("op='%c' flags=0x%x maskp=0x%x\n", op, modifiers->flags, modifiers->mask);

	return 0;
}

static int ddebug_load_zstd_metadata(const void *src, size_t src_len);

static inline bool str_has_suffix(const char *str, const char *suffix)
{
	size_t str_len = strlen(str);
	size_t suffix_len = strlen(suffix);

	return str_len >= suffix_len && !strcmp(str + str_len - suffix_len, suffix);
}

static int ddebug_load_firmware_metadata(const char *name)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware_direct(&fw, name, NULL);
	if (ret) {
		pr_err("failed to load site metadata firmware '%s': %d\n", name, ret);
		return ret;
	}

	ret = ddebug_load_zstd_metadata(fw->data, fw->size);
	release_firmware(fw);
	return ret;
}

static int ddebug_exec_query(char *query_string, const char *modname)
{
	struct flag_settings modifiers = {};
	struct ddebug_query query = {};
#define MAXWORDS 15
	int nwords, nfound;
	char *words[MAXWORDS];

	nwords = ddebug_tokenize(query_string, words, MAXWORDS);
	if (nwords <= 0) {
		pr_err("tokenize failed\n");
		return -EINVAL;
	}

	/* Check for standalone metadata load directive: ends in .zst or .dyndbg */
	if (nwords == 1 && (str_has_suffix(words[0], ".zst") || str_has_suffix(words[0], ".dyndbg")))
		return ddebug_load_firmware_metadata(words[0]);

	/* check flags 1st (last arg) so query is pairs of spec,val */
	if (ddebug_parse_flags(words[nwords-1], &modifiers)) {
		pr_err("flags parse failed\n");
		return -EINVAL;
	}
	if (ddebug_parse_query(words, nwords-1, &query, modname)) {
		pr_err("query parse failed\n");
		return -EINVAL;
	}

	/* actually go and implement the change */
	nfound = ddebug_change(&query, &modifiers);
	v3pr_info_dq(&query, nfound ? "applied" : "no-match");

	return nfound;
}

/* handle multiple queries in query string, continue on error, return
   last error or number of matching callsites.  Module name is either
   in the modname arg (for boot args) or perhaps in query string.
*/
static int ddebug_exec_queries(char *query, const char *modname)
{
	char *split;
	int i, errs = 0, exitcode = 0, rc, nfound = 0;

	for (i = 0; query; query = split) {
		split = strpbrk(query, "@;\n");
		if (split)
			*split++ = '\0';

		query = skip_spaces_and_commas(query);

		if (!query || !*query || *query == '#')
			continue;

		if (modname)
			v2pr_info("query %d: module %s \"%s\"\n", i, modname, query);
		else
			v2pr_info("query %d: \"%s\"\n", i, query);

		rc = ddebug_exec_query(query, modname);
		if (rc < 0) {
			errs++;
			exitcode = rc;
		} else {
			nfound += rc;
		}
		i++;
	}
	if (i)
		v2pr_info("processed %d queries, with %d matches, %d errs\n",
			 i, nfound, errs);

	if (exitcode)
		return exitcode;
	return nfound;
}

/* apply a new class-param setting */
static int ddebug_apply_class_bitmap(const struct ddebug_class_param *dcp,
				     const u32 *new_bits, const u32 old_bits,
				     const char *query_modname)
{
#define QUERY_SIZE 128
	char query[QUERY_SIZE];
	const struct ddebug_class_map *map = dcp->map;
	int matches = 0;
	int bi, ct;

	if (*new_bits != old_bits)
		v2pr_info("apply bitmap: 0x%x to: 0x%x for %s\n", *new_bits,
			  old_bits, query_modname ?: "'*'");

	for (bi = 0; bi < map->length && bi < 32; bi++) {
		bool new_b = !!(*new_bits & BIT(bi));
		bool old_b = !!(old_bits & BIT(bi));

		if (new_b == old_b)
			continue;

		snprintf(query, QUERY_SIZE, "class %s %c%s", map->class_names[bi],
			 new_b ? '+' : '-', dcp->flags);

		ct = ddebug_exec_queries(query, query_modname);
		matches += ct;

		v2pr_info("bit_%d: %d matches on class: %s -> 0x%x\n", bi,
			  ct, map->class_names[bi], *new_bits);
	}
	if (*new_bits != old_bits)
		v2pr_info("applied bitmap: 0x%x to: 0x%x for %s\n", *new_bits,
			  old_bits, query_modname ?: "'*'");

	return matches;
}

/* stub to later conditionally add "$module." prefix where not already done */
#define KP_NAME(kp)	kp->name

#define CLASSMAP_BITMASK(width) ((width) >= 32 ? ~0U : (1U << (width)) - 1)

static void __maybe_unused ddebug_class_param_clamp_input(u32 *inrep, const struct kernel_param *kp)
{
	const struct ddebug_class_param *dcp = kp->arg;
	const struct ddebug_class_map *map = dcp->map;

	switch (map->map_type) {
	case DD_CLASS_TYPE_DISJOINT_BITS:
		/* expect bits. mask and warn if too many */
		if (*inrep & ~CLASSMAP_BITMASK(map->length)) {
			pr_warn("%s: input: 0x%x exceeds mask: 0x%x, masking\n",
				KP_NAME(kp), *inrep, CLASSMAP_BITMASK(map->length));
			*inrep &= CLASSMAP_BITMASK(map->length);
		}
		break;
	case DD_CLASS_TYPE_LEVEL_NUM:
		/* input is bitpos, of highest verbosity to be enabled */
		if (*inrep > map->length) {
			pr_warn("%s: level:%d exceeds max:%d, clamping\n",
				KP_NAME(kp), *inrep, map->length);
			*inrep = map->length;
		}
		break;
	}
}

/**
 * param_set_dyndbg_classes - class FOO >control
 * @instr: string echo>d to sysfs, input depends on map_type
 * @kp:    kp->arg has state: bits/lvl, map, map_type
 * @mod_name: module name or null for all modules with the classes
 *
 * Enable/disable prdbgs by their class, as given in the arguments to
 * DECLARE_DYNDBG_CLASSMAP.  For LEVEL map-types, enforce relative
 * levels by bitpos.
 *
 * Returns: 0 or <0 if error.
 */
static int param_set_dyndbg_module_classes(const char *instr,
					   const struct kernel_param *kp,
					   const char *mod_name)
{
	const struct ddebug_class_param *dcp = kp->arg;
	const struct ddebug_class_map *map = dcp->map;
	u32 inrep, new_bits, old_bits, old_val;
	int rc, totct = 0;

	rc = kstrtou32(instr, 0, &inrep);
	if (rc) {
		int len = strcspn(instr, "\n");

		pr_err("expecting numeric input, not: %.*s > %s\n",
		       len, instr, KP_NAME(kp));
		return -EINVAL;
	}
	ddebug_class_param_clamp_input(&inrep, kp);

	switch (map->map_type) {
	case DD_CLASS_TYPE_DISJOINT_BITS:
		old_val = READ_ONCE(*dcp->bits);
		v2pr_info("bits:0x%x > %s.%s\n", inrep, mod_name ?: "*", KP_NAME(kp));
		totct += ddebug_apply_class_bitmap(dcp, &inrep, old_val, mod_name);
		WRITE_ONCE(*dcp->bits, inrep);
		break;
	case DD_CLASS_TYPE_LEVEL_NUM:
		old_val = READ_ONCE(*dcp->lvl);
		old_bits = CLASSMAP_BITMASK(old_val);
		new_bits = CLASSMAP_BITMASK(inrep);
		v2pr_info("lvl:%u bits:0x%x > %s\n", inrep, new_bits, KP_NAME(kp));
		v2pr_info("lvl:%u bits:0x%x > %s\n", inrep, new_bits, KP_NAME(kp));
		totct += ddebug_apply_class_bitmap(dcp, &new_bits, old_bits, mod_name);
		WRITE_ONCE(*dcp->lvl, inrep);
		break;
	default:
		pr_warn("%s: bad map type: %d\n", KP_NAME(kp), map->map_type);
		return -EINVAL;
	}
	vpr_info("%s: total matches: %d\n", KP_NAME(kp), totct);
	return 0;
}

/**
 * param_set_dyndbg_classes - classmap-based kernel parameter setter
 * @instr: string value to set (numeric bitmask or level)
 * @kp:    kernel parameter info referencing classmap state
 *
 * Enable or disable all class'd pr_debug callsites in the classmap,
 * independent of the module they're in.
 *
 * Returns: 0 on success, or a negative error code.
 */
int param_set_dyndbg_classes(const char *instr, const struct kernel_param *kp)
{
	return param_set_dyndbg_module_classes(instr, kp, NULL);
}
EXPORT_SYMBOL(param_set_dyndbg_classes);

/**
 * param_get_dyndbg_classes - classmap kparam getter
 * @buffer: string description of controlled bits -> classes
 * @kp:     kp->arg has state: bits, map
 *
 * Reads last written state, underlying pr_debug states may have been
 * altered by direct >control.  Displays 0x for DISJOINT classmap
 * types, 0-N for LEVEL types.
 *
 * Returns: ct of chars written or <0 on error
 */
int param_get_dyndbg_classes(char *buffer, const struct kernel_param *kp)
{
	const struct ddebug_class_param *dcp = kp->arg;
	const struct ddebug_class_map *map = dcp->map;

	switch (map->map_type) {
	case DD_CLASS_TYPE_DISJOINT_BITS:
		return scnprintf(buffer, PAGE_SIZE, "0x%x\n", *dcp->bits);
	case DD_CLASS_TYPE_LEVEL_NUM:
		return scnprintf(buffer, PAGE_SIZE, "%u\n", *dcp->lvl);
	default:
		return -1;
	}
	return 0;
}
EXPORT_SYMBOL(param_get_dyndbg_classes);

const struct kernel_param_ops param_ops_dyndbg_classes = {
	.set = param_set_dyndbg_classes,
	.get = param_get_dyndbg_classes,
};
EXPORT_SYMBOL(param_ops_dyndbg_classes);

#define PREFIX_SIZE 128

static int remaining(int wrote)
{
	if (PREFIX_SIZE - wrote > 0)
		return PREFIX_SIZE - wrote;
	return 0;
}

static int __dynamic_emit_lookup(const struct _ddebug *desc, char *buf, int start)
{
	char *prefix;
	int pos = start;
	unsigned long key;

	if (!(desc->flags & _DPRINTK_FLAGS_INCL_LOOKUP))
		return pos;

	key = ddebug_prefix_key(desc);

	rcu_read_lock();
	prefix = (char *) mtree_load(&pr_prefixes, key);
	rcu_read_unlock();

	if (likely(prefix)) {
		pos += snprintf(buf + pos, remaining(pos), "%s", prefix);
		v4pr_info("using cached prefix: %s\n", prefix);
		return pos;
	}

	/*
	 * Cache miss (should only happen under extreme memory
	 * pressure where eager allocation failed, or during early
	 * boot if we didn't pre-fill).  Just resolve and format on
	 * the stack, but DO NOT allocate or write to the cache.
	 */
	{
		const char *mod = NULL, *file = NULL, *func = NULL;
		struct ddebug_table *dt;
		struct bonsai_tree *site_map = &dd_builtin_site_map;

		/* Locate the table for this descriptor to find its specific site_map */
		mutex_lock(&ddebug_lock);
		list_for_each_entry(dt, &ddebug_tables, link) {
			if (desc >= dt->info.descs.start &&
			    desc < dt->info.descs.start + dt->info.descs.len) {
				site_map = ddebug_get_site_map(dt);
				break;
			}
		}
		mutex_unlock(&ddebug_lock);

		ddebug_resolve_site(site_map, desc,
				    (desc->flags & _DPRINTK_FLAGS_INCL_MODNAME) ? &mod : NULL,
				    (desc->flags & _DPRINTK_FLAGS_INCL_SOURCENAME) ? &file : NULL,
				    (desc->flags & _DPRINTK_FLAGS_INCL_FUNCNAME) ? &func : NULL);

		if (mod)
			pos += snprintf(buf + pos, remaining(pos), "%s:", mod);
		if (func)
			pos += snprintf(buf + pos, remaining(pos), "%s:", func);
		if (file)
			pos += snprintf(buf + pos, remaining(pos), "%s:", trim_prefix(file));
	}
	if (desc->flags & _DPRINTK_FLAGS_INCL_LINENO)
		pos += snprintf(buf + pos, remaining(pos), "%d:",
				desc->lineno);
	if (remaining(pos)) {
		buf[pos++] = ' ';
		buf[pos] = '\0';
	}
	return pos;
}

static char *__dynamic_emit_prefix(const struct _ddebug *desc, char *buf)
{
	int pos = 0;

	if (desc->flags & _DPRINTK_FLAGS_INCL_TID) {
		if (in_interrupt())
			pos += snprintf(buf + pos, remaining(pos), "<intr> ");
		else
			pos += snprintf(buf + pos, remaining(pos), "[%d] ",
					task_pid_vnr(current));
	}

	if (unlikely(desc->flags & _DPRINTK_FLAGS_INCL_LOOKUP))
		pos += __dynamic_emit_lookup(desc, buf, pos);

	if (pos >= PREFIX_SIZE)
		buf[PREFIX_SIZE - 1] = '\0';

	return buf;
}

static inline char *dynamic_emit_prefix(struct _ddebug *desc, char *buf)
{
	if (unlikely(desc->flags & _DPRINTK_FLAGS_INCL_ANY))
		return __dynamic_emit_prefix(desc, buf);
	return buf;
}

void __dynamic_pr_debug(struct _ddebug *descriptor, const char *fmt, ...)
{
	va_list args;
	struct va_format vaf;
	char buf[PREFIX_SIZE] = "";

	BUG_ON(!descriptor);
	BUG_ON(!fmt);

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk(KERN_DEBUG "%s%pV", dynamic_emit_prefix(descriptor, buf), &vaf);

	va_end(args);
}
EXPORT_SYMBOL(__dynamic_pr_debug);

void __dynamic_dev_dbg(struct _ddebug *descriptor,
		      const struct device *dev, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	BUG_ON(!descriptor);
	BUG_ON(!fmt);

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	if (!dev) {
		printk(KERN_DEBUG "(NULL device *): %pV", &vaf);
	} else {
		char buf[PREFIX_SIZE] = "";

		dev_printk_emit(LOGLEVEL_DEBUG, dev, "%s%s %s: %pV",
				dynamic_emit_prefix(descriptor, buf),
				dev_driver_string(dev), dev_name(dev),
				&vaf);
	}

	va_end(args);
}
EXPORT_SYMBOL(__dynamic_dev_dbg);

#ifdef CONFIG_NET

void __dynamic_netdev_dbg(struct _ddebug *descriptor,
			  const struct net_device *dev, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	BUG_ON(!descriptor);
	BUG_ON(!fmt);

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	if (dev && dev->dev.parent) {
		char buf[PREFIX_SIZE] = "";

		dev_printk_emit(LOGLEVEL_DEBUG, dev->dev.parent,
				"%s%s %s %s%s: %pV",
				dynamic_emit_prefix(descriptor, buf),
				dev_driver_string(dev->dev.parent),
				dev_name(dev->dev.parent),
				netdev_name(dev), netdev_reg_state(dev),
				&vaf);
	} else if (dev) {
		printk(KERN_DEBUG "%s%s: %pV", netdev_name(dev),
		       netdev_reg_state(dev), &vaf);
	} else {
		printk(KERN_DEBUG "(NULL net_device): %pV", &vaf);
	}

	va_end(args);
}
EXPORT_SYMBOL(__dynamic_netdev_dbg);

#endif

#if IS_ENABLED(CONFIG_INFINIBAND)

void __dynamic_ibdev_dbg(struct _ddebug *descriptor,
			 const struct ib_device *ibdev, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	if (ibdev && ibdev->dev.parent) {
		char buf[PREFIX_SIZE] = "";

		dev_printk_emit(LOGLEVEL_DEBUG, ibdev->dev.parent,
				"%s%s %s %s: %pV",
				dynamic_emit_prefix(descriptor, buf),
				dev_driver_string(ibdev->dev.parent),
				dev_name(ibdev->dev.parent),
				dev_name(&ibdev->dev),
				&vaf);
	} else if (ibdev) {
		printk(KERN_DEBUG "%s: %pV", dev_name(&ibdev->dev), &vaf);
	} else {
		printk(KERN_DEBUG "(NULL ib_device): %pV", &vaf);
	}

	va_end(args);
}
EXPORT_SYMBOL(__dynamic_ibdev_dbg);

#endif

/*
 * Install a noop handler to make dyndbg look like a normal kernel cli param.
 * This avoids warnings about dyndbg being an unknown cli param when supplied
 * by a user.
 */
static __init int dyndbg_setup(char *str)
{
	return 1;
}

__setup("dyndbg=", dyndbg_setup);

static void reset_ddebug_call_count(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		per_cpu(ddebug_call_count, cpu) = 0;
}

/*
 * File_ops->write method for <debugfs>/dynamic_debug/control.  Gathers the
 * command text from userspace, parses and executes it.
 */
#define USER_BUF_PAGE 4096
static int ddebug_load_zstd_metadata(const void *src, size_t src_len);

static ssize_t ddebug_proc_write(struct file *file, const char __user *ubuf,
				  size_t len, loff_t *offp)
{
	u32 magic;
	char *tmpbuf;
	int ret;

	if (len == 0)
		return 0;

	/* Detect binary ZSTD metadata stream */
	if (len >= sizeof(magic) &&
	    !copy_from_user(&magic, ubuf, sizeof(magic)) &&
	    magic == ZSTD_MAGICNUMBER) {
		void *zbuf = vmemdup_user(ubuf, len);
		if (IS_ERR(zbuf))
			return PTR_ERR(zbuf);

		ret = ddebug_load_zstd_metadata(zbuf, len);
		kvfree(zbuf);
		if (ret < 0)
			return ret;
		*offp += len;
		return len;
	}

	if (len > USER_BUF_PAGE - 1) {
		pr_warn("expected <%d bytes into control\n", USER_BUF_PAGE);
		return -E2BIG;
	}
	tmpbuf = memdup_user_nul(ubuf, len);
	if (IS_ERR(tmpbuf))
		return PTR_ERR(tmpbuf);
	v2pr_info("read %zu bytes from userspace\n", len);

	if (len >= 11 && !strncmp(tmpbuf, "reset_stats", 11)) {
		reset_ddebug_call_count();
		return len;
	}
	ret = ddebug_exec_queries(tmpbuf, NULL);
	kfree(tmpbuf);
	if (ret < 0)
		return ret;

	*offp += len;
	return len;
}

/*
 * Set the iterator to point to the first _ddebug object
 * and return a pointer to that first object.  Returns
 * NULL if there are no _ddebugs at all.
 */
static struct _ddebug *ddebug_iter_first(struct ddebug_iter *iter)
{
	if (list_empty(&ddebug_tables)) {
		iter->table = NULL;
		return NULL;
	}
	iter->table = list_entry(ddebug_tables.next,
				 struct ddebug_table, link);
	iter->idx = iter->table->info.descs.len;
	return &iter->table->info.descs.start[--iter->idx];
}

/*
 * Advance the iterator to point to the next _ddebug
 * object from the one the iterator currently points at,
 * and returns a pointer to the new _ddebug.  Returns
 * NULL if the iterator has seen all the _ddebugs.
 */
static struct _ddebug *ddebug_iter_next(struct ddebug_iter *iter)
{
	if (iter->table == NULL)
		return NULL;
	if (--iter->idx < 0) {
		/* iterate to next table */
		if (list_is_last(&iter->table->link, &ddebug_tables)) {
			iter->table = NULL;
			return NULL;
		}
		iter->table = list_entry(iter->table->link.next,
					 struct ddebug_table, link);
		iter->idx = iter->table->info.descs.len;
		--iter->idx;
	}
	return &iter->table->info.descs.start[iter->idx];
}

/*
 * Seq_ops start method.  Called at the start of every
 * read() call from userspace.  Takes the ddebug_lock and
 * seeks the seq_file's iterator to the given position.
 */
static void *ddebug_proc_start(struct seq_file *m, loff_t *pos)
{
	struct ddebug_iter *iter = m->private;
	struct _ddebug *dp;
	int n = *pos;

	mutex_lock(&ddebug_lock);

	if (!n)
		return SEQ_START_TOKEN;
	if (n < 0)
		return NULL;
	dp = ddebug_iter_first(iter);
	while (dp != NULL && --n > 0)
		dp = ddebug_iter_next(iter);
	return dp;
}

/*
 * Seq_ops next method.  Called several times within a read()
 * call from userspace, with ddebug_lock held.  Walks to the
 * next _ddebug object with a special case for the header line.
 */
static char ddebug_epilogue_token;
#define EPILOGUE_TOKEN (&ddebug_epilogue_token)

static void *ddebug_proc_next(struct seq_file *m, void *p, loff_t *pos)
{
	struct ddebug_iter *iter = m->private;
	struct _ddebug *dp;

	(*pos)++;

	if (p == EPILOGUE_TOKEN)
		return NULL;

	if (p == SEQ_START_TOKEN)
		dp = ddebug_iter_first(iter);
	else
		dp = ddebug_iter_next(iter);

	if (dp)
		return dp;

	return EPILOGUE_TOKEN;
}

static bool ddebug_class_map_in_range(const int class_id, const struct ddebug_class_map *map)
{
	if (!map)
		return false;
	return (class_id >= map->base &&
		class_id < map->base + map->length);
}

static bool ddebug_class_user_in_range(const int class_id, const struct ddebug_class_user *user)
{
	if (!user)
		return false;
	return ddebug_class_map_in_range(class_id - user->offset, user->map);
}
static const char *ddebug_class_name(struct _ddebug_info *di, struct _ddebug *dp)
{
	struct ddebug_class_map *map;
	struct ddebug_class_user *cli;
	int i;

	for_subvec(i, map, di, maps)
		if (ddebug_class_map_in_range(dp->class_id, map))
			return map->class_names[dp->class_id - map->base];

	for_subvec(i, cli, di, users)
		if (ddebug_class_user_in_range(dp->class_id, cli))
			return cli->map->class_names[dp->class_id - cli->map->base - cli->offset];

	return NULL;
}

static unsigned long get_ddebug_call_count(void)
{
	unsigned long total = 0;
	int cpu;

	for_each_online_cpu(cpu)
		total += per_cpu(ddebug_call_count, cpu);
	return total;
}

/*
 * Seq_ops show method.  Called several times within a read()
 * call from userspace, with ddebug_lock held.  Formats the
 * current _ddebug as a single human-readable line, with a
 * special case for the header line.
 */
static int ddebug_proc_show(struct seq_file *m, void *p)
{
	struct ddebug_iter *iter = m->private;
	struct _ddebug *dp = p;
	struct flagsbuf flags;
	char const *class, *filename, *function, *modname;

	if (p == SEQ_START_TOKEN) {
		seq_puts(m,
			 "# filename:lineno [module]function flags format\n");
		return 0;
	}
	if (p == EPILOGUE_TOKEN) {
		seq_printf(m, "#: cached_prefixes=%u\n", pr_prefixes_count);
		seq_printf(m, "#: total call-counts: %lu\n",
			   get_ddebug_call_count());
		return 0;
	}

	modname = NULL;
	ddebug_resolve_site(ddebug_get_site_map(iter->table), dp, &modname, &filename, &function);
	if (!modname)
		modname = iter->table->info.mod_name;

	seq_printf(m, "%s:%u [%s]%s =%s \"",
		   trim_prefix(filename), dp->lineno,
		   modname, function,
		   ddebug_describe_flags(dp->flags, &flags));
	seq_escape_str(m, dp->format, ESCAPE_SPACE, "\t\r\n\"");
	seq_putc(m, '"');

	if (dp->class_id != _DPRINTK_CLASS_DFLT) {
		class = ddebug_class_name(&iter->table->info, dp);
		if (class)
			seq_printf(m, " class:%s", class);
		else
			seq_printf(m, " class:_UNKNOWN_ _id:%d", dp->class_id);
	}
	seq_putc(m, '\n');

	return 0;
}

/*
 * Seq_ops stop method.  Called at the end of each read()
 * call from userspace.  Drops ddebug_lock.
 */
static void ddebug_proc_stop(struct seq_file *m, void *p)
{
	mutex_unlock(&ddebug_lock);
}

static const struct seq_operations ddebug_proc_seqops = {
	.start = ddebug_proc_start,
	.next = ddebug_proc_next,
	.show = ddebug_proc_show,
	.stop = ddebug_proc_stop
};

static int ddebug_proc_open(struct inode *inode, struct file *file)
{
	return seq_open_private(file, &ddebug_proc_seqops,
				sizeof(struct ddebug_iter));
}

static const struct file_operations ddebug_proc_fops = {
	.owner = THIS_MODULE,
	.open = ddebug_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release_private,
	.write = ddebug_proc_write
};

static const struct proc_ops proc_fops = {
	.proc_open = ddebug_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release_private,
	.proc_write = ddebug_proc_write
};

#define vpr_cm_info(cm_p, msg_fmt, ...) ({				\
	struct ddebug_class_map const *_cm = cm_p;			\
	v2pr_info(msg_fmt "%s [%d..%d] %s..%s\n", ##__VA_ARGS__,	\
		  _cm->mod_name, _cm->base, _cm->base + _cm->length,	\
		  _cm->class_names[0], _cm->class_names[_cm->length - 1]); \
	})

/*
 * Modules which define classmaps get them initialized by
 * param-callback via module.c:parse_one.  Modules which use other's
 * classmaps must be initialized explicitly.
 */
static inline u32 ddebug_class_param_to_bits(const struct ddebug_class_param *dcp)
{
        const struct ddebug_class_map *map = dcp->map;
        switch (map->map_type) {
        case DD_CLASS_TYPE_DISJOINT_BITS:
		return *dcp->bits & CLASSMAP_BITMASK(map->length);
        case DD_CLASS_TYPE_LEVEL_NUM:
		return CLASSMAP_BITMASK(min_t(u32, *dcp->lvl, map->length));
        default:
		return 0;
        }
}



/* called for class-users only, parse_one does this for definer modules */
static void ddebug_sync_classbits(const struct kernel_param *kp, const char *modname)
{
	const struct ddebug_class_param *dcp = kp->arg;
	u32 val, new_bits;

	if (!dcp || !dcp->map)
		return;

	switch (dcp->map->map_type) {
	case DD_CLASS_TYPE_DISJOINT_BITS:
		val = READ_ONCE(*dcp->bits);
		new_bits = val;
		v2pr_info("  %s: classbits: 0x%x\n", KP_NAME(kp), new_bits);
		ddebug_apply_class_bitmap(dcp, &new_bits, 0UL, modname);
		break;
	case DD_CLASS_TYPE_LEVEL_NUM:
		val = READ_ONCE(*dcp->lvl);
		new_bits = CLASSMAP_BITMASK(val);
		v2pr_info("  %s: lvl:%d bits:0x%x\n", KP_NAME(kp), val, new_bits);
		ddebug_apply_class_bitmap(dcp, &new_bits, 0UL, modname);
		break;
	default:
		pr_err("bad map type %d\n", dcp->map->map_type);
		return;
	}
}

static struct ddebug_class_param *
ddebug_get_classmap_kparam(const struct kernel_param *kp,
			   const struct ddebug_class_map *map)
{
	struct ddebug_class_param *dcp;

	if (kp->ops != &param_ops_dyndbg_classes)
		return NULL;

	dcp = (struct ddebug_class_param *)kp->arg;
	return (map == dcp->map)
		? dcp : (struct ddebug_class_param *)NULL;
}

static void ddebug_match_apply_kparam(const struct kernel_param *kp,
				      struct ddebug_class_map *map,
				      const char *mod_name)
{
	struct ddebug_class_param *dcp = ddebug_get_classmap_kparam(kp, map);

	if (dcp && dcp->map == map) {
		v2pr_info(" kp:%s.%s =0x%x", mod_name, kp->name, *dcp->bits);
		vpr_cm_info(map, " %s maps ", mod_name);
		ddebug_sync_classbits(kp, mod_name);
	}
}

static void ddebug_apply_params(struct ddebug_class_map *cm, const char *mod_name)
{
	const struct kernel_param *kp;

	if (!cm)
		return;
#if IS_ENABLED(CONFIG_MODULES)
	int i;

	if (cm->mod) {
		vpr_cm_info(cm, "loaded classmap: %s ", mod_name);
		/* ifdef protects the cm->mod->kp deref */
		for (i = 0, kp = cm->mod->kp; i < cm->mod->num_kp; i++, kp++)
			ddebug_match_apply_kparam(kp, cm, mod_name);
	}
#endif
	if (!cm->mod) {
		vpr_cm_info(cm, "builtin classmap: %s ", mod_name);
		for (kp = __start___param; kp < __stop___param; kp++)
			ddebug_match_apply_kparam(kp, cm, mod_name);
	}
}

#if 0
/*
 * called from add_module, ie early. it can find controlling kparams,
 * which can/does? enable protection of this classmap from class-less
 * queries, on the grounds that the user created the kparam, means to
 * use it, and expects it to reflect reality.  We should oblige him,
 * and protect those classmaps from classless "-p" changes.
 */
static void ddebug_apply_class_maps(const struct _ddebug_info *di)
{
	struct ddebug_class_map *cm;
	int i;

	for_subvec(i, cm, di, maps)
		ddebug_apply_params(cm, cm->mod_name);

	v2pr_di_info(di, "attached %d class-maps to ", i);
}
#endif

static void ddebug_apply_class_users(const struct _ddebug_info *di)
{
	struct ddebug_class_user *cli;
	int i;

	for_subvec(i, cli, di, users)
		ddebug_apply_params(cli->map, cli->mod_name);

	v2pr_di_info(di, "attached %d class-users to ", i);
}

/*
 * dd_set_module_subrange - find matching subrange of classmaps
 * @_i:   caller-provided index var
 * @_sp:  cursor into @_vec
 * @_di:  pointer to the struct _ddebug_info to be narrowed
 * @_vec: name of the vector member (must have .start and .len)
 *
 * Narrow a _ddebug_info's vector (@_vec) of classmaps to the
 * contiguous subrange of elements where ->mod_name matches
 * @__di->mod_name.  This is primarily for builtins, loadable modules
 * have only their classmaps, and dont need this sub-selection.
 */
#define dd_set_module_subrange(_i, _sp, _di, _vec) ({			\
	struct _ddebug_info *__di = (_di);				\
	typeof(__di->_vec.start) __start = NULL;			\
	int __nc = 0;							\
	for_subvec(_i, _sp, __di, _vec) {				\
		if (!strcmp((_sp)->mod_name, __di->mod_name)) {		\
			if (!__nc++)					\
				__start = (_sp);			\
		} else if (__nc) {					\
			break; /* end of consecutive matches */		\
		}							\
	}								\
	__di->_vec.len = __nc;						\
	if (__nc)							\
		__di->_vec.start = __start;				\
})

static int ddebug_class_range_overlap(struct ddebug_class_map *cm, u64 *reserved_ids)
{
	u64 range = (((1ULL << cm->length) - 1) << cm->base);

	if (range & *reserved_ids) {
		pr_err("[%d..%d] on %s conflicts with %llx\n", cm->base,
		       cm->base + cm->length - 1, cm->class_names[0],
		       *reserved_ids);
		return -EINVAL;
	}
	*reserved_ids |= range;
	return 0;
}

static int ddebug_class_user_overlap(struct ddebug_class_user *cli,
				     u64 *reserved_ids)
{
	struct ddebug_class_map *cm = cli->map;
	int base = cm->base + cli->offset;
	u64 range = (((1ULL << cm->length) - 1) << base);

	if (range & *reserved_ids) {
		pr_err("module %s: [%d..%d] (from %s) conflicts with %llx\n",
		       cli->mod_name, base, base + cm->length - 1,
		       cm->class_names[0], *reserved_ids);
		return -EINVAL;
	}
	*reserved_ids |= range;
	return 0;
}



static void *ddebug_zstd_alloc(void *opaque, size_t size)
{
	return kvmalloc(size, GFP_KERNEL);
}

static void ddebug_zstd_free(void *opaque, void *address)
{
	kvfree(address);
}

static const ZSTD_customMem ddebug_zstd_mem = {
	.customAlloc = ddebug_zstd_alloc,
	.customFree = ddebug_zstd_free,
	.opaque = NULL,
};

#define DDEBUG_META_MAGIC 0x44594E44 /* 'DYND' */
#define DDEBUG_META_VERSION 2
#define ZSTD_MAGICNUMBER  0xFD2FB528

struct _ddebug_metadata_header {
	u32 magic;          /* DDEBUG_META_MAGIC */
	u16 version;        /* DDEBUG_META_VERSION (2) */
	u16 flags;          /* reserved / flags */
	u32 desc_count;     /* Total descriptors represented */
	u32 interval_count; /* Number of condensed interval records */
	u32 strings_len;    /* Byte size of string pool */
	u8  build_id[20];   /* Optional ELF .note.gnu.build-id */
	u8  reserved[8];    /* Alignment padding to 48 bytes */
};

struct _ddebug_interval_record {
	u16 start_idx;      /* 0-indexed descriptor start */
	u16 end_idx;        /* 0-indexed descriptor end (inclusive) */
	u8  tag;            /* DD_TAG_MOD (0), DD_TAG_FILE (1), DD_TAG_FUNC (2) */
	u8  reserved;
	u32 string_offset;  /* Byte offset into string pool */
};

struct ddebug_string_pool {
	char *buf;
	unsigned int len;
	unsigned int cap;
};

static unsigned int ddebug_pool_add_string(struct ddebug_string_pool *pool, const char *str)
{
	unsigned int offset, slen;

	if (!str)
		str = "";
	slen = strlen(str) + 1;

	/* Simple deduplication scan */
	for (offset = 0; offset < pool->len; ) {
		if (strcmp(pool->buf + offset, str) == 0)
			return offset;
		offset += strlen(pool->buf + offset) + 1;
	}

	if (pool->len + slen > pool->cap) {
		unsigned int new_cap = max(pool->cap * 2, pool->len + slen + 4096);
		char *new_buf = kvmalloc(new_cap, GFP_KERNEL);
		if (!new_buf)
			return 0;
		if (pool->buf) {
			memcpy(new_buf, pool->buf, pool->len);
			kvfree(pool->buf);
		}
		pool->buf = new_buf;
		pool->cap = new_cap;
	}

	offset = pool->len;
	memcpy(pool->buf + offset, str, slen);
	pool->len += slen;
	return offset;
}

static int ddebug_hydrate_intervals(const void *src, size_t src_len,
				    struct _ddebug *descs, unsigned int desc_count,
				    struct bonsai_tree *bt)
{
	unsigned long long uncomp_sz;
	struct _ddebug_metadata_header *hdr;
	const struct _ddebug_interval_record *intervals;
	const char *strings_base;
	char *permanent_strings = NULL;
	ZSTD_DCtx *dctx;
	void *uncomp_buf;
	size_t dlen;
	unsigned int i;

	if (src_len < 4)
		return -EINVAL;

	uncomp_sz = ZSTD_getFrameContentSize(src, src_len);
	if (uncomp_sz == ZSTD_CONTENTSIZE_UNKNOWN || uncomp_sz == ZSTD_CONTENTSIZE_ERROR)
		return -EINVAL;
	if (uncomp_sz < sizeof(*hdr))
		return -EINVAL;

	dctx = ZSTD_createDCtx_advanced(ddebug_zstd_mem);
	if (!dctx)
		return -ENOMEM;

	uncomp_buf = kvmalloc(uncomp_sz, GFP_KERNEL);
	if (!uncomp_buf) {
		ZSTD_freeDCtx(dctx);
		return -ENOMEM;
	}

	dlen = ZSTD_decompressDCtx(dctx, uncomp_buf, uncomp_sz, src, src_len);
	ZSTD_freeDCtx(dctx);

	if (ZSTD_isError(dlen) || dlen < sizeof(*hdr)) {
		kvfree(uncomp_buf);
		return -EINVAL;
	}

	hdr = (struct _ddebug_metadata_header *)uncomp_buf;
	if (hdr->magic != DDEBUG_META_MAGIC || hdr->version != DDEBUG_META_VERSION) {
		kvfree(uncomp_buf);
		return -EINVAL;
	}

	if (hdr->desc_count != desc_count) {
		kvfree(uncomp_buf);
		return -EINVAL;
	}

	intervals = (const struct _ddebug_interval_record *)(uncomp_buf + sizeof(*hdr));
	strings_base = (const char *)uncomp_buf + sizeof(*hdr) +
		       hdr->interval_count * sizeof(struct _ddebug_interval_record);

	if (hdr->strings_len > 0) {
		permanent_strings = kvmemdup(strings_base, hdr->strings_len, GFP_KERNEL);
		if (!permanent_strings) {
			kvfree(uncomp_buf);
			return -ENOMEM;
		}
		strings_base = permanent_strings;
	}

	if (!bt->num_segments)
		bonsai_init(bt, 0, GFP_KERNEL);

	/* Single linear pass pouring intervals into Bonsai tree */
	for (i = 0; i < hdr->interval_count; i++) {
		const struct _ddebug_interval_record *rec = &intervals[i];
		const char *str;
		unsigned long start_k, end_k;

		if (rec->start_idx >= desc_count || rec->end_idx >= desc_count)
			continue;
		if (rec->string_offset >= hdr->strings_len)
			continue;

		str = strings_base + rec->string_offset;
		start_k = ddebug_site_tag_key((unsigned long)&descs[rec->start_idx], rec->tag);
		end_k   = ddebug_site_tag_key((unsigned long)&descs[rec->end_idx],   rec->tag);
		bonsai_store_range(bt, start_k, end_k, (void *)str, GFP_KERNEL);
	}

	bonsai_seal(bt);
	kvfree(uncomp_buf);
	return 0;
}

static int ddebug_reconstruct_site_map(struct ddebug_table *dt)
{
	struct _ddebug *descs;
	unsigned int desc_count;
	void *compressed_buf;
	unsigned long compressed_len;
	struct bonsai_tree *bt;
	bool is_builtin = !dt;

	if (is_builtin) {
		if (dd_builtin_site_map.root_idx)
			return 0;
		if (!dd_builtin_compressed_sites)
			return -ENODATA;
		bt = &dd_builtin_site_map;
		descs = __start___dyndbg_descs;
		desc_count = __stop___dyndbg_descs - __start___dyndbg_descs;
		compressed_buf = dd_builtin_compressed_sites;
		compressed_len = dd_builtin_compressed_len;
	} else {
		if (dt->site_map.root_idx)
			return 0;
		if (!dt->compressed_sites)
			return 0;
		bt = &dt->site_map;
		descs = dt->info.descs.start;
		desc_count = dt->info.descs.len;
		compressed_buf = dt->compressed_sites;
		compressed_len = dt->compressed_len;
	}

	return ddebug_hydrate_intervals(compressed_buf, compressed_len,
					descs, desc_count, bt);
}

static int ddebug_load_zstd_metadata(const void *src, size_t src_len)
{
	struct _ddebug *descs = __start___dyndbg_descs;
	unsigned int desc_count = __stop___dyndbg_descs - __start___dyndbg_descs;
	int ret;

	mutex_lock(&ddebug_lock);
	if (dd_builtin_site_map.num_segments > 0) {
		mutex_unlock(&ddebug_lock);
		return -EEXIST;
	}

	ret = ddebug_hydrate_intervals(src, src_len, descs, desc_count,
				       &dd_builtin_site_map);
	if (ret) {
		mutex_unlock(&ddebug_lock);
		return ret;
	}

	kvfree(dd_builtin_compressed_sites);
	dd_builtin_compressed_sites = kvmemdup(src, src_len, GFP_KERNEL);
	dd_builtin_compressed_len = src_len;

	mutex_unlock(&ddebug_lock);

	vpr_info("ingested external site metadata: %u descs, %zu compressed bytes -> %u nodes, %u segments (%u KiB)\n",
		 desc_count, src_len, dd_builtin_site_map.node_count,
		 dd_builtin_site_map.num_segments,
		 (unsigned int)(dd_builtin_site_map.num_segments * (BONSAI_SEG_SIZE >> 10)));

	return 0;
}

static int ddebug_condense_and_compress_sites(struct _ddebug_info *di, struct bonsai_tree *bt,
					     void **out_zbuf, unsigned long *out_zlen)
{
	struct _ddebug *p = di->descs.start;
	const struct _ddebug_site *site = di->sites.start;
	unsigned int len = di->sites.len;
	struct ddebug_string_pool pool = { 0 };
	struct _ddebug_interval_record *records;
	unsigned int max_records = len * 3 + 16;
	unsigned int rec_count = 0;
	unsigned int i, start, offset;
	unsigned long total_raw_sz, max_dst_sz;
	struct _ddebug_metadata_header *hdr;
	ZSTD_CCtx *cctx;
	void *uncomp_payload, *compressed_payload;
	size_t clen;

	if (!len)
		return 0;

	records = kvmalloc_array(max_records, sizeof(*records), GFP_KERNEL);
	if (!records)
		return -ENOMEM;

	if (!bt->num_segments)
		bonsai_init(bt, 0, GFP_KERNEL);

	/* 1. Consolidate and insert Module ranges */
	start = 0;
	for (i = 1; i < len; i++) {
		if (site[i]._modname != site[start]._modname &&
		    strcmp(site[i]._modname, site[start]._modname) != 0) {
			offset = ddebug_pool_add_string(&pool, site[start]._modname);
			records[rec_count++] = (struct _ddebug_interval_record){
				.start_idx = start,
				.end_idx = i - 1,
				.tag = DD_TAG_MOD,
				.string_offset = offset,
			};
			bonsai_store_range(bt,
					   ddebug_site_tag_key((unsigned long)&p[start], DD_TAG_MOD),
					   ddebug_site_tag_key((unsigned long)&p[i - 1], DD_TAG_MOD),
					   (void *)site[start]._modname, GFP_KERNEL);
			start = i;
		}
	}
	offset = ddebug_pool_add_string(&pool, site[start]._modname);
	records[rec_count++] = (struct _ddebug_interval_record){
		.start_idx = start,
		.end_idx = len - 1,
		.tag = DD_TAG_MOD,
		.string_offset = offset,
	};
	bonsai_store_range(bt,
			   ddebug_site_tag_key((unsigned long)&p[start], DD_TAG_MOD),
			   ddebug_site_tag_key((unsigned long)&p[len - 1], DD_TAG_MOD),
			   (void *)site[start]._modname, GFP_KERNEL);

	/* 2. Consolidate and insert File ranges */
	start = 0;
	for (i = 1; i < len; i++) {
		if (site[i]._filename != site[start]._filename &&
		    strcmp(site[i]._filename, site[start]._filename) != 0) {
			offset = ddebug_pool_add_string(&pool, site[start]._filename);
			records[rec_count++] = (struct _ddebug_interval_record){
				.start_idx = start,
				.end_idx = i - 1,
				.tag = DD_TAG_FILE,
				.string_offset = offset,
			};
			bonsai_store_range(bt,
					   ddebug_site_tag_key((unsigned long)&p[start], DD_TAG_FILE),
					   ddebug_site_tag_key((unsigned long)&p[i - 1], DD_TAG_FILE),
					   (void *)site[start]._filename, GFP_KERNEL);
			start = i;
		}
	}
	offset = ddebug_pool_add_string(&pool, site[start]._filename);
	records[rec_count++] = (struct _ddebug_interval_record){
		.start_idx = start,
		.end_idx = len - 1,
		.tag = DD_TAG_FILE,
		.string_offset = offset,
	};
	bonsai_store_range(bt,
			   ddebug_site_tag_key((unsigned long)&p[start], DD_TAG_FILE),
			   ddebug_site_tag_key((unsigned long)&p[len - 1], DD_TAG_FILE),
			   (void *)site[start]._filename, GFP_KERNEL);

	/* 3. Consolidate and insert Function ranges */
	start = 0;
	for (i = 1; i < len; i++) {
		if (site[i]._function != site[start]._function &&
		    strcmp(site[i]._function, site[start]._function) != 0) {
			offset = ddebug_pool_add_string(&pool, site[start]._function);
			records[rec_count++] = (struct _ddebug_interval_record){
				.start_idx = start,
				.end_idx = i - 1,
				.tag = DD_TAG_FUNC,
				.string_offset = offset,
			};
			bonsai_store_range(bt,
					   ddebug_site_tag_key((unsigned long)&p[start], DD_TAG_FUNC),
					   ddebug_site_tag_key((unsigned long)&p[i - 1], DD_TAG_FUNC),
					   (void *)site[start]._function, GFP_KERNEL);
			start = i;
		}
	}
	offset = ddebug_pool_add_string(&pool, site[start]._function);
	records[rec_count++] = (struct _ddebug_interval_record){
		.start_idx = start,
		.end_idx = len - 1,
		.tag = DD_TAG_FUNC,
		.string_offset = offset,
	};
	bonsai_store_range(bt,
			   ddebug_site_tag_key((unsigned long)&p[start], DD_TAG_FUNC),
			   ddebug_site_tag_key((unsigned long)&p[len - 1], DD_TAG_FUNC),
			   (void *)site[start]._function, GFP_KERNEL);

	bonsai_seal(bt);

	/* Build canonical uncompressed payload */
	total_raw_sz = sizeof(*hdr) + rec_count * sizeof(*records) + pool.len;
	uncomp_payload = kvmalloc(total_raw_sz, GFP_KERNEL);
	if (!uncomp_payload) {
		kvfree(records);
		kvfree(pool.buf);
		return -ENOMEM;
	}

	hdr = (struct _ddebug_metadata_header *)uncomp_payload;
	*hdr = (struct _ddebug_metadata_header){
		.magic = DDEBUG_META_MAGIC,
		.version = DDEBUG_META_VERSION,
		.desc_count = len,
		.interval_count = rec_count,
		.strings_len = pool.len,
	};

	memcpy(uncomp_payload + sizeof(*hdr), records, rec_count * sizeof(*records));
	if (pool.len > 0)
		memcpy(uncomp_payload + sizeof(*hdr) + rec_count * sizeof(*records), pool.buf, pool.len);

	kvfree(records);
	kvfree(pool.buf);

	/* Compress payload */
	max_dst_sz = ZSTD_compressBound(total_raw_sz);
	cctx = ZSTD_createCCtx_advanced(ddebug_zstd_mem);
	compressed_payload = kvmalloc(max_dst_sz, GFP_KERNEL);
	if (!cctx || !compressed_payload) {
		if (cctx) ZSTD_freeCCtx(cctx);
		kvfree(compressed_payload);
		kvfree(uncomp_payload);
		return -ENOMEM;
	}

	clen = ZSTD_compressCCtx(cctx, compressed_payload, max_dst_sz, uncomp_payload, total_raw_sz, 3);
	ZSTD_freeCCtx(cctx);
	kvfree(uncomp_payload);

	if (ZSTD_isError(clen)) {
		kvfree(compressed_payload);
		return -EINVAL;
	}

	*out_zbuf = kvmemdup(compressed_payload, clen, GFP_KERNEL);
	*out_zlen = clen;
	kvfree(compressed_payload);

	v3pr_info("condensed and compressed %s metadata: %u intervals, %lu bytes -> %zu bytes\n",
		  di->mod_name ? di->mod_name : "builtin", rec_count, total_raw_sz, clen);

	return 0;
}

/*
 * Allocate a new ddebug_table for the given module
 * and add it to the global list.
 */
static int ddebug_add_module(struct _ddebug_info *di)
{
	struct ddebug_table *dt;
	struct ddebug_class_map *cm;
	struct ddebug_class_user *cli;
	u64 reserved_ids = 0;
	u64 bad_ids = 0;
	int i, err = 0;

	if (!di->descs.len)
		return 0;

	v3pr_info("add-module: %s %d sites\n", di->mod_name, di->descs.len);

	dt = kzalloc_obj(*dt);
	if (dt == NULL) {
		pr_err("error adding module: %s\n", di->mod_name);
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&dt->link);
	dt->info = *di;

	/*
	 * Built-in modules (which have di->sites.len == 0 here because they
	 * were condensed globally in dynamic_debug_init) will leave site_map empty.
	 * Loadable modules get their own dedicated site_map tree.
	 */
	if (dt->info.sites.len) {
		ddebug_condense_and_compress_sites(&dt->info, &dt->site_map,
						   &dt->compressed_sites, &dt->compressed_len);
		dt->info.sites.len = 0;
	}

	dd_set_module_subrange(i, cm, &dt->info, maps);
	dd_set_module_subrange(i, cli, &dt->info, users);

	/* validate the per-module shared 0..62 class_id space */
	for_subvec(i, cm, &dt->info, maps)
		if (ddebug_class_range_overlap(cm, &reserved_ids))
			err = -EINVAL;

	for_subvec(i, cli, &dt->info, users) {
		cm = cli->map;
		if (!cm) {
			pr_err("module %s: classmap not found for user\n", di->mod_name);
			err = -EINVAL;
			continue;
		}

		if (cm->base + cm->length + cli->offset >= _DPRINTK_CLASS_DFLT) {
			pr_err("module %s: base:%d + classes.len:%d + cli.offset:%d must be < %d\n",
			       di->mod_name, cm->base, cm->length,
			       cli->offset, _DPRINTK_CLASS_DFLT);
			err = -EINVAL;
			continue;
		}

		if (ddebug_class_user_overlap(cli, &reserved_ids))
			err = -EINVAL;
	}
	if (err)
		goto cleanup;

	/* validate all class_ids against module's classmaps/users */
	for (i = 0; i < dt->info.descs.len; i++) {
		struct _ddebug *dp = &dt->info.descs.start[i];

		if (dp->class_id == _DPRINTK_CLASS_DFLT)
			continue;
		if (bad_ids & (1ULL << dp->class_id))
			continue;
		if (!ddebug_find_map_by_class_id(&dt->info, dp->class_id)) {
			pr_warn("module %s uses unknown class_id %d\n",
				dt->info.mod_name, dp->class_id);
			bad_ids |= (1ULL << dp->class_id);
		}
	}

	mutex_lock(&ddebug_lock);
	list_add_tail(&dt->link, &ddebug_tables);
	mutex_unlock(&ddebug_lock);
	if (dt->info.users.len)
		ddebug_apply_class_users(&dt->info);

	vpr_info("%3u debug prints in module %s\n",
		 dt->info.descs.len, dt->info.mod_name);
	return 0;
cleanup:
	pr_err("dyndbg multi-classmap conflict in %s\n", di->mod_name);
	kfree(dt);
	return -EINVAL;
}

/* helper for ddebug_dyndbg_(boot|module)_param_cb */
static int ddebug_dyndbg_param_cb(char *param, char *val,
				const char *modname, int on_err)
{
	char *sep;

	sep = strchr(param, '.');
	if (sep) {
		/* needed only for ddebug_dyndbg_boot_param_cb */
		*sep = '\0';
		modname = param;
		param = sep + 1;
	}
	if (strcmp(param, "dyndbg"))
		return on_err; /* determined by caller */

	ddebug_exec_queries((val ? val : "+p"), modname);

	return 0; /* query failure shouldn't stop module load */
}

/* handle both dyndbg and $module.dyndbg params at boot */
static int ddebug_dyndbg_boot_param_cb(char *param, char *val,
				const char *unused, void *arg)
{
	vpr_info("%s=\"%s\"\n", param, val);
	return ddebug_dyndbg_param_cb(param, val, NULL, 0);
}

/*
 * modprobe foo finds foo.params in boot-args, strips "foo.", and
 * passes them to load_module().  This callback gets unknown params,
 * processes dyndbg params, rejects others.
 */
int ddebug_dyndbg_module_param_cb(char *param, char *val, const char *module)
{
	vpr_info("module: %s %s=\"%s\"\n", module, param, val);
	return ddebug_dyndbg_param_cb(param, val, module, -ENOENT);
}

static void ddebug_table_free(struct ddebug_table *dt)
{
	list_del_init(&dt->link);
	kfree(dt);
}

#ifdef CONFIG_MODULES

/*
 * clear the maple tree containing __dyndbg_sites info of their
 * contents for a module being rmmod'd.
 */
static void ddebug_module_sites_clear(struct ddebug_table *dt)
{
	if (!dt->site_map.num_segments)
		return; /* Built-in modules share the global tree */

	v2pr_info("clearing %3d debugs of removed module %s\n",
		  dt->info.descs.len, dt->info.mod_name);

	/* Safely destroy the isolated per-module site map and free the pot memory */
	bonsai_destroy(&dt->site_map);
}

/*
 * Called in response to a module being unloaded.  Removes
 * any ddebug_table's which point at the module.
 */
static int ddebug_remove_module(const char *mod_name)
{
	struct ddebug_table *dt, *nextdt;
	int ret = -ENOENT;

	mutex_lock(&ddebug_lock);
	list_for_each_entry_safe(dt, nextdt, &ddebug_tables, link) {
		/*
		 * NB: with multiple "main" builtins, strcmp would be
		 * incorrect.  Linker gives us this one.
		 */
		if (dt->info.mod_name == mod_name) {
			ddebug_drop_all_cached_prefixes(&dt->info);

			ddebug_module_sites_clear(dt);
			ddebug_table_free(dt);
			ret = 0;
			break;
		}
	}
	mutex_unlock(&ddebug_lock);
	if (!ret)
		v2pr_info("removed module \"%s\"\n", mod_name);
	return ret;
}

static int ddebug_module_notify(struct notifier_block *self, unsigned long val,
				void *data)
{
	struct module *mod = data;
	int ret = 0;

	switch (val) {
	case MODULE_STATE_COMING:
		mod->dyndbg_info.mod_name = mod->name;
		ret = ddebug_add_module(&mod->dyndbg_info);
		if (ret)
			pr_err("dyndbg: failed to add module %s: %d\n", mod->name, ret);
		break;
	case MODULE_STATE_GOING:
		ddebug_remove_module(mod->name);
		break;
	}

	return notifier_from_errno(ret);
}

static struct notifier_block ddebug_module_nb = {
	.notifier_call = ddebug_module_notify,
	.priority = 0, /* dynamic debug depends on jump label */
};

#endif /* CONFIG_MODULES */

static void ddebug_remove_all_tables(void)
{
	mutex_lock(&ddebug_lock);
	while (!list_empty(&ddebug_tables)) {
		struct ddebug_table *dt = list_entry(ddebug_tables.next,
						     struct ddebug_table,
						     link);
		ddebug_table_free(dt);
	}
	mutex_unlock(&ddebug_lock);
}

/*
 * dynamic prefix cache keys and descriptor ranges.
 *
 * ddebug_prefix_key() constructs the maple tree key by combining
 * prefix flags with the descriptor address, creating separate
 * key-spaces for different flag combinations.
 *
 * ddebug_prefix_range() determines the address range of descriptors
 * that can share a dynamic prefix based on these flags.
 */
#define DDEBUG_PREFIX_KEY_FLAGS_SHIFT (BITS_PER_LONG - 4)

static inline unsigned long ddebug_pack_key(unsigned long addr, uint8_t flags)
{
	/*
	 * Prefix flags are at bits 1-4. Pack them into bits 0-3 then shift
	 * to the top of the key to partition the key-space by flag-set.
	 * Shift the address down 4 bits; since descs are 16-byte aligned,
	 * they remain unique.
	 */
	return ((unsigned long)(flags >> 1) & 0xF) << DDEBUG_PREFIX_KEY_FLAGS_SHIFT |
		(addr >> 4);
}

static unsigned long ddebug_prefix_key(const struct _ddebug *desc)
{
	return ddebug_pack_key((unsigned long)desc, prefix_flags(desc->flags));
}

static void __maybe_unused ddebug_drop_all_cached_prefixes(const struct _ddebug_info *di)
{
	int i, f;
	struct _ddebug *dp;

	for_subvec(i, dp, di, descs) {
		for (f = 0; f < 16; f++) {
			unsigned long key = ((unsigned long)f << DDEBUG_PREFIX_KEY_FLAGS_SHIFT) |
					    ((unsigned long)dp >> 4);
			char *prefix = mtree_erase(&pr_prefixes, key);
			if (prefix) {
				pr_prefixes_count--;
				v3pr_info("drop cached prefix: %s\n", prefix);
				kfree(prefix);
			}
		}
	}
}

static void ddebug_add_cached_prefix(struct _ddebug *dp)
{
	unsigned long key = ddebug_prefix_key(dp);
	char *prefix;
	char buf[PREFIX_SIZE] = "";
	int pos = 0;
	struct _dd_prefix_key_range range;

	prefix = mtree_load(&pr_prefixes, key);
	if (prefix)
		return;

	{
		const char *mod = NULL, *file = NULL, *func = NULL;
		struct ddebug_table *dt;
		struct bonsai_tree *site_map = &dd_builtin_site_map;

		list_for_each_entry(dt, &ddebug_tables, link) {
			if (dp >= dt->info.descs.start &&
			    dp < dt->info.descs.start + dt->info.descs.len) {
				site_map = ddebug_get_site_map(dt);
				break;
			}
		}

		ddebug_resolve_site(site_map, dp,
				    (dp->flags & _DPRINTK_FLAGS_INCL_MODNAME) ? &mod : NULL,
				    (dp->flags & _DPRINTK_FLAGS_INCL_SOURCENAME) ? &file : NULL,
				    (dp->flags & _DPRINTK_FLAGS_INCL_FUNCNAME) ? &func : NULL);

		if (mod)
			pos += snprintf(buf + pos, remaining(pos), "%s:", mod);
		if (func)
			pos += snprintf(buf + pos, remaining(pos), "%s:", func);
		if (file)
			pos += snprintf(buf + pos, remaining(pos), "%s:", trim_prefix(file));
	}
	if (dp->flags & _DPRINTK_FLAGS_INCL_LINENO)
		pos += snprintf(buf + pos, remaining(pos), "%d:",
				dp->lineno);
	if (remaining(pos)) {
		buf[pos++] = ' ';
		buf[pos] = '\0';
	}

	prefix = kstrdup(buf, GFP_KERNEL);
	if (!prefix)
		return;

	ddebug_prefix_range(dp, &range);

	if (mtree_store_range(&pr_prefixes, range.start, range.end, prefix, GFP_KERNEL)) {
		kfree(prefix);
	} else {
		pr_prefixes_count++;
		v3pr_info("eagerly filled prefix cache: %s [%lx-%lx]\n", prefix, range.start, range.end);
	}
}

static void ddebug_prefix_range(const struct _ddebug *desc,
				struct _dd_prefix_key_range *range)
{
	range->start = ddebug_prefix_key(desc);
	range->end = ddebug_prefix_key(desc);
}

#include <linux/shrinker.h>

static inline unsigned long bonsai_shrinker_cost(const struct bonsai_tree *bt)
{
	if (!bt || !bt->num_segments)
		return 0;
	return bt->num_segments * (BONSAI_SEG_SIZE / PAGE_SIZE);
}

static unsigned long ddebug_shrinker_count(struct shrinker *shrinker,
					   struct shrink_control *sc)
{
	unsigned long count = 0;
	struct ddebug_table *dt;

	mutex_lock(&ddebug_lock);
	if (dd_builtin_site_map.num_segments)
		count += bonsai_shrinker_cost(&dd_builtin_site_map);

	list_for_each_entry(dt, &ddebug_tables, link) {
		if (dt->site_map.num_segments)
			count += bonsai_shrinker_cost(&dt->site_map);
	}
	if (!mtree_empty(&pr_prefixes))
		count += pr_prefixes_count;
	mutex_unlock(&ddebug_lock);

	return count ? count : SHRINK_EMPTY;
}

static unsigned long ddebug_shrinker_scan(struct shrinker *shrinker,
					  struct shrink_control *sc)
{
	struct ddebug_table *dt;
	unsigned long freed = 0;

	mutex_lock(&ddebug_lock);

	/* 1. Free built-in site map */
	if (dd_builtin_site_map.num_segments) {
		freed += bonsai_shrinker_cost(&dd_builtin_site_map);
		bonsai_destroy(&dd_builtin_site_map);
	}

	/* 2. Free module site maps */
	list_for_each_entry(dt, &ddebug_tables, link) {
		if (dt->site_map.num_segments) {
			freed += bonsai_shrinker_cost(&dt->site_map);
			bonsai_destroy(&dt->site_map);
		}
	}

	/* 3. Free prefix cache */
	if (!mtree_empty(&pr_prefixes)) {
		__mt_destroy(&pr_prefixes);
		pr_prefixes_count = 0;
		freed += 1;
	}

	mutex_unlock(&ddebug_lock);

	return freed ? freed : SHRINK_STOP;
}

static __initdata int ddebug_init_success;

static int __init dynamic_debug_init_control(void)
{
	struct proc_dir_entry *procfs_dir;
	struct dentry *debugfs_dir;
	struct shrinker *shrinker;

	if (!ddebug_init_success)
		return -ENODEV;

	shrinker = shrinker_alloc(0, "dynamic_debug");
	if (shrinker) {
		shrinker->count_objects = ddebug_shrinker_count;
		shrinker->scan_objects = ddebug_shrinker_scan;
		shrinker_register(shrinker);
	}

	/* Create the control file in debugfs if it is enabled */
	if (debugfs_initialized()) {
		debugfs_dir = debugfs_create_dir("dynamic_debug", NULL);
		debugfs_create_file("control", 0644, debugfs_dir, NULL,
				    &ddebug_proc_fops);
	}

	/* Also create the control file in procfs */
	procfs_dir = proc_mkdir("dynamic_debug", NULL);
	if (procfs_dir)
		proc_create("control", 0644, procfs_dir, &proc_fops);

	return 0;
}

static int __init dynamic_debug_init(void)
{
	int i = 0, ret = 0, mod_ct = 0;
	char *cmdline;

	struct _ddebug_info di = {
		.descs.start = __start___dyndbg_descs,
		.sites.start = __start___dyndbg_sites,
		.strings_mod.start = __start___dyndbg_strings_mod,
		.strings_file.start = __start___dyndbg_strings_file,
		.maps.start  = __start___dyndbg_class_maps,
		.users.start = __start___dyndbg_class_users,
		.descs.len = __stop___dyndbg_descs - __start___dyndbg_descs,
		.sites.len = __stop___dyndbg_sites - __start___dyndbg_sites,
		.strings_mod.len = __stop___dyndbg_strings_mod - __start___dyndbg_strings_mod,
		.strings_file.len = __stop___dyndbg_strings_file - __start___dyndbg_strings_file,
		.maps.len  = __stop___dyndbg_class_maps - __start___dyndbg_class_maps,
		.users.len = __stop___dyndbg_class_users - __start___dyndbg_class_users,
	};

#ifdef CONFIG_MODULES
	ret = register_module_notifier(&ddebug_module_nb);
	if (ret) {
		pr_warn("Failed to register dynamic debug module notifier\n");
		return ret;
	}
#endif /* CONFIG_MODULES */

	if (&__start___dyndbg_descs == &__stop___dyndbg_descs) {
		if (IS_ENABLED(CONFIG_DYNAMIC_DEBUG)) {
			pr_warn("_ddebug table is empty in a CONFIG_DYNAMIC_DEBUG build\n");
			return 1;
		}
		pr_info("Ignore empty _ddebug table in a CONFIG_DYNAMIC_DEBUG_CORE build\n");
		ddebug_init_success = 1;
		return 0;
	}
	/*
	 * Walk the builtin sites and add each module's subrange.
	 */
	if (di.sites.len) {
		unsigned int count = di.sites.len;
		struct _ddebug *range_start = di.descs.start;
		const char *cur_mod = di.sites.start[0]._modname;

		ddebug_condense_and_compress_sites(&di, &dd_builtin_site_map,
						   &dd_builtin_compressed_sites, &dd_builtin_compressed_len);

		for (i = 0; i < count; i++) {
			const char *p_mod = di.sites.start[i]._modname;
			if (p_mod != cur_mod && strcmp(p_mod, cur_mod) != 0) {
				struct _ddebug_info mod_di = di;
				mod_di.mod_name = cur_mod;
				mod_di.descs.start = range_start;
				mod_di.descs.len = &di.descs.start[i] - range_start;
				mod_di.sites.len = 0; /* Built-in modules share global tree */
				ret = ddebug_add_module(&mod_di);
				if (ret)
					goto out_err;
				mod_ct++;
				range_start = &di.descs.start[i];
				cur_mod = p_mod;
			}
		}
		if (range_start < di.descs.start + count) {
			struct _ddebug_info mod_di = di;
			mod_di.mod_name = cur_mod;
			mod_di.descs.start = range_start;
			mod_di.descs.len = (di.descs.start + count) - range_start;
			mod_di.sites.len = 0;
			ret = ddebug_add_module(&mod_di);
			mod_ct++;
		}
		i = count;
	} else {
		/* Built-in metadata is stripped: register single table covering descs */
		struct _ddebug_info mod_di = di;
		mod_di.mod_name = "vmlinux";
		mod_di.descs.start = di.descs.start;
		mod_di.descs.len = di.descs.len;
		mod_di.sites.len = 0;
		ret = ddebug_add_module(&mod_di);
		if (ret)
			goto out_err;
		mod_ct = 1;
		i = di.descs.len;
	}

	ddebug_init_success = 1;
	vpr_info("%d prdebugs in %d modules, %d KiB in ddebug tables, %d+%d kiB in __dyndbg:_descs+_sites sections\n",
		 i, mod_ct, (int)((mod_ct * sizeof(struct ddebug_table)) >> 10),
		 (int)((i * sizeof(struct _ddebug)) >> 10),
		 (int)((i * sizeof(struct _ddebug_site)) >> 10));
	vpr_info("builtin site map: %lu entries, %u nodes, height %u, segments %u (%u KiB)\n",
		 dd_builtin_site_map.entry_count, dd_builtin_site_map.node_count,
		 dd_builtin_site_map.height, dd_builtin_site_map.num_segments,
		 (unsigned int)(dd_builtin_site_map.num_segments * (BONSAI_SEG_SIZE >> 10)));

	if (di.maps.len)
		v2pr_info("  %d builtin ddebug class-maps\n", di.maps.len);

	/* now that ddebug tables are loaded, process all boot args
	 * again to find and activate queries given in dyndbg params.
	 * While this has already been done for known boot params, it
	 * ignored the unknown ones (dyndbg in particular).  Reusing
	 * parse_args avoids ad-hoc parsing.  This will also attempt
	 * to activate queries for not-yet-loaded modules, which is
	 * slightly noisy if verbose, but harmless.
	 */
	cmdline = kstrdup(saved_command_line, GFP_KERNEL);
	parse_args("dyndbg params", cmdline, NULL,
		   0, 0, 0, NULL, &ddebug_dyndbg_boot_param_cb);
	kfree(cmdline);
	return 0;

out_err:
	ddebug_remove_all_tables();
	return ret;
}
/* Allow early initialization for boot messages via boot param */
early_initcall(dynamic_debug_init);

/* Debugfs setup must be done later */
fs_initcall(dynamic_debug_init_control);
