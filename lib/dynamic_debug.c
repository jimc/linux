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
#include <linux/trace.h>

#define CREATE_TRACE_POINTS
#include <trace/events/dyndbg.h>

#include <rdma/ib_verbs.h>

extern struct _ddebug __start___dyndbg[];
extern struct _ddebug __stop___dyndbg[];
extern struct ddebug_class_map __start___dyndbg_classes[];
extern struct ddebug_class_map __stop___dyndbg_classes[];
extern struct ddebug_class_user __start___dyndbg_class_users[];
extern struct ddebug_class_user __stop___dyndbg_class_users[];

struct ddebug_table {
	struct list_head link;
	const char *mod_name;
	struct _ddebug *ddebugs;
	struct ddebug_class_map *classes;
	struct ddebug_class_user *class_users;
	unsigned int num_ddebugs, num_classes, num_class_users;
} __packed;

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
	int trace_dst;
};

#define DD_OPEN_CMD	"open"
#define DD_CLOSE_CMD	"close"
#define DD_TR_EVENT	"0"

struct dd_private_tracebuf {
	const char *name;
	struct trace_array *arr;
	int use_cnt;
};

/*
 * When trace is enabled (T flag is set) and trace destination field
 * value is in range [1..63] then debug logs will be written to trace
 * instance whose name is stored in buf[trace destination].name, e.g.
 * when trace destination value is 2 and buf[2].name is set to tbt then
 * debug logs will be written to <debugfs>/tracing/instances/tbt.
 * On the other hand trace destination value 0 is reserved for writing
 * debug logs to trace events (prdbg, devdbg), that is why buf[0] is not
 * used and we traverse bitmap starting from bit 1 (bit 0 is also not used).
 */
struct dd_tracebuf_tbl_info {
	struct dd_private_tracebuf buf[TRACE_DST_LAST];
	DECLARE_BITMAP(bmap, TRACE_DST_LAST);
	int bmap_size;
#define DST_NOT_SET	(-1)
#define DST_TR_EVENT	0
	int default_dst;
};

static DEFINE_MUTEX(ddebug_lock);
static LIST_HEAD(ddebug_tables);
static int verbose;
module_param(verbose, int, 0644);
MODULE_PARM_DESC(verbose, " dynamic_debug/control processing "
		 "( 0 = off (default), 1 = module add/rm, 2 = >control summary, 3 = parsing, 4 = per-site changes)");

static struct
dd_tracebuf_tbl_info trc_tbl = { .bmap_size = TRACE_DST_LAST,
				 .default_dst = DST_TR_EVENT, };

static inline struct dd_ctrl *get_ctrl(struct _ddebug *desc)
{
	return &desc->ctrl;
}

static inline void set_ctrl(struct _ddebug *desc, struct dd_ctrl *ctrl)
{
	desc->ctrl = *ctrl;
}

static inline unsigned int get_flags(const struct _ddebug *desc)
{
	return desc->ctrl.flags;
}

static inline unsigned int get_trace_dst(const struct _ddebug *desc)
{
	return desc->ctrl.trace_dst;
}

static int find_tr_instance(const char *name)
{
	int idx = 1;

	for_each_set_bit_from(idx, trc_tbl.bmap, trc_tbl.bmap_size)
		if (!strcmp(trc_tbl.buf[idx].name, name))
			return idx;

	return -ENOENT;
}

static const
char *read_colon_args(const char *str, struct flag_settings *modifiers)
{
	int len, idx = DST_TR_EVENT;
	char *end;

	/* Check if trace destination was already set */
	if (modifiers->trace_dst != DST_NOT_SET)
		return NULL;
	/*
	 * When both T flag and ':' are provided then the T flag
	 * has to be followed by ':' in the form of 'T:'.
	 */
	if (modifiers->flags & _DPRINTK_FLAGS_TRACE &&
	    *(str-1) != 'T')
		return NULL;

	if (strchr(str, 'T'))
		return NULL;

	str++;
	end = strchr(str, '.');
	if (end) {
		len = end - str;
		*end = '\0';
	} else
		len = strlen(str);
	len -= 1;

	/* destination trace events */
	if (!strcmp(str, DD_TR_EVENT))
		goto end;

	idx = find_tr_instance(str);
	if (idx < 0)
		return NULL;
end:
	modifiers->trace_dst = idx;
	return end ? end : str + len;
}

/*
 * Maximum number of characters which are being displayed when
 * printing trace instance name, longer names are truncated
 */
#define FLAG_COLON_ARG_LEN 24

static char *show_colon_args(struct dd_ctrl *ctrl, char *p)
{
	int n, len = FLAG_COLON_ARG_LEN;
	const char *str;

	str = !ctrl->trace_dst ? DD_TR_EVENT :
	      trc_tbl.buf[ctrl->trace_dst].name;

	n = snprintf(p, len, "%s", str);
	if (n >= len) {
		strscpy(&p[len-4], "...", 4);
		n = len - 1;
	}

	return n < 0 ? p : p + n;
}

/* Return the path relative to source root */
static inline const char *trim_prefix(const char *path)
{
	int skip = strlen(__FILE__) - strlen("lib/dynamic_debug.c");

	if (strncmp(path, __FILE__, skip))
		skip = 0; /* prefix mismatch, don't skip */

	return path + skip;
}

