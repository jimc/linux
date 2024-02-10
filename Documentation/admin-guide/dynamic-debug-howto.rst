Dynamic debug
+++++++++++++


Introduction
============

Dynamic debug allows you to dynamically enable/disable kernel
debug-print code to obtain additional kernel information.

If ``/proc/dynamic_debug/control`` exists, your kernel has dynamic
debug.  You'll need root access (sudo su) to use this.

Dynamic debug provides:

 * a Catalog of all *prdbgs* in your kernel.
   ``cat /proc/dynamic_debug/control`` to see them.

 * a Simple query/command language to alter groups (or singles) of
   *prdbgs* by selecting on any combination of 0 or 1 of:

   - source filename
   - function name
   - line number (including ranges of line numbers)
   - module name
   - format string
   - class name (as known/declared by each module)
   - label name (trace-label, as determined by user)

NOTE: To actually get the debug-print output on the console, you may
need to adjust the kernel ``loglevel=``, or use ``ignore_loglevel``.
Read about these kernel parameters in
Documentation/admin-guide/kernel-parameters.rst.

Viewing Dynamic Debug Behaviour
===============================

You can view the currently configured behaviour in the *prdbg* catalog::

  :#> head -n7 /proc/dynamic_debug/control
  # filename:lineno [module]function flags format
  init/main.c:1179 [main]initcall_blacklist =_ "blacklisting initcall %s\012
  init/main.c:1218 [main]initcall_blacklisted =_ "initcall %s blacklisted\012"
  init/main.c:1424 [main]run_init_process =_ "  with arguments:\012"
  init/main.c:1426 [main]run_init_process =_ "    %s\012"
  init/main.c:1427 [main]run_init_process =_ "  with environment:\012"
  init/main.c:1429 [main]run_init_process =_ "    %s\012"

The 3rd space-delimited column shows the current flags, preceded by
a ``=`` for easy use with grep/cut. ``=p`` shows enabled callsites.

Controlling dynamic debug Behaviour
===================================

The behaviour of *prdbg* sites are controlled by writing
query/commands to the control file.  Example::

  # grease the interface
  :#> alias ddcmd='echo $* > /proc/dynamic_debug/control'

  :#> ddcmd '-p; module main func run* +p'	# disable all, then enable main
  :#> grep =p /proc/dynamic_debug/control
  init/main.c:1424 [main]run_init_process =p "  with arguments:\n"
  init/main.c:1426 [main]run_init_process =p "    %s\n"
  init/main.c:1427 [main]run_init_process =p "  with environment:\n"
  init/main.c:1429 [main]run_init_process =p "    %s\n"

Error messages go to console/syslog::

  :#> ddcmd mode foo +p
  dyndbg: unknown keyword "mode"
  dyndbg: query parse failed
  bash: echo: write error: Invalid argument

If debugfs is also enabled and mounted, ``dynamic_debug/control`` is
also under the mount-dir, typically ``/sys/kernel/debug/``.

Command Language Reference
==========================

At the basic lexical level, a command is a sequence of words separated
by spaces, tabs, or commas.  So these are all equivalent::

  :#> ddcmd file svcsock.c line 1603 +p
  :#> ddcmd "file svcsock.c line 1603 +p"
  :#> ddcmd '  file   svcsock.c     line  1603 +p  '
  :#> ddcmd file,svcsock.c,line,1603,+p

Command submissions are bounded by a write() system call.  Multiple
commands can be sent together in a CMD_BLK, each separated by ``%``,
``;`` or ``\n``::

  :#> ddcmd func foo +p % func bar +p
  :#> ddcmd "func foo +p ; func bar +p"
  :#> ddcmd <<CMD_BLK
  func pnpacpi_get_resources +p
  func pnp_assign_mem +p
  CMD_BLK
  :#> cat query-batch-file > /proc/dynamic_debug/control

You can also use wildcards in each query term. The match rule supports
``*`` (matches zero or more characters) and ``?`` (matches exactly one
character). For example, you can match all usb drivers::

  :#> ddcmd file "drivers/usb/*" +p	# "" to suppress shell expansion

Syntactically, a query/command is 0 or more pairs of keyword values,
followed by a flags change or setting::

  command ::= match-spec* flags-spec

The match-spec's select *prdbgs* from the catalog, upon which to apply
the flags-spec, all constraints are ANDed together.  An absent keyword
is the same as keyword "*".

