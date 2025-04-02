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
#include <linux/mutex.h>
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

#include <rdma/ib_verbs.h>

extern struct _ddebug __start___dyndbg_descs[];
extern struct _ddebug __stop___dyndbg_descs[];
extern struct _ddebug_site __start___dyndbg_sites[];
extern struct _ddebug_site __stop___dyndbg_sites[];
extern struct _ddebug_class_map __start___dyndbg_class_maps[];
extern struct _ddebug_class_map __stop___dyndbg_class_maps[];
extern struct _ddebug_class_user __start___dyndbg_class_users[];
extern struct _ddebug_class_user __stop___dyndbg_class_users[];

/*
 * create the descriptor header record for all the builtin pr_debugs.
 */
extern struct _ddebug __hdr___dyndbg_descs[];
DYNAMIC_DEBUG_DESCRIPTORS_HEADER("builtin-header");

struct ddebug_table {
	struct list_head link;
	struct _ddebug_info info;
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

/* cache of composed prefixes for enabled pr_debugs */
static DEFINE_MTREE(pr_prefixes);

static DEFINE_MUTEX(ddebug_lock);
static LIST_HEAD(ddebug_tables);
static int verbose;
module_param(verbose, int, 0644);
MODULE_PARM_DESC(verbose, " dynamic_debug/control processing "
		 "( 0 = off (default), 1 = module add/rm, 2 = >control summary, 3 = parsing, 4 = per-site changes)");

static void ddebug_clear_prefix_cache(const struct _ddebug *dp)
{
	mtree_erase(&pr_prefixes, (unsigned long)dp);
}

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

/*
 * simplify a repeated for-loop pattern walking N steps in a T _vec
 * member inside a struct _box.  It expects int i and T *_sp to be
 * declared in the caller.
 * @_i:  caller provided counter.
 * @_sp: cursor into _vec, to examine each item.
 * @_box: ptr to a struct containing @_vec member
 * @_vec: name of a sub-struct member in _box, with array-ref and length
 */
#define for_subvec(_i, _sp, _box, _vec)				       \
	for ((_i) = 0, (_sp) = (_box)->_vec.start;		       \
	     (_i) < (_box)->_vec.len;				       \
	     (_i)++, (_sp)++)

static void vpr_info_dq(const struct ddebug_query *query, const char *msg)
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

#define vpr_di_info(di_p, msg_p, ...) ({				\
	struct _ddebug_info const *_di = di_p;				\
	v2pr_info(msg_p " module:%s nd:%d nc:%d nu:%d\n", ##__VA_ARGS__, \
		  _di->mod_name, _di->descs.len, _di->maps.len,		\
		  _di->users.len);					\
	})

static struct _ddebug_class_map *
ddebug_find_valid_class(struct _ddebug_info const *di, const char *query_class, int *class_id)
{
	struct _ddebug_class_map *map;
	struct _ddebug_class_user *cli;
	int i, idx;

	for_subvec(i, map, di, maps) {
		idx = match_string(map->class_names, map->length, query_class);
		if (idx >= 0) {
			vpr_di_info(di, "good-class: %s.%s ", map->mod_name, query_class);
			*class_id = idx + map->base;
			return map;
		}
	}
	for_subvec(i, cli, di, users) {
		idx = match_string(cli->map->class_names, cli->map->length, query_class);
		if (idx >= 0) {
			vpr_di_info(di, "class-ref: %s -> %s.%s ",
				    cli->mod_name, cli->map->mod_name, query_class);
			*class_id = idx + cli->map->base;
			return cli->map;
		}
	}
	*class_id = -ENOENT;
	return NULL;
}

/*
 * classmaps-V1 protected classes from changes by legacy commands
 * (those selecting _DPRINTK_CLASS_DFLT by omission).  This had the
 * downside that saying "class FOO" for every change can get tedious.
 *
 * V2 is smarter, it protects class-maps if the defining module also
 * calls DYNAMIC_DEBUG_CLASSMAP_PARAM to create a sysfs parameter.
 * Since they want the knob, we should assume they intend to use it
 * (in preference to "class FOO +p" >control), and want to trust its
 * settings.
 * This gives protection when its useful, and not when its just tedious.
 */
static inline bool ddebug_class_has_param(const struct _ddebug_class_map *map)
{
	return !!(map->controlling_param);
}

/* re-framed as a policy choice */
#define ddebug_class_wants_protection(map) \
	ddebug_class_has_param(map)

/*
 * validate codetree construction manual helpers
 */
__maybe_unused
static void codetree_scan(const struct _ddebug_codetree *tree, char *id)
{
	const char **p;
	int i;

	v4pr_info("scan: %s\n", id);

	for (i = 0, p = tree->mods.start; i < tree->mods.len; i++, p++)
		v4pr_info("mods: %d %s\n", i, *p);

	for (i = 0, p = tree->files.start; i < tree->files.len; i++, p++)
		v4pr_info("files: %d %s\n", i, *p);

	for (i = 0, p = tree->funcs.start; i < tree->funcs.len; i++, p++)
		v4pr_info("funcs: %d %s\n", i, *p);

	for (i = 0, p = tree->_impl.start; i < tree->_impl.len; i++, p++)
		v4pr_info("impl: %d %s\n", i, *p);
}