typedef const char* (*read_flag_args_f)(const char *, struct flag_settings *);
typedef char* (*show_flag_args_f)(struct dd_ctrl *, char *);

static const struct
{
	unsigned flag:8;
	char opt_char;
	bool always_show;
	read_flag_args_f read_args;
	show_flag_args_f show_args;
} opt_array[] = {
	{ _DPRINTK_FLAGS_PRINTK, 'p' },
	{ _DPRINTK_FLAGS_TRACE, 'T' },
	/*
	 * We don't reserve a flag for ':'. The ':' is used to provide
	 * trace destination name and when provided together with T flag
	 * it enables output to the trace destination. When ':' is provided
	 * without T flag then it sets trace destination for a callsite for
	 * future use. When both T flag and ':'are provided together then
	 * ':' has to follow T flag in the form of 'T:'.
	 */
	{ _DPRINTK_FLAGS_NONE, ':', true, read_colon_args, show_colon_args },
	{ _DPRINTK_FLAGS_INCL_MODNAME, 'm' },
	{ _DPRINTK_FLAGS_INCL_FUNCNAME, 'f' },
	{ _DPRINTK_FLAGS_INCL_SOURCENAME, 's' },
	{ _DPRINTK_FLAGS_INCL_LINENO, 'l' },
	{ _DPRINTK_FLAGS_INCL_TID, 't' },
	{ _DPRINTK_FLAGS_NONE, '_' },
};

struct ctrlbuf { char buf[ARRAY_SIZE(opt_array)+FLAG_COLON_ARG_LEN+1]; };

/* format a string into buf[] which describes the _ddebug's flags */
static char *ddebug_describe_ctrl(struct dd_ctrl *ctrl, struct ctrlbuf *cb)
{
	show_flag_args_f show_args = NULL;
	char *p = cb->buf;
	int i;

	for (i = 0; i < ARRAY_SIZE(opt_array); ++i)
		if (ctrl->flags & opt_array[i].flag ||
		    opt_array[i].always_show) {
			if (show_args)
				*p++ = '.';
			*p++ = opt_array[i].opt_char;
			show_args = opt_array[i].show_args;
			if (show_args)
				p = show_args(ctrl, p);
		}

	if (p == cb->buf)
		*p++ = '_';
	*p = '\0';

	return cb->buf;
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

static bool is_dd_trace_cmd(const char *str)
{
	if (!strcmp(str, DD_OPEN_CMD) || !strcmp(str, DD_CLOSE_CMD))
		return true;

	return false;
}

static bool dd_good_trace_name(const char *str)
{
	/* "0" is reserved for writing debug logs to trace events (prdbg, devdbg) */
	if (!strcmp(str, DD_TR_EVENT))
		return false;

	/* we allow trace instance names to include ^\w+ and underscore */
	while (*str != '\0') {
		if (!isalnum(*str) && *str != '_')
			return false;
		str++;
	}

	return true;
}

static const char *get_tr_default_dst_str(void)
{
	if (trc_tbl.default_dst == DST_TR_EVENT)
		return DD_TR_EVENT;
	else
		return trc_tbl.buf[trc_tbl.default_dst].name;
}

static void update_tr_default_dst(int trace_dst)
{
	if (trace_dst == trc_tbl.default_dst)
		return;

	trc_tbl.default_dst = trace_dst;
	v3pr_info("set default trace dst to idx=%d, name=%s\n", trace_dst,
		  get_tr_default_dst_str());
}

static int handle_trace_open_cmd(const char *arg)
{
	struct dd_private_tracebuf *buf;
	int idx = 0, ret = 0;

	mutex_lock(&ddebug_lock);

	/*
	 * request to open '0' or an already opened trace instance
	 * results in update of default trace destination
	 */
	if (!strcmp(arg, DD_TR_EVENT))
		goto update;

	idx = find_tr_instance(arg);
	if (idx >= 0)
		goto update;

	/* bit 0 is not used, reserved for trace prdbg and devdbg events */
	idx = find_next_zero_bit(trc_tbl.bmap, trc_tbl.bmap_size, 1);
	if (idx == trc_tbl.bmap_size) {
		ret = -ENOSPC;
		goto end;
	}

	if (!dd_good_trace_name(arg)) {
		pr_err("invalid instance name:%s\n", arg);
		ret = -EINVAL;
		goto end;
	}

	buf = &trc_tbl.buf[idx];
	buf->name = kstrdup(arg, GFP_KERNEL);
	if (!buf->name) {
		ret = -ENOMEM;
		goto end;
	}

	buf->arr = trace_array_get_by_name(buf->name, NULL);
	if (!buf->arr) {
		pr_err("failed to get trace array name:%s", buf->name);
		ret = -EINVAL;
		goto end;
	}

	ret = trace_array_init_printk(buf->arr);
	if (ret) {
		pr_err("failed to init trace array name:%s", buf->name);
		trace_array_put(buf->arr);
		trace_array_destroy(buf->arr);
		goto end;
	}

	buf->use_cnt = 0;
	set_bit(idx, trc_tbl.bmap);
	v3pr_info("opened trace instance idx=%d, name=%s\n", idx, arg);
update:
	update_tr_default_dst(idx);
end:
	mutex_unlock(&ddebug_lock);
	return ret;
}

static int handle_trace_close_cmd(const char *arg)
{
	struct dd_private_tracebuf *buf;
	int idx, ret = 0;

	mutex_lock(&ddebug_lock);

	idx = find_tr_instance(arg);
	if (idx < 0) {
		ret = idx;
		goto end;
	}

	buf = &trc_tbl.buf[idx];

	WARN_ON(buf->use_cnt < 0);
	if (buf->use_cnt) {
		pr_err("trace instance is being used name=%s, use_cnt=%d\n",
		       buf->name, buf->use_cnt);
		ret = -EBUSY;
		goto end;
	}

	/*
	 * check if default trace instance is being closed,
	 * if yes then update default destination to '0'
	 */
	if (trc_tbl.default_dst == idx)
		trc_tbl.default_dst = DST_TR_EVENT;

	trace_array_put(buf->arr);
	/*
	 * don't destroy trace instance but let user do it manually
	 * with rmdir command at a convenient time later, if it is
	 * destroyed here all debug logs will be lost
	 *
	 * trace_array_destroy(inst->arr);
	 */
	buf->arr = NULL;

	kfree(buf->name);
	buf->name = NULL;

	clear_bit(idx, trc_tbl.bmap);
	v3pr_info("closed trace instance idx=%d, name=%s\n", idx, arg);
end:
	mutex_unlock(&ddebug_lock);
	return ret;
}

static
void update_tr_dst(const struct _ddebug *desc, const struct dd_ctrl *nctrl)
{
	int odst = get_trace_dst(desc);
	int ndst = nctrl->trace_dst;

	if (odst == ndst)
		return;

	if (odst)
		trc_tbl.buf[odst].use_cnt--;

	if (ndst)
		trc_tbl.buf[ndst].use_cnt++;
}


/*
 * Selects trace destination. If callsite's current trace destination is '0'
 * and user provided T flag without trace destination name then set callsite's
 * trace destination to default trace destination. If a user provided trace
 * destination name :trace_dest_name with or without T flag then set callsite's
 * trace destination to the provided name. Otherwise keep callsite's current
 * trace destination.
 */
static int select_tr_dst(struct flag_settings *modifiers, int cur_dst)
{
	if (!cur_dst && modifiers->flags & _DPRINTK_FLAGS_TRACE &&
	    modifiers->trace_dst == DST_NOT_SET)
		return trc_tbl.default_dst;

	if (modifiers->trace_dst >= 0)
		return modifiers->trace_dst;

	return cur_dst;
}

static int ddebug_parse_cmd(char *words[], int nwords)
{
	if (nwords != 1)
		return -EINVAL;

	if (!strcmp(words[0], DD_OPEN_CMD))
		return handle_trace_open_cmd(words[1]);
	if (!strcmp(words[0], DD_CLOSE_CMD))
		return handle_trace_close_cmd(words[1]);

	pr_err("invalid command %s\n", words[0]);
	return -EINVAL;
}

#define vpr_dt_info(dt_p, msg_p, ...) ({				\
	struct ddebug_table const *_dt = dt_p;				\
	v2pr_info(msg_p " module:%s nd:%d nc:%d nu:%d\n", ##__VA_ARGS__, \
		  _dt->mod_name, _dt->num_ddebugs, _dt->num_classes,	\
		  _dt->num_class_users);				\
	})