Note: because the match-spec can be empty, the flags are checked 1st,
then the pairs of keyword values.  Flag errs will hide keyword errs:

  bash-5.2# ddcmd mod bar +foo
  dyndbg: read 13 bytes from userspace
  dyndbg: query 0: "mod bar +foo" mod:*
  dyndbg: unknown flag 'o'
  dyndbg: flags parse failed
  dyndbg: processed 1 queries, with 0 matches, 1 errs

non-query commands support connection to tracefs:

  command ::= open <name>
  command ::= close <name>

Match specification
===================

A match specification is a keyword, which selects the attribute of
the callsite to be compared, and a value to compare against.  Possible
keywords are:::

  match-spec ::= 'func' string |
		 'file' string |
		 'module' string |
		 'format' string |
		 'class' string |
		 'label' string |
		 'line' line-range

  line-range ::= lineno |
		 '-'lineno |
		 lineno'-' |
		 lineno'-'lineno

  lineno ::= unsigned-int

.. note::

  ``line-range`` cannot contain space, e.g.
  "1-30" is valid range but "1 - 30" is not.

The meanings of each keyword are:

func <func_name>
    The func_name is compared against the function name of each
    callsite.  Example::

	func svc_tcp_accept
	func *recv*		# in rfcomm, bluetooth, ping, tcp