static int codetree_sanity_check_namevec(const struct _ddebug_codetree *tree,
					 const struct _ddebug_namevec *vec,
					 const char *vec_name, const char *id,
					 int stor_len, int max_len)
{
	if (vec->len < 0) {
		pr_err("chk:%s %s.len is negative\n", id, vec_name);
		return 1;
	} else if (vec->len > max_len) {
		pr_err("chk:%s %s.len %d exceeds max length\n", id, vec_name, vec->len);
		return 2;
	}
	if (!vec->start) {
		pr_err(":%s %s.start is NULL\n", id, vec_name);
		return 3;
	} else if (vec->start < &tree->_storage[0]) {
		pr_err(":%s %s.start below _storage\n", id, vec_name);
		return 4;
	} else if (&vec->start[vec->len] >= &tree->_storage[stor_len]) {
		v2pr_info(":%s %s %d is past _storage\n",
			  id, vec_name, vec->len);
		return 5;
	}
	return 0;
}

static inline int codetree_sanity_check(const struct _ddebug_codetree *tree,
					unsigned long stor_len, char *id)
{
	int rc = 0;

	if (!tree) {
		pr_err("codetree_sanity_check: tree is NULL\n");
		return 6;
	}
	/*
	 * XXX: hardcoded limits from squeeze output.
	 * for vng-config: 2384, 540, 236, 3160
	 * for fedora-cf: 338 mods, 566 files, 2223 funcs, 3708 descs, 3127 unique
	 */
	rc += codetree_sanity_check_namevec(tree, &tree->funcs, "funcs", id, stor_len, 2384);
	rc += codetree_sanity_check_namevec(tree, &tree->files, "files", id, stor_len, 566);
	rc += codetree_sanity_check_namevec(tree, &tree->mods, "mods",   id, stor_len, 338);
	rc += codetree_sanity_check_namevec(tree, &tree->_impl, "_impl", id, stor_len, 3160);

	codetree_scan(tree, id);
	return rc;
}


/* WIP 5000 is enough 3160 is not */
#define CODETREE_SANITY_CHECK(tree, str)  codetree_sanity_check(tree, 5000, str)

static __inline struct _ddebug_header *get_ddebug_header(const struct _ddebug *dp)
{
	struct _ddebug_descriptors_vector *box;
	/*
	 * BIG-RED-FLAG: we are negative indexing from dp[N] back to dp[0]
	 */
	box = container_of(&dp[-(dp->self_idx)],
			   struct _ddebug_descriptors_vector,
			   descriptors[0]);

	return &box->header;
}

static __inline struct _ddebug_info *to_info(const struct _ddebug *dp)
{
	struct _ddebug_header *dh = get_ddebug_header(dp);
        struct _ddebug_info *di = dh->uplink;

	BUG_ON(!di);
        return di;
}

/*
 * break some macro rules - this is a temporary use, once ->site->
 * goes away so does validation against it
 */