static int ddebug_find_valid_class(struct ddebug_table const *dt, const char *class_string)
{
	struct ddebug_class_map *map;
	struct ddebug_class_user *cli;
	int i, idx;

	for (i = 0, map = dt->classes; i < dt->num_classes; i++, map++) {
		idx = match_string(map->class_names, map->length, class_string);
		if (idx >= 0) {
			vpr_dt_info(dt, "good-class: %s.%s ", map->mod_name, class_string);
			return idx + map->base;
		}
	}
	for (i = 0, cli = dt->class_users; i < dt->num_class_users; i++, cli++) {
		idx = match_string(cli->map->class_names, cli->map->length, class_string);
		if (idx >= 0) {
			vpr_dt_info(dt, "class-ref: %s.%s ",
				    cli->mod_name, class_string);
			return idx + cli->map->base;
		}
	}
	return -ENOENT;
}

/*
 * Search the tables for _ddebug's which match the given `query' and
 * apply the `flags' and `mask' to them.  Returns number of matching
 * callsites, normally the same as number of changes.  If verbose,
 * logs the changes.
 */
static int ddebug_change(const struct ddebug_query *query, struct flag_settings *modifiers)
{
	int i;
	struct ddebug_table *dt;
	unsigned int nfound = 0;
	struct ctrlbuf cbuf, nbuf;
	struct dd_ctrl nctrl = {0};
	int valid_class;

	/* search for matching ddebugs */
	list_for_each_entry(dt, &ddebug_tables, link) {

		/* match against the module name */
		if (query->module &&
		    !match_wildcard(query->module, dt->mod_name))
			continue;

		if (query->class_string) {
			valid_class = ddebug_find_valid_class(dt, query->class_string);
			if (valid_class < 0)
				/* skip/reject unknown classes */
				continue;
		} else {
			valid_class = _DPRINTK_CLASS_ANY;
		}

		for (i = 0; i < dt->num_ddebugs; i++) {
			struct _ddebug *dp = &dt->ddebugs[i];
			/*
			 * skip site if its class isn't the one
			 * queried, unless its the default class.
			 * 2nd term drops protection of class'd
			 * prdbgs from unclassed queries.
			 */
			if (dp->class_id != valid_class &&
			    valid_class != _DPRINTK_CLASS_ANY)
				continue;

			/* match against the source filename */
			if (query->filename &&
			    !match_wildcard(query->filename, dp->filename) &&
			    !match_wildcard(query->filename,
					   kbasename(dp->filename)) &&
			    !match_wildcard(query->filename,
					   trim_prefix(dp->filename)))
				continue;

			/* match against the function */
			if (query->function &&
			    !match_wildcard(query->function, dp->function))
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

			nctrl.flags = (get_flags(dp) & modifiers->mask) | modifiers->flags;
			nctrl.trace_dst = select_tr_dst(modifiers, get_trace_dst(dp));
			if (!memcmp(&nctrl, get_ctrl(dp), sizeof(nctrl)))
				continue;
#ifdef CONFIG_JUMP_LABEL
			if (get_flags(dp) & _DPRINTK_FLAGS_ENABLED) {
				if (!(nctrl.flags & _DPRINTK_FLAGS_ENABLED))
					static_branch_disable(&dp->key.dd_key_true);
			} else if (nctrl.flags & _DPRINTK_FLAGS_ENABLED) {
				static_branch_enable(&dp->key.dd_key_true);
			}
#endif
			v4pr_info("changed %s:%d [%s]%s %s => %s\n",
				  trim_prefix(dp->filename), dp->lineno,
				  dt->mod_name, dp->function,
				  ddebug_describe_ctrl(&dp->ctrl, &cbuf),
				  ddebug_describe_ctrl(&nctrl, &nbuf));
			update_tr_dst(dp, &nctrl);
			set_ctrl(dp, &nctrl);
		}
	}

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
	read_flag_args_f read_args;
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
				read_args = opt_array[i].read_args;
				if (read_args) {
					str = read_args(str, modifiers);
					if (!str)
						return -EINVAL;
				}
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

	v3pr_info("op='%c' flags=0x%x mask=0x%x, trace_dest=0x%x\n", op,
		  modifiers->flags, modifiers->mask, modifiers->trace_dst);
	return 0;
}