file <file_name>
    The file_name is compared against either the src-root relative
    pathname, or the basename of the source file of each callsite.
    Examples::

	file svcsock.c
	file kernel/freezer.c	# ie column 1 of control file
	file drivers/usb/*	# all callsites under it
	file inode.c:start_*	# parse :tail as a func (above)
	file inode.c:1-100	# parse :tail as a line-range (above)

module <mod_name>
    The mod_name is compared to each callsites mod_name, as seen in
    ``lsmod``, i.e. without the directory or the ``.ko`` suffix and
    with ``-`` changed to ``_``.

    Examples::

	module,sunrpc	# with ',' as token separator
	module nfsd
	module drm*	# both drm, drm_kms_helper

format <fmtstr>
    The given string is searched for in the dynamic debug format
    string.  Note that the string does not need to match the
    entire format, only some part.  Whitespace and other
    special characters can be escaped using C octal character
    escape ``\ooo`` notation, e.g. the space character is ``\040``.
    Alternatively, the string can be enclosed in double quote
    characters (``"``) or single quote characters (``'``).
    Examples::

	format svcrdma:         // many of the NFS/RDMA server pr_debugs
	format readahead        // some pr_debugs in the readahead cache
	format nfsd:\040SETATTR // one way to match a format with whitespace
	format "nfsd: SETATTR"  // a neater way to match a format with whitespace
	format 'nfsd: SETATTR'  // yet another way to match a format with whitespace

class <cl_name>
    The cl_name is validated against each module, which may have
    declared a list of class_names it knows.  If the cl_name is known
    by a module, site matching and site flags adjustment proceeds.
    Examples::

	class DRM_UT_KMS	# a DRM.debug category
	class JUNK		# silent non-match
	# class TLD_*		# NOTICE: no wildcard in class names

line <ln_spec>
    The given line number or range of line numbers is compared
    against the line number of each ``pr_debug()`` callsite.  A single
    line number matches the callsite line number exactly.  A
    range of line numbers matches any callsite between the first
    and last line number inclusive.  An empty first number means
    the first line in the file, an empty last line number means the
    last line number in the file.  Examples::

	line 1603           // exactly line 1603
	line 1600-1605      // the six lines from line 1600 to line 1605
	line -1605          // the 1605 lines from line 1 to line 1605
	line 1600-          // all lines from line 1600 to the end of the file

label <lbl_name>
    This matches the lbl_name against each callsite's current label
    (the default is "0").  This allows a user to select and enable a
    previously labelled set of callsites, after assembling a group label
    to express the "relatedness" they perceive.

Flags Specification
===================

The flags-spec is a change operation followed by one or more flag
characters.  The change operation is one of the characters::

  -    disable these flags
  +    enable these flags
  =    set these flags

The primary flags are::

  p    print to syslog
  T    write to tracefs
  _    no-flags (allows positive assertion of no-flags)
  :    trace-label pseudo-flag (see below)

The prefix flags append callsite info to each site's dynamic-prefix,
in the order shown below, (with '':'' between each).  That is then
prepended to the pr_debug message, for both sylog and tracefs.

  t    thread ID, or <intr>
  m    module name
  f    the function name
  s    the source file name
  l    line number

Basic flag examples:

  # because match-spec can be empty, these are legal commands.
  =p    # output to syslog (on all sites)
  =T    # output to trace (on all sites)
  =_    # clear all flags (set them all off)
  +_    # add no flags [#nochgquery]_
  -_    # drop no flags [#nochgquery]_
  +mf   # set "module:function: " prefix
  +sl   # set "file:line: " prefix

[#nochgquery] these queries alter no flags, they are processed
normally.

Labelling pr_debug callsites
============================

Callsites can also be labelled, using the ``:<lbl_name>`` trace-label
pseudo-flag, and the following <lbl_name>.  This labels the callsite
with that name, allowing its later selection and enablement using the
"label" keyword.  From boot, the [#default_dest] and all callsite
labels are set to "0".

Labelling Examples:

  =T:0     # enable tracing to "0"/global explicitly
  =T:0.    # same, dot terminates lbl_name (optional here)
  =T:0.mf  # same, but add "module:func:" prefix to msgs (dot required)
  =T       # enable tracing to site.label (0 by default)

  =T:foo    # set labels to foo [#ifopened]_, send them to tracing/instances/foo
  =T:foo.mf # same, with "module:function:" prefix

  =_:foo    # clear all flags, set all labels to foo [#ifopened]_
  =:foo     # set labels, touch no flags, since no flags are given
  =:0       # reset all labels to global trace-buf
  =:0.      # same (with optional dot)

Labelling is primarily for mapping into tracing, but is syntactically
separate, and is allowed independently, so the label keyword can be
used to enable to syslog, or to both.

  =p:foo    # enable to syslog, p ignores foo
  =pT:foo   # trace to instances/foo, and to syslog

Debug output to Syslog and/or Tracefs
=====================================

Dynamic Debug can independently direct pr_debugs to both syslog and
tracefs, using the +p, +T flags respectively.  This allows users to
migrate away from syslog in bites, if and as they see a reason.

Dyndbg supports 64-way steering of pr_debugs into tracefs, by labelling
the callsites as described above.  You can steer trace traffic for any
number of reasons:

 - create a flight-recorder buffer.
 - isolate hi-rate traffic.
 - simplify buffer management and overwrite guarantees.
 - assemble "related" sets of prdbgs by labelling them.
 - select & enable them later, with "label" keyword.
 - just label some traffic as trash/uninteresting (>/dev/null?)
 - 63 private buffers are supported + global

The ``:0.`` default label steers output to the global trace-event buf:

   ddcmd open 0   # opened by default, also sets [#default_dest]_
   ddcmd =:0	  # steer pr_debugs to /sys/kernel/tracing/trace
   ddcmd =T	  # enable pr_debugs to their respective destinations

   # also need to enable the events into tracefs
   echo 1 > /sys/kernel/tracing/trace_on
   echo 1 > /sys/kernel/tracing/events/dyndbg/enable

Or the ``:<name>.`` labels steer +T enabled callsites into
/sys/kernel/tracing/instances/<name> [#ifopened]_

   ddcmd open foo	# open or append to /sys/kernel/tracing/instances/foo
   ddcmd =:foo		# set labels explicitly
   ddcmd =T		# enable tracing to site.label

   # needed if appending (above)
   echo 1 > /sys/kernel/tracing/instances/foo/trace_on
   echo 1 > /sys/kernel/tracing/instances/foo/events/dyndbg/enable

open foo & close foo
====================

The ``open foo`` & ``close foo`` commands allow dyndbg to manage the
63 private trace-instances it can use simultaneously, so it can error
with -ENOSPC when asked for one-too-many.  Otherwise, [#default_dest]
is upated accordingly.

[#ifopened] It is an error -EINVAL to set a label (=:foo) that hasn't
been previously opened.

[#already_opened] If /sys/kernel/tracing/instances/foo has already
been created separately, then dyndbg just uses it, mixing any =T:foo
labelled pr_debugs into instances/foo/trace.  Otherwise dyndbg will
open the trace-instance, and enable it for you.

Dyndbg treats ``:0.`` as the name of the global trace-event buffer; it
is automatically opened, but needs to be enabled in tracefs too.

If ``open bar`` fails (if bar was misspelled), the [#last_good_open]
is not what the user expects, so the open-cmd also terminates the
play-thru-query-errors strategy normally used over a CMD_BLK of
query-cmds.

``open 0`` always succeeds.

``close foo`` insures that no pr_debugs are set to :foo, then unmaps
the label from its reserved trace-id, preserving the trace buffer for
trace-cmd etc.  Otherwise the command returns -EBUSY.

Labeled Trace Examples
======================

Example 1:

Use 2 private trace instances to trivially segregate interesting debug.

  ddcmd open usbcore_buf	# create or share tracing/instances/usbcore_buf
  ddcmd module usbcore =T	# enable module usbcore to just opened instance

  ddcmd open tbt		# create or share tracing/instances/tbt
  ddcmd module thunderbolt =T	# enable module thunderbolt to just opened instance

Example 2:

RFC: This is plausible but aggressive conjecture, needs DRM-folk
review for potential utility.

  echo <<DRM_CMD_BLK > /proc/dynamic_debug/control

    # open 0		# automatically opened anyway
    open 0		# but also resets [#default_dest]_ to 0

      # send some traffic to global trace, to mix with other events

      class DRM_UT_KMS +T:0	# set label explicitly
      class DRM_UT_ATOMIC +T	# enable to site.label

    # label 2 classes together (presuming its useful)
    open drm_bulk	# open instances/drm_bulk/, set [#default_dest]_

      class DRM_UT_CORE +T		# implicit :drm_bulk
      class DRM_UT_DRIVER +T:drm_bulk	# explicit (but unnecessary)

    # capture DRM screen/layout changes
    open drm_screens
      class DRM_UT_LEASE +T	# all implied [#default_dest]_
      class DRM_UT_DP    +T
      class DRM_UT_DRMRES +T
      class DRM_UT_STATE  +T

    # mark traffic to ignore
    open trash			# will remain empty
      class junk -T		# cuz we disable the label

    open drm_vblank		# isolate hi-rate traffic
      class DRM_UT_VBL +T	# use drm_vblank (implicitly)

    # afterthought - add to drm_bulk
    class DRM_UT_DRIVER +T:drm_bulk	# explicit name needed here

    open 0	# reset [#default_dest]_ for next user

   DRM_CMD_BLK

This example uses +T (not =T) to enable pr_debugs to tracefs.  Doing
so preserves all other flags, so you can independently use +p for
syslog, and customize the shared prefix-flags per your personal whim
(or need), knowing they're not changed later spuriously.

NB: Dyndbg's support for DRM.debug uses ``+p`` & ``-p`` to toggle each
DRM_UT_* class by name, without altering any prefix customization you
might favor and apply.

This example also does explicit ``+T:<name>`` labelling more than
strictly needed, because it also mostly follows a repeating "open then
label" pattern, and could rely upon [#last_good_open] being set.  The
afterthought provides a counter-example.

Trash is handled by labelling and disabling certain traffic, so it is
never collected.  This will waste a trace instance, but it will stay
empty.

The extra ``open 0`` commands at the start & end of the DRM_CMD_BLK
explicitly reset the [#last_good_open], since ``open 0`` never fails.
This defensive practice prevents surprises when the next user expects
the "0" default (reasonably!) which enables to the global trace-buf.

Example 3: labelling 1st, deferred enable.

If the DRM_CMD_BLK above had replaced ``+T`` with ``-T``, then the
selected sites would get their labels set, but the trace-enable flag
is unset, and they are all trace-disabled.

This style lets a user aggregate an arbitrary set of "related"
pr_debugs.  Then those labels can be later selected and enabled
together:

  ddcmd label drm_screens +T	# enable tracing on the user's label
  ddcmd label drm_bulk +p	# works for syslog too

RFC:

Its practical to not require the open-1st if the trace instance
already exists, but auto-open of misspelled names would be an
anti-feature.

Also, without ``open foo`` required, theres no [#last_good_open], and
[#default_dest] must be set by explicit labelling at least once before
using [#default_dest] in following query-cmds.

Example 4:

This example opens interesting instances/labels 1st (perhaps at boot),
then labels several modules, and enables their pr_debugs to the
labelled trace-instances.

  echo <<ALT_BLK_STYLE > /proc/dynamic_debug/control
    open x;             # set [#default_dest]_ to x
    open y;             # set [#default_dest]_ to y
    open z              # set [#default_dest]_ to z
    module X  +T:z
    module X1 +T	# :z, use [#default_dest]_ implicitly
    module Y  +T:y
    module Z  +T:z
    module Z1 +T	# :z, use [#default_dest]_ implicitly
  ALT_BLK_STYLE

Debug messages during Boot Process
==================================

To activate debug messages for core code and built-in modules during
the boot process, even before userspace and debugfs exists, use
``dyndbg="QUERY"`` or ``module.dyndbg="QUERY"``.  QUERY follows
the syntax described above, but must not exceed 1023 characters.  Your
bootloader may impose lower limits.

These ``dyndbg`` params are processed just after the ddebug tables are
processed, as part of the early_initcall.  Thus you can enable debug
messages in all code run after this early_initcall via this boot
parameter.

On an x86 system for example ACPI enablement is a subsys_initcall and::

   dyndbg="file ec.c +p"

will show early Embedded Controller transactions during ACPI setup if
your machine (typically a laptop) has an Embedded Controller.
PCI (or other devices) initialization also is a hot candidate for using
this boot parameter for debugging purposes.

If ``foo`` module is not built-in, ``foo.dyndbg`` will still be processed at
boot time, without effect, but will be reprocessed when module is
loaded later. Bare ``dyndbg=`` is only processed at boot.


Debug Messages at Module Initialization Time
============================================

When ``modprobe foo`` is called, modprobe scans ``/proc/cmdline`` for
``foo.params``, strips ``foo.``, and passes them to the kernel along with
params given in modprobe args or ``/etc/modprobe.d/*.conf`` files,
in the following order:

1. parameters given via ``/etc/modprobe.d/*.conf``::

	options foo dyndbg=+pt
	options foo dyndbg # defaults to +p

2. ``foo.dyndbg`` as given in boot args, ``foo.`` is stripped and passed::

	foo.dyndbg=" func bar +p; func buz +mp"

3. args to modprobe::

	modprobe foo dyndbg==pmf # override previous settings

These ``dyndbg`` queries are applied in order, with last having final say.
This allows boot args to override or modify those from ``/etc/modprobe.d``
(sensible, since 1 is system wide, 2 is kernel or boot specific), and
modprobe args to override both.

In the ``foo.dyndbg="QUERY"`` form, the query must exclude ``module foo``.
``foo`` is extracted from the param-name, and applied to each query in
``QUERY``, and only 1 match-spec of each type is allowed.

The ``dyndbg`` option is a "fake" module parameter, which means:

- modules do not need to define it explicitly
- every module gets it tacitly, whether they use pr_debug or not
- it doesn't appear in ``/sys/module/$module/parameters/``
  To see it, grep the control file, or inspect ``/proc/cmdline.``

For ``CONFIG_DYNAMIC_DEBUG`` kernels, any settings given at boot-time (or
enabled by ``-DDEBUG`` flag during compilation) can be disabled later via
the debugfs interface if the debug messages are no longer needed::

   echo "module module_name -p" > /proc/dynamic_debug/control

Examples
========

::

  // enable the message at line 1603 of file svcsock.c
  :#> ddcmd 'file svcsock.c line 1603 +p'

  // enable all the messages in file svcsock.c
  :#> ddcmd 'file svcsock.c +p'

  // enable all the messages in the NFS server module
  :#> ddcmd 'module nfsd +p'

  // enable all 12 messages in the function svc_process()
  :#> ddcmd 'func svc_process +p'

  // disable all 12 messages in the function svc_process()
  :#> ddcmd 'func svc_process -p'

  // enable messages for NFS calls READ, READLINK, READDIR and READDIR+.
  :#> ddcmd 'format "nfsd: READ" +p'

  // enable messages in files of which the paths include string "usb"
  :#> ddcmd 'file *usb* +p'

  // enable all messages
  :#> ddcmd '+p'

  // add module, function to all enabled messages
  :#> ddcmd '+mf'

  // boot-args example, with newlines and comments for readability
  Kernel command line: ...
    // see what's going on in dyndbg=value processing
    dynamic_debug.verbose=3
    // enable pr_debugs in the btrfs module (can be builtin or loadable)
    btrfs.dyndbg="+p"
    // enable pr_debugs in all files under init/
    // and the function parse_one, #cmt is stripped
    dyndbg="file init/* +p #cmt ; func parse_one +p"
    // enable pr_debugs in 2 functions in a module loaded later
    pc87360.dyndbg="func pc87360_init_device +p; func pc87360_find +p"
    // open private tracing/instances/foo,bar
    dyndbg=open,foo%open,bar

Kernel Configuration
====================

Dynamic Debug is enabled via kernel config items::

  CONFIG_DYNAMIC_DEBUG=y	# build catalog, enables CORE
  CONFIG_DYNAMIC_DEBUG_CORE=y	# enable mechanics only, skip catalog

If you do not want to enable dynamic debug globally (i.e. in some embedded
system), you may set ``CONFIG_DYNAMIC_DEBUG_CORE`` as basic support of dynamic
debug and add ``ccflags := -DDYNAMIC_DEBUG_MODULE`` into the Makefile of any
modules which you'd like to dynamically debug later.


Kernel *prdbg* API
==================

The following functions are cataloged and controllable when dynamic
debug is enabled::

  pr_debug()
  dev_dbg()
  print_hex_dump_debug()
  print_hex_dump_bytes()

Otherwise, they are off by default; ``ccflags += -DDEBUG`` or
``#define DEBUG`` in a source file will enable them appropriately.

If ``CONFIG_DYNAMIC_DEBUG`` is not set, ``print_hex_dump_debug()`` is
just a shortcut for ``print_hex_dump(KERN_DEBUG)``.

Miscellaneous Notes
===================

For ``print_hex_dump_debug()``/``print_hex_dump_bytes()``, format string is
its ``prefix_str`` argument, if it is constant string; or ``hexdump``
in case ``prefix_str`` is built dynamically.

Dynamic Debug classmaps
=======================

The "class" keyword selects prdbgs based on author supplied,
domain-oriented names.  This complements the nested-scope keywords:
module, file, function, line.

The main difference from the others: class'd prdbgs must be named to
be changed.  This protects them from generic overwrite:

  # IOW this cannot undo any DRM.debug settings
  :#> ddcmd -p

So each class must be enabled individually (no wildcards):

  :#> ddcmd class DRM_UT_CORE +p
  :#> ddcmd class DRM_UT_KMS +p
  # or more selectively
  :#> ddcmd class DRM_UT_CORE module drm +p

Or the legacy/normal (more convenient) way:

  :#> echo 0x1ff > /sys/module/drm/parameters/debug

Dynamic Debug Classmap API
==========================

DRM.debug is built upon:
  ~23 macros, all passing a DRM_UT_* constant as arg-1.
  ~5000 calls to them, across drivers/gpu/drm/*
  bits in /sys/module/drm/parameters/debug control all DRM_UT_* together

The const short ints are good for optimizing compilers; a classmaps
design goal was to keep that.  So basically .classid === category.

And since prdbgs are cataloged with just a DRM_UT_* to identify them,
the "class" keyword maps known classnames to those reserved IDs, and
by explicitly requiring "class FOO" in queries, we protect FOO class'd
debugs from overwrite by generic queries.

Its expected that other classmap users will also provide debug-macros
using an enum-defined categorization scheme like DRM's, and dyndbg can
be adapted under them similarly.

DYNDBG_CLASSMAP_DEFINE(var,type,_base,classnames) - this maps
classnames onto class-ids starting at _base, it also maps the
names onto CLASSMAP_PARAM bits 0..N.

DYNDBG_CLASSMAP_USE(var) - modules call this to refer to the var
_DEFINEd elsewhere (and exported).

Classmaps are opt-in: modules invoke _DEFINE or _USE to authorize
dyndbg to update those classes.  "class FOO" queries are validated
against the classes, this finds the classid to alter; classes are not
directly selectable by their classid.

There are 2 types of classmaps:

 DD_CLASS_TYPE_DISJOINT_BITS: classes are independent, like DRM.debug
 DD_CLASS_TYPE_LEVEL_NUM: classes are relative, ordered (V3 > V2)

DYNDBG_CLASSMAP_PARAM - modelled after module_param_cb, it refers to a
DEFINEd classmap, and associates it to the param's data-store.  This
state is then applied to DEFINEr and USEr modules when they're modprobed.

The PARAM interface also enforces the DD_CLASS_TYPE_LEVEL_NUM relation
amongst the contained classnames; all classes are independent in the
control parser itself; there is no implied meaning in names like "V4".

Modules or module-groups (drm & drivers) can define multiple
classmaps, as long as they share the limited 0..62 per-module-group
_class_id range, without overlap.

``#define DEBUG`` will enable all pr_debugs in scope, including any
class'd ones.  This won't be reflected in the PARAM readback value,
but the class'd pr_debug callsites can be forced off by toggling the
classmap-kparam all-on then all-off.

For ``print_hex_dump_debug()`` and ``print_hex_dump_bytes()``, only
the ``p`` and ``T`` flags have meaning, other flags are ignored.

pr_fmt displays after the dynamic-prefix.