#define access_check(_dp, _field, _arr, _idx)				\
	struct _ddebug_info *di = to_info(_dp);				\
									\
	if (di && di->tree) {						\
		CODETREE_SANITY_CHECK(di->tree, "desc_"#_field);	\
		if (_dp->site->_field != di->tree->_arr.start[_dp->_idx]) \
			pr_info("mismatch %d %s vs %s\n",		\
				_dp->_idx,				\
				di->tree->_arr.start[_dp->_idx],	\
				_dp->site->_field);			\
	} else {							\
		pr_warn("no ddebug-info ??: %s\n", _dp->site->_field);	\
	}

static const char* desc_modname(const struct _ddebug *dp)
{
	access_check(dp, modname, mods, mods_idx);
	return (dp)->site->modname;
}

static const char* desc_filename(const struct _ddebug *dp)
{
	access_check(dp, filename, files, files_idx);
	return (dp)->site->filename;
}
static const char* desc_function(const struct _ddebug *dp)
{
	access_check(dp, function, funcs, funcs_idx);
	return (dp)->site->function;
}

#define ddebug_user_wants_protected_classes(a, b) false

/*
 * Search the tables for _ddebug's which match the given `query' and
 * apply the `flags' and `mask' to them.  Returns number of matching
 * callsites, normally the same as number of changes.  If verbose,
 * logs the changes.  Takes ddebug_lock.
 */
static int ddebug_change(const struct ddebug_query *query, struct flag_settings *modifiers)
{
	int i;
	struct ddebug_table *dt;
	unsigned int newflags;
	unsigned int nfound = 0;
	struct flagsbuf fbuf, nbuf;
	struct _ddebug_class_map *map = NULL;
	int selected_class;

	/* search for matching ddebugs */
	mutex_lock(&ddebug_lock);
	list_for_each_entry(dt, &ddebug_tables, link) {

		/* match against the module name */
		if (query->module &&
		    !match_wildcard(query->module, dt->info.mod_name))
			continue;

		if (query->class_string) {
			map = ddebug_find_valid_class(&dt->info, query->class_string,
						      &selected_class);
			if (!map)
				continue;
		} else {
			selected_class = _DPRINTK_CLASS_DFLT;
		}

		for (i = 0; i < dt->info.descs.len; i++) {
			struct _ddebug *dp = &dt->info.descs.start[i];

			if (dp->class_id != selected_class) {
				if (query->class_string)
					/* site.class != given class */
					continue;
				/* legacy query, class'd site */
				else if (ddebug_class_wants_protection(map))
					continue;
				/* allow change on class'd pr_debug */
			}
			/* match against the source filename */
			if (query->filename &&
			    !match_wildcard(query->filename, desc_filename(dp)) &&
			    !match_wildcard(query->filename,
					    kbasename(desc_filename(dp))) &&
			    !match_wildcard(query->filename,
					    trim_prefix(desc_filename(dp))))
				continue;

			/* match against the function */
			if (query->function &&
			    !match_wildcard(query->function, desc_function(dp)))
				continue;

			/* match against the format */
			if (query->format) {
				if (*query->format == '^') {
					char *p;
					/* anchored search. match must be at beginning */
					p = strstr(dp->format, query->format+1);
					if (p != dp->format)
						continue;
				} else if (!strstr(dp->format, query->format))
					continue;
			}

			/* match against the line number range */
			if (query->first_lineno &&
			    dp->lineno < query->first_lineno)
				continue;
			if (query->last_lineno &&
			    dp->lineno > query->last_lineno)
				continue;

			nfound++;

			newflags = (dp->flags & modifiers->mask) | modifiers->flags;
			if (newflags == dp->flags)
				continue;
			ddebug_clear_prefix_cache(dp);
#ifdef CONFIG_JUMP_LABEL
			if (dp->flags & _DPRINTK_FLAGS_PRINT) {
				if (!(newflags & _DPRINTK_FLAGS_PRINT))
					static_branch_disable(&dp->key.dd_key_true);
			} else if (newflags & _DPRINTK_FLAGS_PRINT) {
				static_branch_enable(&dp->key.dd_key_true);
			}
#endif
			v4pr_info("changed %s:%d [%s]%s %s => %s\n",
				  trim_prefix(desc_filename(dp)), dp->lineno,
				  dt->info.mod_name, desc_function(dp),
				  ddebug_describe_flags(dp->flags, &fbuf),
				  ddebug_describe_flags(newflags, &nbuf));
			dp->flags = newflags;
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

	vpr_info_dq(query, "parsed");
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

static int ddebug_exec_query(char *query_string, const char *modname)
{
	struct flag_settings modifiers = {};
	struct ddebug_query query = {};
#define MAXWORDS 9
	int nwords;
	char *words[MAXWORDS];

	nwords = ddebug_tokenize(query_string, words, MAXWORDS);
	if (nwords <= 0) {
		pr_err("tokenize failed\n");
		return -EINVAL;
	}
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
	return ddebug_change(&query, &modifiers);
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
		split = strpbrk(query, "%;\n");
		if (split)
			*split++ = '\0';

		query = skip_spaces_and_commas(query);

		if (!query || !*query || *query == '#')
			continue;

		vpr_info("query %d: \"%s\" mod:%s\n", i, query, modname ?: "*");

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

static int ddebug_apply_class_bitmap(const struct _ddebug_class_param *dcp,
				     const unsigned long *new_bits,
				     const unsigned long old_bits,
				     const char *query_modname)
{
#define QUERY_SIZE 128
	char query[QUERY_SIZE];
	const struct _ddebug_class_map *map = dcp->map;
	int matches = 0;
	int bi, ct;

	if (*new_bits != old_bits)
		v2pr_info("apply bitmap: 0x%lx to: 0x%lx for %s\n", *new_bits,
			  old_bits, query_modname ?: "'*'");

	for (bi = 0; bi < map->length; bi++) {
		if (test_bit(bi, new_bits) == test_bit(bi, &old_bits))
			continue;

		snprintf(query, QUERY_SIZE, "class %s %c%s", map->class_names[bi],
			 test_bit(bi, new_bits) ? '+' : '-', dcp->flags);

		ct = ddebug_exec_queries(query, query_modname);
		matches += ct;

		v2pr_info("bit_%d: %d matches on class: %s -> 0x%lx\n", bi,
			  ct, map->class_names[bi], *new_bits);
	}
	if (*new_bits != old_bits)
		v2pr_info("applied bitmap: 0x%lx to: 0x%lx for %s\n", *new_bits,
			  old_bits, query_modname ?: "'*'");

	return matches;
}

/* stub to later conditionally add "$module." prefix where not already done */
#define KP_NAME(kp)	kp->name

#define CLASSMAP_BITMASK(width) ((1UL << (width)) - 1)

static void ddebug_class_param_clamp_input(unsigned long *inrep, const struct kernel_param *kp)
{
	const struct _ddebug_class_param *dcp = kp->arg;
	const struct _ddebug_class_map *map = dcp->map;

	switch (map->map_type) {
	case DD_CLASS_TYPE_DISJOINT_BITS:
		/* expect bits. mask and warn if too many */
		if (*inrep & ~CLASSMAP_BITMASK(map->length)) {
			pr_warn("%s: input: 0x%lx exceeds mask: 0x%lx, masking\n",
				KP_NAME(kp), *inrep, CLASSMAP_BITMASK(map->length));
			*inrep &= CLASSMAP_BITMASK(map->length);
		}
		break;
	case DD_CLASS_TYPE_LEVEL_NUM:
		/* input is bitpos, of highest verbosity to be enabled */
		if (*inrep > map->length) {
			pr_warn("%s: level:%ld exceeds max:%d, clamping\n",
				KP_NAME(kp), *inrep, map->length);
			*inrep = map->length;
		}
		break;
	}
}
static int param_set_dyndbg_module_classes(const char *instr,
					   const struct kernel_param *kp,
					   const char *mod_name)
{
	const struct _ddebug_class_param *dcp = kp->arg;
	const struct _ddebug_class_map *map = dcp->map;
	unsigned long inrep, new_bits, old_bits;
	int rc, totct = 0;
	char *nl;

	rc = kstrtoul(instr, 0, &inrep);
	if (rc) {
		nl = strchr(instr, '\n');
		if (nl)
			*nl = '\0';
		pr_err("expecting numeric input, not: %s > %s\n", instr, KP_NAME(kp));
		return -EINVAL;
	}
	ddebug_class_param_clamp_input(&inrep, kp);

	switch (map->map_type) {
	case DD_CLASS_TYPE_DISJOINT_BITS:
		v2pr_info("bits:0x%lx > %s.%s\n", inrep, mod_name ?: "*", KP_NAME(kp));
		totct += ddebug_apply_class_bitmap(dcp, &inrep, *dcp->bits, mod_name);
		*dcp->bits = inrep;
		break;
	case DD_CLASS_TYPE_LEVEL_NUM:
		old_bits = CLASSMAP_BITMASK(*dcp->lvl);
		new_bits = CLASSMAP_BITMASK(inrep);
		v2pr_info("lvl:%ld bits:0x%lx > %s\n", inrep, new_bits, KP_NAME(kp));
		totct += ddebug_apply_class_bitmap(dcp, &new_bits, old_bits, mod_name);
		*dcp->lvl = inrep;
		break;
	default:
		pr_warn("%s: bad map type: %d\n", KP_NAME(kp), map->map_type);
	}
	vpr_info("%s: total matches: %d\n", KP_NAME(kp), totct);
	return 0;
}

/**
 * param_set_dyndbg_classes - classmap kparam setter
 * @instr: string echo>d to sysfs, input depends on map_type
 * @kp:    kp->arg has state: bits/lvl, classmap, map_type
 *
 * enable/disable all class'd pr_debugs in the classmap. For LEVEL
 * map-types, enforce * relative levels by bitpos.
 *
 * Returns: 0 or <0 if error.
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
	const struct _ddebug_class_param *dcp = kp->arg;
	const struct _ddebug_class_map *map = dcp->map;

	switch (map->map_type) {
	case DD_CLASS_TYPE_DISJOINT_BITS:
		return scnprintf(buffer, PAGE_SIZE, "0x%lx\n", *dcp->bits);
	case DD_CLASS_TYPE_LEVEL_NUM:
		return scnprintf(buffer, PAGE_SIZE, "%ld\n", *dcp->lvl);
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


static int __dynamic_emit_lookup(struct _ddebug *desc, char *buf, int pos)
{
	char *prefix, *cpy;

	if (desc->flags & _DPRINTK_FLAGS_PREFIX_CACHED) {
		prefix = (char *) mtree_load(&pr_prefixes, (unsigned long)desc);
		if (prefix) {
			pos += snprintf(buf + pos, remaining(pos), "%s", prefix);
			v4pr_info("using prefix cache:%px %s", buf, buf + pos);
			return pos;
		}
	}
	if (desc->flags & _DPRINTK_FLAGS_INCL_MODNAME)
		pos += snprintf(buf + pos, remaining(pos), "%s:",
				desc_modname(desc));
	if (desc->flags & _DPRINTK_FLAGS_INCL_FUNCNAME)
		pos += snprintf(buf + pos, remaining(pos), "%s:",
				desc_function(desc));
	if (desc->flags & _DPRINTK_FLAGS_INCL_SOURCENAME)
		pos += snprintf(buf + pos, remaining(pos), "%s:",
				trim_prefix(desc_filename(desc)));
	if (desc->flags & _DPRINTK_FLAGS_INCL_LINENO)
		pos += snprintf(buf + pos, remaining(pos), "%d:",
				desc->lineno);

	/* cuz LOOKUP, we've emitted, so add trailing space if room */
	if (remaining(pos))
		buf[pos++] = ' ';

	/* save dup of buf to cache */
	cpy = kstrdup(buf + pos, GFP_KERNEL);
	mtree_store(&pr_prefixes, (unsigned long)desc, (void *)cpy, GFP_KERNEL);
	desc->flags |= _DPRINTK_FLAGS_PREFIX_CACHED;
	v3pr_info("filling prefix cache for pr-dbg:%px %s", desc, cpy);

	return pos;
}

static char *__dynamic_emit_prefix(struct _ddebug *desc, char *buf)
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
	if (desc->flags & _DPRINTK_FLAGS_INCL_ANY)
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

/*
 * File_ops->write method for <debugfs>/dynamic_debug/control.  Gathers the
 * command text from userspace, parses and executes it.
 */
#define USER_BUF_PAGE 4096
static ssize_t ddebug_proc_write(struct file *file, const char __user *ubuf,
				  size_t len, loff_t *offp)
{
	char *tmpbuf;
	int ret;

	if (len == 0)
		return 0;
	if (len > USER_BUF_PAGE - 1) {
		pr_warn("expected <%d bytes into control\n", USER_BUF_PAGE);
		return -E2BIG;
	}
	tmpbuf = memdup_user_nul(ubuf, len);
	if (IS_ERR(tmpbuf))
		return PTR_ERR(tmpbuf);
	v2pr_info("read %zu bytes from userspace\n", len);

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
static void *ddebug_proc_next(struct seq_file *m, void *p, loff_t *pos)
{
	struct ddebug_iter *iter = m->private;
	struct _ddebug *dp;

	if (p == SEQ_START_TOKEN)
		dp = ddebug_iter_first(iter);
	else
		dp = ddebug_iter_next(iter);
	++*pos;
	return dp;
}

#define class_in_range(class_id, map)					\
	(class_id >= map->base && class_id < map->base + map->length)

static const char *ddebug_class_name(struct _ddebug_info *di, struct _ddebug *dp)
{
	struct _ddebug_class_map *map;
	struct _ddebug_class_user *cli;
	int i;

	for_subvec(i, map, di, maps)
		if (class_in_range(dp->class_id, map))
			return map->class_names[dp->class_id - map->base];

	for_subvec(i, cli, di, users)
		if (class_in_range(dp->class_id, cli->map))
			return cli->map->class_names[dp->class_id - cli->map->base];

	return NULL;
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
	char const *class;

	if (p == SEQ_START_TOKEN) {
		seq_puts(m,
			 "# filename:lineno [module]function flags format\n");
		return 0;
	}

	seq_printf(m, "%s:%u [%s]%s =%s \"",
		   trim_prefix(desc_filename(dp)), dp->lineno,
		   iter->table->info.mod_name, desc_function(dp),
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
	struct _ddebug_class_map const *_cm = cm_p;			\
	v2pr_info(msg_fmt " %s [%d..%d] %s..%s\n", ##__VA_ARGS__,	\
		  _cm->mod_name, _cm->base, _cm->base + _cm->length,	\
		  _cm->class_names[0], _cm->class_names[_cm->length - 1]); \
	})

static void ddebug_sync_classbits(const struct kernel_param *kp, const char *modname)
{
	const struct _ddebug_class_param *dcp = kp->arg;
	unsigned long new_bits;

	ddebug_class_param_clamp_input(dcp->bits, kp);

	switch (dcp->map->map_type) {
	case DD_CLASS_TYPE_DISJOINT_BITS:
		v2pr_info("  %s: classbits: 0x%lx\n", KP_NAME(kp), *dcp->bits);
		ddebug_apply_class_bitmap(dcp, dcp->bits, 0UL, modname);
		break;
	case DD_CLASS_TYPE_LEVEL_NUM:
		new_bits = CLASSMAP_BITMASK(*dcp->lvl);
		v2pr_info("  %s: lvl:%ld bits:0x%lx\n", KP_NAME(kp), *dcp->lvl, new_bits);
		ddebug_apply_class_bitmap(dcp, &new_bits, 0UL, modname);
		break;
	default:
		pr_err("bad map type %d\n", dcp->map->map_type);
		return;
	}
}

static struct _ddebug_class_param *
ddebug_get_classmap_kparam(const struct kernel_param *kp,
			   const struct _ddebug_class_map *map)
{
	struct _ddebug_class_param *dcp;

	if (kp->ops != &param_ops_dyndbg_classes)
		return false;

	dcp = (struct _ddebug_class_param *)kp->arg;

	return (map == dcp->map)
		? dcp : (struct _ddebug_class_param *)NULL;
}

static void ddebug_match_apply_kparam(const struct kernel_param *kp,
				      struct _ddebug_class_map *map,
				      const char *mod_name)
{
	struct _ddebug_class_param *dcp = ddebug_get_classmap_kparam(kp, map);

	if (dcp) {
		map->controlling_param = dcp;
		v2pr_info(" kp:%s.%s =0x%lx", mod_name, kp->name, *dcp->bits);
		vpr_cm_info(map, " %s mapped to: ", mod_name);
		ddebug_sync_classbits(kp, mod_name);
	}
}

static void ddebug_apply_params(struct _ddebug_class_map *cm, const char *mod_name)
{
	const struct kernel_param *kp;
#if IS_ENABLED(CONFIG_MODULES)
	int i;

	if (cm->mod) {
		vpr_cm_info(cm, "loaded classmap: %s", mod_name);
		/* ifdef protects the cm->mod->kp deref */
		for (i = 0, kp = cm->mod->kp; i < cm->mod->num_kp; i++, kp++)
			ddebug_match_apply_kparam(kp, cm, mod_name);
	}
#endif
	if (!cm->mod) {
		vpr_cm_info(cm, "builtin classmap: %s", mod_name);
		for (kp = __start___param; kp < __stop___param; kp++)
			ddebug_match_apply_kparam(kp, cm, mod_name);
	}
}

/*
 * called from add_module, ie early. it can find controlling kparams,
 * which can/does? enable protection of this classmap from class-less
 * queries, on the grounds that the user created the kparam, means to
 * use it, and expects it to reflect reality.  We should oblige him,
 * and protect those classmaps from classless "-p" changes.
 */
static void ddebug_apply_class_maps(const struct _ddebug_info *di)
{
	struct _ddebug_class_map *cm;
	int i;

	for_subvec(i, cm, di, maps)
		ddebug_apply_params(cm, cm->mod_name);

	vpr_di_info(di, "attached %d classmaps to module: %s ", i, cm->mod_name);
}

static void ddebug_apply_class_users(const struct _ddebug_info *di)
{
	struct _ddebug_class_user *cli;
	int i;

	for_subvec(i, cli, di, users)
		ddebug_apply_params(cli->map, cli->mod_name);

	vpr_di_info(di, "attached %d class-users to module: %s ", i, cli->mod_name);
}

/*
 * Walk the @_box->@_vec member, over @_vec.start[0..len], and find
 * the contiguous subrange of elements matching on ->mod_name.  Copy
 * the subrange into @_dst.  This depends on vars defd by caller.
 *
 * @_i:   caller provided counter var, init'd by macro
 * @_sp:  cursor into @_vec.
 * @_box: contains member named @_vec
 * @_vec: an array-ref, with: .start .len fields.
 * @_dst: an array-ref: to remember the module's subrange
 */
#define dd_mark_vector_subrange(_i, _dst, _sp, _box, _vec) ({		\
	int nc = 0;							\
	for_subvec(_i, _sp, _box, _vec) {				\
		if (!strcmp((_sp)->mod_name, (_dst)->info.mod_name)) {	\
			if (!nc++)					\
				(_dst)->info._vec.start = (_sp);	\
		} else {						\
			if (nc)						\
				break; /* end of consecutive matches */ \
		}							\
	}								\
	(_dst)->info._vec.len = nc;					\
})

static int __maybe_unused
ddebug_class_range_overlap(struct _ddebug_class_map *cm,
			   u64 *reserved_ids)
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

/*
 * @struct _ddebug_codetree: is composed of 4 struct _ddebug_namevecs,
 * and a flex-array char *_storage[] array.
 *
 * 3 of the structs track the stack allocation of entries for
 * de-duplicated copies of the modname, filename, function columns of
 * the sites vector.  A simple top-of-stack duplicate check suffices
 * here because the sites vector is ordered by compiler/linker, giving
 * us a cheap and sufficient de-duplication of each of the 3 vectors.
 *
 * The 4th struct, named _impl, tracks the block-wize allocation of
 * _storage to the other 3 columns.  The sequential block filling of
 * these 3 stacks yields ~91%,90%,45% compression by RLE (run-length
 * encoding).
 *
 * The codetree record is created with enough _storage for 1/3 the
 * total "unique" (low-cost approx dedup'd), then shrunk to fit whats
 * actually needed.
 */

struct str_vec {
	unsigned int base;
	unsigned int len;
};

struct str_stk {
	const char **start;
	unsigned int len;
	int tosi;
};

static void set_STK(struct str_stk *stk, struct _ddebug_namevec *_vec)
{
	stk->start = _vec->start;
	stk->len = _vec->len;
	stk->tosi = 0;
}

/* tosi refs 1st free slot, push sentinel so */
#define tos_STK(stk)		((stk)->start[ (stk)->tosi - 1 ])
#define push_STK(stk, val)	((stk)->start[ (stk)->tosi++ ] = val)
#define idx_STK(stk)		((stk)->tosi - 1)

static int push_STK_ifnew(struct str_stk *stk, const char *val)
{
	if (val != tos_STK(stk)) {
		push_STK(stk, val);
	}
	return idx_STK(stk);
}

static void freeze_STK_to_tree(struct str_stk *stk,
			       struct _ddebug_namevec *kind,
			       struct _ddebug_codetree *tree)
{
	kind->start = stk->start;
	kind->len = stk->tosi;
	/* append kind to _impl */
	tree->_impl.start += kind->len;
	tree->_impl.len -= kind->len;
}

/*
 * @ddebug_squeeze_codeorg(&_ddebug_info) scans the sites/org vector,
 * examines the 3 fields, and saves the "unique" (char*) vals of each
 * to separate "allocations" within the codetree record (more below).
 * Since descs,sites are ordered (by the code & compiler),
 * top-of-stack check is sufficient.
 */
static int ddebug_squeeze_codeorg(struct _ddebug_info *di)
{
	struct _ddebug_site *code;
	struct _ddebug_codetree *tree, *trimmed;
	struct str_stk stk;
	int i;

	if (di->tree) {
		pr_info("dyndbg: squeeze already done!!\n");
		return 0;
	}
	/* allocate enough for everything */
	tree = kzalloc(sizeof(struct _ddebug_codetree)
		       + di->descs.len * sizeof(char*), GFP_KERNEL);
	if (!tree)
		return -ENOMEM;

	/* start codetree's stack of stacks */
	tree->_impl.start = tree->_storage;
	tree->_impl.len = di->descs.len;

#define load_vec_and_index(_vec, _field)				\
	do {								\
		set_STK(&stk, &tree->_impl);				\
		push_STK(&stk, "_NO_SUCH_" # _field);			\
		for_subvec(i, code, di, sites) {			\
			di->descs.start[i]._vec##_idx =			\
				push_STK_ifnew(&stk, code->_field);	\
		}							\
		/* freeze stack on _impl */				\
		freeze_STK_to_tree(&stk, &tree->_vec, tree);		\
	} while (0)

	load_vec_and_index(funcs, function);
	load_vec_and_index(files, filename);
	load_vec_and_index(mods, modname);
	/*
	 * finalize the packed array (segmented by func,file,mod).
	 * enumerated from descriptors.
	 */
	tree->_impl.start = tree->_storage;
	tree->_impl.len = di->descs.len - tree->_impl.len;

	CODETREE_SANITY_CHECK(tree, "tree");

	/* shrink to fit and attach */
	trimmed = krealloc(tree, sizeof(struct _ddebug_codetree)
			    + tree->_impl.len * sizeof(char*), GFP_KERNEL);
	if (!trimmed)
		return -ENOMEM;  /* this would be weird */

	CODETREE_SANITY_CHECK(trimmed, "trimmed");

	if (trimmed != tree) {
		/*
		 * krealloc moved the tree while shrinking it.  While
		 * this is *unlikely* its not impossible?  If it
		 * happens, fixup the 4 vectors (subranges) with
		 * pointers into the _storage
		 */
		unsigned long int offset = (trimmed - tree);

		trimmed->mods.start += offset;
		trimmed->files.start += offset;
		trimmed->funcs.start += offset;
		trimmed->_impl.start = trimmed->_storage;
		pr_info("ddebug: krealloc moved codetree\n");
	}
	di->tree = trimmed;

	/* set self-index in the descriptors, this gives access to header */
	for_subvec(i, code, di, sites) {
		di->descs.start[i].self_idx = i;
	}

	vpr_info("squeeze done: %d mods, %d files, %d funcs, %d descs\n",
		 tree->mods.len, tree->files.len, tree->funcs.len, di->descs.len);

#define kept (tree->mods.len + tree->files.len + tree->funcs.len)
#define total (3 * di->descs.len)

	vpr_info("squeeze kept %d saved %d of total %d ptrs\n",
		 kept, total - kept, total);

	CODETREE_SANITY_CHECK(di->tree, "saved");

	return 0;
}

/*
 * Allocate a new ddebug_table for the given module
 * and add it to the global list.
 */
static int ddebug_add_module(struct _ddebug_info *di)
{
	struct ddebug_table *dt;
	struct _ddebug_class_map *cm;
	struct _ddebug_class_user *cli;
	u64 reserved_ids = 0;
	int i;

	if (!di->descs.len)
		return 0;

	v3pr_info("add-module: %s %d sites\n", di->mod_name, di->descs.len);

	dt = kzalloc(sizeof(*dt), GFP_KERNEL);
	if (dt == NULL) {
		pr_err("error adding module: %s\n", di->mod_name);
		return -ENOMEM;
	}
	/*
	 * For built-in modules, name (as supplied in di by its
	 * callers) lives in .rodata and is immortal. For loaded
	 * modules, name points at the name[] member of struct module,
	 * which lives at least as long as this struct ddebug_table.
	 */
	dt->info = *di;
	INIT_LIST_HEAD(&dt->link);

	dd_mark_vector_subrange(i, dt, cm, di, maps);
	dd_mark_vector_subrange(i, dt, cli, di, users);

	if (!di->tree) {
		vpr_info("re-squeeze?\n");
		ddebug_squeeze_codeorg(di);
		CODETREE_SANITY_CHECK(di->tree, "add_module refresh");
	}
	else CODETREE_SANITY_CHECK(di->tree, "add_module same-old");

	for_subvec(i, cm, &dt->info, maps)
		if (ddebug_class_range_overlap(cm, &reserved_ids))
			goto cleanup;
	for_subvec(i, cli, &dt->info, users)
		if (ddebug_class_range_overlap(cli->map, &reserved_ids))
			goto cleanup;

	if (dt->info.maps.len)
		ddebug_apply_class_maps(&dt->info);

	mutex_lock(&ddebug_lock);
	list_add_tail(&dt->link, &ddebug_tables);
	mutex_unlock(&ddebug_lock);

	if (dt->info.users.len)
		ddebug_apply_class_users(&dt->info);

	vpr_info("%3u debug prints in module %s\n", di->descs.len, di->mod_name);
	return 0;
cleanup:
	WARN_ONCE("dyndbg multi-classmap conflict in %s\n", di->mod_name);
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
	if (dt->info.tree)
		kfree(dt->info.tree);
	kfree(dt);
}

#ifdef CONFIG_MODULES

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
		if (dt->info.mod_name == mod_name) {
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
			WARN(1, "Failed to allocate memory: dyndbg may not work properly.\n");
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

static __initdata int ddebug_init_success;

static int __init dynamic_debug_init_control(void)
{
	struct proc_dir_entry *procfs_dir;
	struct dentry *debugfs_dir;

	if (!ddebug_init_success)
		return -ENODEV;

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

static struct _ddebug_info _dd_builtins_info;

static int __init dynamic_debug_init(void)
{
	struct _ddebug_descriptors_vector *ddvec;
	struct _ddebug_site *site, *site_mod_start;
	struct _ddebug *iter, *iter_mod_start;
	int ret, i, mod_sites, mod_ct;
	const char *modname;
	char *cmdline;

	struct _ddebug_info di = {
		.descs.start = __start___dyndbg_descs,
		.sites.start = __start___dyndbg_sites,
		.maps.start  = __start___dyndbg_class_maps,
		.users.start = __start___dyndbg_class_users,
		.descs.len = __stop___dyndbg_descs - __start___dyndbg_descs,
		.sites.len = __stop___dyndbg_sites - __start___dyndbg_sites,
		.maps.len  = __stop___dyndbg_class_maps - __start___dyndbg_class_maps,
		.users.len = __stop___dyndbg_class_users - __start___dyndbg_class_users,
	};
	pr_info("builtin descs:%d sites:%d maps:%d users:%d\n",
		di.descs.len, di.sites.len, di.maps.len, di.users.len);

	BUG_ON(di.sites.len != di.descs.len);
	BUILD_BUG_ON(sizeof(struct _ddebug_header) != sizeof(struct _ddebug));

	ddebug_squeeze_codeorg(&di);
	CODETREE_SANITY_CHECK(di.tree, "dynamic_debug_init");

	BUG_ON(di.sites.len != di.descs.len);
	BUG_ON(di.descs.len > (1 << _DD_BI_DESCS_MAX));
	BUG_ON(di.tree->files.len > (1 << _DD_BI_FILES_MAX));
	BUG_ON(di.tree->funcs.len > (1 << _DD_BI_FUNCS_MAX));

	if (di.tree->funcs.len > (1 << (_DD_BI_FUNCS_MAX)))
		pr_info("builtin overflow: %d vs %d\n",
			di.tree->funcs.len, (1 << (_DD_BI_FUNCS_MAX)));

	/* save the state before using the cursor to iterate the modules */
	_dd_builtins_info = di;

	ddvec = (struct _ddebug_descriptors_vector *) __hdr___dyndbg_descs;
	ddvec->header.uplink = &_dd_builtins_info;
	ddvec->header.num_descs = di.descs.len;

	vpr_info("ddebug: header for %s, %d descriptors\n",
		 ddvec->header._id,  ddvec->header.num_descs);

	ddebug_squeeze_codeorg(&di);

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

	site = site_mod_start = di.sites.start;
	iter = iter_mod_start = __start___dyndbg_descs;
	modname = desc_modname(iter);
	i = mod_sites = mod_ct = 0;

	for (; iter < __stop___dyndbg_descs; iter++, site++, i++, mod_sites++) {

		BUG_ON(site != iter->site);

		if (strcmp(modname, desc_modname(iter))) {
			mod_ct++;

			di.descs.len = mod_sites;
			di.descs.start = iter_mod_start;
			di.sites.len = mod_sites;
			di.sites.start = site_mod_start;
			di.mod_name = modname;
			ret = ddebug_add_module(&di);
			if (ret)
				goto out_err;

			mod_sites = 0;
			modname = desc_modname(iter);
			iter_mod_start = iter;
			site_mod_start = site;
		}
	}
	di.descs.len = mod_sites;
	di.descs.start = iter_mod_start;
	di.sites.len = mod_sites;
	di.sites.start = site_mod_start;
	di.mod_name = modname;
	ret = ddebug_add_module(&di);
	if (ret)
		goto out_err;

	ddebug_init_success = 1;
	vpr_info("%d prdebugs in %d modules, %d KiB in ddebug tables, %d kiB in __dyndbg section\n",
		 i, mod_ct, (int)((mod_ct * sizeof(struct ddebug_table)) >> 10),
		 (int)((i * sizeof(struct _ddebug)) >> 10));

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
	return 0;
}
/* Allow early initialization for boot messages via boot param */
early_initcall(dynamic_debug_init);

/* Debugfs setup must be done later */
fs_initcall(dynamic_debug_init_control);