static int ddebug_exec_query(char *query_string, const char *modname)
{
	struct flag_settings modifiers = { .trace_dst = DST_NOT_SET };
	struct ddebug_query query = {};
#define MAXWORDS 9
	int nwords, nfound;
	char *words[MAXWORDS];

	nwords = ddebug_tokenize(query_string, words, MAXWORDS);
	if (nwords <= 0) {
		pr_err("tokenize failed\n");
		return -EINVAL;
	}

	/* check for open, close commands */
	if (is_dd_trace_cmd(words[0]))
		return ddebug_parse_cmd(words, nwords-1);

	if (ddebug_parse_query(words, nwords-1, &query, modname)) {
		pr_err("query parse failed\n");
		return -EINVAL;
	}

	/*
	 * Both ddebug_parse_flags and ddebug_change have to be run
	 * with locking because ddebug_parse_flags->read_T_args verifies
	 * if a requested trace instance is opened. Next ddebug_change
	 * sets the requested trace instance as output for debug logs of
	 * callsite(s) which matched a query. Because ddebug_proc_write
	 * function can be called concurrently then without locking we
	 * could for example run into a scenario where ddebug_change uses
	 * a trace instance which is already closed.
	 */
	mutex_lock(&ddebug_lock);

	/* check flags 1st (last arg) so query is pairs of spec,val */
	if (ddebug_parse_flags(words[nwords-1], &modifiers)) {
		pr_err("flags parse failed\n");
		goto err;
	}

	/* actually go and implement the change */
	nfound = ddebug_change(&query, &modifiers);

	mutex_unlock(&ddebug_lock);
	vpr_info_dq(&query, nfound ? "applied" : "no-match");
	return nfound;
err:
	mutex_unlock(&ddebug_lock);
	return -EINVAL;
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
static int ddebug_apply_class_bitmap(const struct ddebug_class_param *dcp,
				     const unsigned long *new_bits,
				     const unsigned long old_bits,
				     const char *query_modname)
{
#define QUERY_SIZE 128
	char query[QUERY_SIZE];
	const struct ddebug_class_map *map = dcp->map;
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
	const struct ddebug_class_param *dcp = kp->arg;
	const struct ddebug_class_map *map = dcp->map;

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
					   const char *modnm)
{
	const struct ddebug_class_param *dcp = kp->arg;
	const struct ddebug_class_map *map = dcp->map;
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
		v2pr_info("bits:0x%lx > %s.%s\n", inrep, modnm ?: "*", KP_NAME(kp));
		totct += ddebug_apply_class_bitmap(dcp, &inrep, *dcp->bits, modnm);
		*dcp->bits = inrep;
		break;
	case DD_CLASS_TYPE_LEVEL_NUM:
		old_bits = CLASSMAP_BITMASK(*dcp->lvl);
		new_bits = CLASSMAP_BITMASK(inrep);
		v2pr_info("lvl:%ld bits:0x%lx > %s\n", inrep, new_bits, KP_NAME(kp));
		totct += ddebug_apply_class_bitmap(dcp, &new_bits, old_bits, modnm);
		*dcp->lvl = inrep;
		break;
	default:
		pr_warn("%s: bad map type: %d\n", KP_NAME(kp), map->map_type);
	}
	vpr_info("%s: total matches: %d\n", KP_NAME(kp), totct);
	return 0;
}

/**
 * param_set_dyndbg_classes - set all classes in a classmap
 * @instr: string echo>d to sysfs, input depends on map_type
 * @kp:    kp->arg has state: bits/lvl, classmap, map_type
 *
 * For all classes in the classmap, enable/disable them per the input
 * (depending on map_type).  For LEVEL map-types, enforce relative
 * levels by bitpos.
 *
 * Returns: 0 or <0 if error.
 */
int param_set_dyndbg_classes(const char *instr, const struct kernel_param *kp)
{
	return param_set_dyndbg_module_classes(instr, kp, NULL);
}
EXPORT_SYMBOL(param_set_dyndbg_classes);

/**
 * param_get_dyndbg_classes - classes reader
 * @buffer: string description of controlled bits -> classes
 * @kp:     kp->arg has state: bits, map
 *
 * Reads last written state, underlying prdbg state may have been
 * altered by direct >control.  Displays 0x for DISJOINT, 0-N for
 * LEVEL Returns: #chars written or <0 on error
 */
int param_get_dyndbg_classes(char *buffer, const struct kernel_param *kp)
{
	const struct ddebug_class_param *dcp = kp->arg;
	const struct ddebug_class_map *map = dcp->map;

	switch (map->map_type) {
	case DD_CLASS_TYPE_DISJOINT_BITS:
		return scnprintf(buffer, PAGE_SIZE, "0x%lx\n", *dcp->bits);
	case DD_CLASS_TYPE_LEVEL_NUM:
		return scnprintf(buffer, PAGE_SIZE, "%ld\n", *dcp->lvl);
	default:
		return -1;
	}
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

static char *__dynamic_emit_prefix(const struct _ddebug *desc, char *buf)
{
	unsigned int flags = get_flags(desc);
	int pos_after_tid;
	int pos = 0;

	if (flags & _DPRINTK_FLAGS_INCL_TID) {
		if (in_interrupt())
			pos += snprintf(buf + pos, remaining(pos), "<intr> ");
		else
			pos += snprintf(buf + pos, remaining(pos), "[%d] ",
					task_pid_vnr(current));
	}
	pos_after_tid = pos;
	if (flags & _DPRINTK_FLAGS_INCL_MODNAME)
		pos += snprintf(buf + pos, remaining(pos), "%s:",
				desc->modname);
	if (flags & _DPRINTK_FLAGS_INCL_FUNCNAME)
		pos += snprintf(buf + pos, remaining(pos), "%s:",
				desc->function);
	if (flags & _DPRINTK_FLAGS_INCL_SOURCENAME)
		pos += snprintf(buf + pos, remaining(pos), "%s:",
				trim_prefix(desc->filename));
	if (flags & _DPRINTK_FLAGS_INCL_LINENO)
		pos += snprintf(buf + pos, remaining(pos), "%d:",
				desc->lineno);
	if (pos - pos_after_tid)
		pos += snprintf(buf + pos, remaining(pos), " ");
	if (pos >= PREFIX_SIZE)
		buf[PREFIX_SIZE - 1] = '\0';

	return buf;
}

static inline char *dynamic_emit_prefix(struct _ddebug *desc, char *buf)
{
	if (unlikely(get_flags(desc) & _DPRINTK_FLAGS_INCL_ANY))
		return __dynamic_emit_prefix(desc, buf);
	return buf;
}

/*
 * This code is heavily based on __ftrace_trace_stack().
 *
 * Allow 4 levels of nesting: normal, softirq, irq, NMI.
 */
#define DYNAMIC_TRACE_NESTING	4

struct ddebug_trace_buf {
	char buf[256];
};

struct ddebug_trace_bufs {
	struct ddebug_trace_buf bufs[DYNAMIC_TRACE_NESTING];
};

static DEFINE_PER_CPU(struct ddebug_trace_bufs, ddebug_trace_bufs);
static DEFINE_PER_CPU(int, ddebug_trace_reserve);

__printf(3, 0)
static void ddebug_trace_event(struct _ddebug *desc, const struct device *dev,
			       const char *fmt, va_list args)
{
	struct ddebug_trace_buf *buf;
	int bufidx;

	preempt_disable_notrace();

	bufidx = __this_cpu_inc_return(ddebug_trace_reserve) - 1;

	if (WARN_ON_ONCE(bufidx > DYNAMIC_TRACE_NESTING))
		goto out;

	/* For the same reasons as in __ftrace_trace_stack(). */
	barrier();

	buf = this_cpu_ptr(ddebug_trace_bufs.bufs) + bufidx;

	vscnprintf(buf->buf, sizeof(buf->buf), fmt, args);

	if (!dev)
		trace_prdbg(desc, NULL, buf->buf);
	else
		trace_devdbg(desc, dev, buf->buf);

out:
	/* As above. */
	barrier();
	__this_cpu_dec(ddebug_trace_reserve);
	preempt_enable_notrace();
}

__printf(2, 0)
static void ddebug_trace_instance(struct _ddebug *desc, const char *fmt,
				  va_list * args)
{
	struct va_format vaf = { .fmt = fmt, .va = args};
	struct trace_array *arr = trc_tbl.buf[get_trace_dst(desc)].arr;

	WARN_ON_ONCE(!arr);

	trace_array_printk(arr, 0, "%pV", &vaf);
}

__printf(2, 3)
static void ddebug_printk(struct _ddebug *desc, const char *fmt, ...)
{

	if (get_flags(desc) & _DPRINTK_FLAGS_TRACE) {
		va_list args;

		va_start(args, fmt);
		/*
		 * All callers include the KERN_DEBUG prefix to keep the
		 * vprintk case simple; strip it out for tracing.
		 */
		if (!get_trace_dst(desc))
			ddebug_trace_event(desc, NULL,
					   fmt + strlen(KERN_DEBUG), args);
		else
			ddebug_trace_instance(desc, fmt + strlen(KERN_DEBUG),
					      &args);
		va_end(args);
	}

	if (get_flags(desc) & _DPRINTK_FLAGS_PRINTK) {
		va_list args;

		va_start(args, fmt);
		vprintk(fmt, args);
		va_end(args);
	}
}

__printf(3, 4)
static void ddebug_dev_printk(struct _ddebug *desc, const struct device *dev,
			      const char *fmt, ...)
{

	if (get_flags(desc) & _DPRINTK_FLAGS_TRACE) {
		va_list args;

		va_start(args, fmt);
		if (!get_trace_dst(desc))
			ddebug_trace_event(desc, dev, fmt, args);
		else
			ddebug_trace_instance(desc, fmt, &args);
		va_end(args);
	}

	if (get_flags(desc) & _DPRINTK_FLAGS_PRINTK) {
		va_list args;

		va_start(args, fmt);
		dev_vprintk_emit(LOGLEVEL_DEBUG, dev, fmt, args);
		va_end(args);
	}
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

	ddebug_printk(descriptor, KERN_DEBUG "%s%pV",
		      dynamic_emit_prefix(descriptor, buf), &vaf);

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
		ddebug_printk(descriptor, KERN_DEBUG "(NULL device *): %pV",
			       &vaf);
	} else {
		char buf[PREFIX_SIZE] = "";

		ddebug_dev_printk(descriptor, dev, "%s%s %s: %pV",
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

		ddebug_dev_printk(descriptor, dev->dev.parent,
				  "%s%s %s %s%s: %pV",
				  dynamic_emit_prefix(descriptor, buf),
				  dev_driver_string(dev->dev.parent),
				  dev_name(dev->dev.parent),
				  netdev_name(dev), netdev_reg_state(dev),
				  &vaf);
	} else if (dev) {
		ddebug_dev_printk(descriptor, &dev->dev, KERN_DEBUG "%s%s: %pV",
				  netdev_name(dev), netdev_reg_state(dev), &vaf);
	} else {
		ddebug_printk(descriptor, KERN_DEBUG "(NULL net_device): %pV", &vaf);
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

		ddebug_dev_printk(descriptor, ibdev->dev.parent,
				  "%s%s %s %s: %pV",
				  dynamic_emit_prefix(descriptor, buf),
				  dev_driver_string(ibdev->dev.parent),
				  dev_name(ibdev->dev.parent),
				  dev_name(&ibdev->dev),
				  &vaf);
	} else if (ibdev) {
		ddebug_dev_printk(descriptor, &ibdev->dev, KERN_DEBUG "%s: %pV",
				  dev_name(&ibdev->dev), &vaf);
	} else {
		ddebug_printk(descriptor, KERN_DEBUG "(NULL ip_device): %pV", &vaf);
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
	iter->idx = iter->table->num_ddebugs;
	return &iter->table->ddebugs[--iter->idx];
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
		iter->idx = iter->table->num_ddebugs;
		--iter->idx;
	}
	return &iter->table->ddebugs[iter->idx];
}

/*
 * Check if the iterator points to the last _ddebug object
 * to traverse.
 */
static bool ddebug_iter_is_last(struct ddebug_iter *iter)
{
	if (iter->table == NULL)
		return false;
	if (iter->idx-1 < 0 &&
	    list_is_last(&iter->table->link, &ddebug_tables))
		return true;
	return false;
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

static const char *ddebug_class_name(struct ddebug_table *dt, struct _ddebug *dp)
{
	struct ddebug_class_map *map = dt->classes;
	struct ddebug_class_user *cli = dt->class_users;
	int i;

	for (i = 0; i < dt->num_classes; i++, map++)
		if (class_in_range(dp->class_id, map))
			return map->class_names[dp->class_id - map->base];

	for (i = 0; i < dt->num_class_users; i++, cli++)
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
	struct ctrlbuf cbuf;
	char const *class;

	if (p == SEQ_START_TOKEN) {
		seq_puts(m,
			 "# filename:lineno [module]function flags format\n");
		return 0;
	}

	seq_printf(m, "%s:%u [%s]%s =%s \"",
		   trim_prefix(dp->filename), dp->lineno,
		   iter->table->mod_name, dp->function,
		   ddebug_describe_ctrl(&dp->ctrl, &cbuf));
	seq_escape_str(m, dp->format, ESCAPE_SPACE, "\t\r\n\"");
	seq_puts(m, "\"");

	if (dp->class_id != _DPRINTK_CLASS_DFLT) {
		class = ddebug_class_name(iter->table, dp);
		if (class)
			seq_printf(m, " class:%s", class);
		else
			seq_printf(m, " class:_UNKNOWN_ _id:%d", dp->class_id);
	}
	seq_puts(m, "\n");

	if (ddebug_iter_is_last(iter)) {

		seq_puts(m, "\n");
		seq_printf(m, "#: Default trace destination: %s\n",
			   get_tr_default_dst_str());

		if (!bitmap_empty(trc_tbl.bmap, trc_tbl.bmap_size)) {
			int idx = 1;

			seq_puts(m, "\n");
			seq_puts(m, "#: Opened trace instances:");
			for_each_set_bit_from(idx, trc_tbl.bmap, trc_tbl.bmap_size)
				seq_printf(m, " %s", trc_tbl.buf[idx].name);
			seq_puts(m, "\n");
		}
	}

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

static const char * const ddebug_classmap_typenames[] = {
	"DISJOINT_BITS", "LEVEL_NUM"
};

#define vpr_cm_info(cm_p, msg_fmt, ...) ({				\
	struct ddebug_class_map const *_cm = cm_p;			\
	v2pr_info(msg_fmt " [%d..%d] %s..%s\n", ##__VA_ARGS__,		\
		  _cm->base, _cm->base + _cm->length,			\
		  _cm->class_names[0],					\
		  _cm->class_names[_cm->length - 1]);			\
	})

static void ddebug_sync_classbits(const struct kernel_param *kp, const char *modname)
{
	struct ddebug_class_param *dcp = kp->arg;
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

static void ddebug_match_apply_kparam(const struct kernel_param *kp,
				      const struct ddebug_class_map *map,
				      const char *modnm)
{
	struct ddebug_class_param *dcp;

	if (kp->ops != &param_ops_dyndbg_classes)
		return;

	dcp = (struct ddebug_class_param *)kp->arg;

	if (map == dcp->map) {
		v2pr_info(" found kp:%s =0x%lx", kp->name, *dcp->bits);
		vpr_cm_info(map, "  mapped to: %s", modnm);
		ddebug_sync_classbits(kp, modnm);
	}
}

static void ddebug_apply_params(const struct ddebug_class_map *cm, const char *modnm)
{
	const struct kernel_param *kp;
#if IS_ENABLED(CONFIG_MODULES)
	int i;

	if (cm->mod) {
		vpr_cm_info(cm, "loaded classmap: %s", modnm);
		/* ifdef protects the cm->mod->kp deref */
		for (i = 0, kp = cm->mod->kp; i < cm->mod->num_kp; i++, kp++)
			ddebug_match_apply_kparam(kp, cm, modnm);
	}
#endif
	if (!cm->mod) {
		vpr_cm_info(cm, "builtin classmap: %s", modnm);
		for (kp = __start___param; kp < __stop___param; kp++)
			ddebug_match_apply_kparam(kp, cm, modnm);
	}
}

static int ddebug_class_range_overlap(struct ddebug_class_map *cm,
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
 * simplify a repeated for-loop pattern walking N steps in a T _vec
 * member inside a struct _box.  It expects int i and T *_sp to be
 * declared in the caller.
 * @_i:  caller provided counter.
 * @_sp: cursor into _vec, to examine each item.
 * @_vec: name of array-ref member in _box.
 * @_box: ptr to a struct containing _vec, num__##_vec fields.
 */
#define for_subvec(_i, _sp, _box, _vec)					\
	for (_i = 0, (_sp) = (_box)->_vec;				\
	     _i < (_box)->num_##_vec;					\
	     _i++, (_sp)++)
/*
 * scan the linker-ordered @_vec, of num_##_vec elements, for the
 * start,len of the elements matching on ->mod_name; remember them in
 * _dst.  Macro depends upon the fields being in both _box and _dst.
 * @_i:   caller provided counter.
 * @_sp:  cursor into @_vec.
 * @_box: ptr to a struct with @_vec, num__##@_vec, mod_name fields.
 * @_vec: name of ref into array[T] of builtin/modular __section data.
 * @_dst: ptr to struct with @_vec and num__##@_vec fields, both updated.
 */
#define __evalout /* macro "return" val */
#define dd_mark_vector_subrange(_i, _dst, _sp, _box, _vec) ({		\
	int nc = 0;							\
	for_subvec(_i, _sp, _box, _vec) {				\
		if (!strcmp((_sp)->mod_name, (_dst)->mod_name)) {	\
			if (!nc++)					\
				(_dst)->_vec = (_sp);			\
		} else {						\
			if (nc)						\
				break; /* end of consecutive matches */ \
		}							\
	}								\
	__evalout _dst->num_##_vec = nc;				\
})

static int ddebug_attach_module_classes(struct ddebug_table *dt,
					const struct _ddebug_info *di,
					u64 *reserved_ids)
{
	struct ddebug_class_map *cm;
	int i;

	for_subvec(i, cm, di, classes) {
		if (ddebug_class_range_overlap(cm, reserved_ids))
			return -EINVAL;
		ddebug_apply_params(cm, cm->mod_name);
	}
	vpr_info("module:%s attached %d classmaps\n", dt->mod_name, dt->num_classes);
	return 0;
}

static int ddebug_attach_user_module_classes(struct ddebug_table *dt,
					     const struct _ddebug_info *di,
					     u64 *reserved_ids)
{
	struct ddebug_class_user *cli;
	int i;

	for_subvec(i, cli, dt, class_users) {
		if (ddebug_class_range_overlap(cli->map, reserved_ids))
			return -EINVAL;
		ddebug_apply_params(cli->map, cli->mod_name);
	}
	vpr_info("module:%s attached %d classmap uses\n", dt->mod_name, dt->num_class_users);
	return 0;
}

/*
 * Allocate a new ddebug_table for the given module
 * and add it to the global list.
 */
static int ddebug_add_module(struct _ddebug_info *di, const char *modname)
{
	struct ddebug_table *dt;
	struct ddebug_class_map *cm;
	struct ddebug_class_user *cli;
	u64 reserved_ids = 0;
	int rc, i;

	if (!di->num_descs)
		return 0;

	v3pr_info("add-module: %s %d sites\n", modname, di->num_descs);

	dt = kzalloc(sizeof(*dt), GFP_KERNEL);
	if (dt == NULL) {
		pr_err("error adding module: %s\n", modname);
		return -ENOMEM;
	}
	/*
	 * For built-in modules, name lives in .rodata and is
	 * immortal. For loaded modules, name points at the name[]
	 * member of struct module, which lives at least as long as
	 * this struct ddebug_table.
	 */
	dt->mod_name = modname;
	dt->ddebugs = di->descs;
	dt->num_ddebugs = di->num_descs;

	INIT_LIST_HEAD(&dt->link);
	/*
	 * for builtin modules, ddebug_init() insures that the di
	 * cursor marks just the module's descriptors, but it doesn't
	 * do so for the builtin class _maps & _users.  find the
	 * start,len of the vectors by mod_name, save to dt.
	 */
	dd_mark_vector_subrange(i, dt, cm, di, classes);
	dd_mark_vector_subrange(i, dt, cli, di, class_users);

	if (dt->num_classes) {
		rc = ddebug_attach_module_classes(dt, di, &reserved_ids);
		if (rc) {
			kfree(dt);
			return rc;
		}
	}

	mutex_lock(&ddebug_lock);
	list_add_tail(&dt->link, &ddebug_tables);
	mutex_unlock(&ddebug_lock);

	if (dt->num_class_users) {
		rc = ddebug_attach_user_module_classes(dt, di, &reserved_ids);
		if (rc)
			return rc;
	}
	vpr_info("%3u debug prints in module %s\n", di->num_descs, modname);
	return 0;
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
	int dst, i;

	for (i = 0; i < dt->num_ddebugs; i++) {
		dst = get_trace_dst(&dt->ddebugs[i]);
		if (dst)
			trc_tbl.buf[dst].use_cnt--;
	}

	list_del_init(&dt->link);
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
		if (dt->mod_name == mod_name) {
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
		ret = ddebug_add_module(&mod->dyndbg_info, mod->name);
		if (ret == -EINVAL) {
			pr_err("conflicting dyndbg-classmap reservations\n");
			ddebug_remove_module(mod->name);
			break;
		}
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

static int __init dynamic_debug_init(void)
{
	struct _ddebug *iter, *iter_mod_start;
	int ret, i, mod_sites, mod_ct;
	const char *modname;
	char *cmdline;

	struct _ddebug_info di = {
		.descs = __start___dyndbg,
		.classes = __start___dyndbg_classes,
		.class_users = __start___dyndbg_class_users,
		.num_descs = __stop___dyndbg - __start___dyndbg,
		.num_classes = __stop___dyndbg_classes - __start___dyndbg_classes,
		.num_class_users = __stop___dyndbg_class_users - __start___dyndbg_class_users,
	};

#ifdef CONFIG_MODULES
	ret = register_module_notifier(&ddebug_module_nb);
	if (ret) {
		pr_warn("Failed to register dynamic debug module notifier\n");
		return ret;
	}
#endif /* CONFIG_MODULES */

	if (&__start___dyndbg == &__stop___dyndbg) {
		if (IS_ENABLED(CONFIG_DYNAMIC_DEBUG)) {
			pr_warn("_ddebug table is empty in a CONFIG_DYNAMIC_DEBUG build\n");
			return 1;
		}
		pr_info("Ignore empty _ddebug table in a CONFIG_DYNAMIC_DEBUG_CORE build\n");
		ddebug_init_success = 1;
		return 0;
	}

	iter = iter_mod_start = __start___dyndbg;
	modname = iter->modname;
	i = mod_sites = mod_ct = 0;

	for (; iter < __stop___dyndbg; iter++, i++, mod_sites++) {

		if (strcmp(modname, iter->modname)) {
			mod_ct++;
			di.num_descs = mod_sites;
			di.descs = iter_mod_start;
			ret = ddebug_add_module(&di, modname);
			if (ret)
				goto out_err;

			mod_sites = 0;
			modname = iter->modname;
			iter_mod_start = iter;
		}
	}
	di.num_descs = mod_sites;
	di.descs = iter_mod_start;
	ret = ddebug_add_module(&di, modname);
	if (ret)
		goto out_err;

	ddebug_init_success = 1;
	vpr_info("%d prdebugs in %d modules, %d KiB in ddebug tables, %d kiB in __dyndbg section\n",
		 i, mod_ct, (int)((mod_ct * sizeof(struct ddebug_table)) >> 10),
		 (int)((i * sizeof(struct _ddebug)) >> 10));

	if (di.num_classes)
		v2pr_info("  %d builtin ddebug class-maps\n", di.num_classes);

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
