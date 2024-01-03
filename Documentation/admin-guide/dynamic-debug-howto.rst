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
   - trace_name <destination>

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
commands can be written together, separated by ``%``, ``;`` or ``\n``::

  :#> ddcmd func foo +p % func bar +p
  :#> ddcmd func foo +p \; func bar +p
  :#> ddcmd <<EOC
  func pnpacpi_get_resources +p
  func pnp_assign_mem +p
  EOC
  :#> cat query-batch-file > /proc/dynamic_debug/control

You can also use wildcards in each query term. The match rule supports
``*`` (matches zero or more characters) and ``?`` (matches exactly one
character). For example, you can match all usb drivers::

  :#> ddcmd file "drivers/usb/*" +p	# "" to suppress shell expansion

Syntactically, a command is pairs of keyword values, followed by a
flags change or setting::

  command ::= match-spec* flags-spec

The match-spec's select *prdbgs* from the catalog, upon which to apply
the flags-spec, all constraints are ANDed together.  An absent keyword
is the same as keyword "*".

A match specification is a keyword, which selects the attribute of
the callsite to be compared, and a value to compare against.  Possible
keywords are:::

  match-spec ::= 'func' string |
		 'file' string |
		 'module' string |
		 'format' string |
		 'class' string |
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

func
    The given string is compared against the function name
    of each callsite.  Example::

	func svc_tcp_accept
	func *recv*		# in rfcomm, bluetooth, ping, tcp

file
    The given string is compared against either the src-root relative
    pathname, or the basename of the source file of each callsite.
    Examples::

	file svcsock.c
	file kernel/freezer.c	# ie column 1 of control file
	file drivers/usb/*	# all callsites under it
	file inode.c:start_*	# parse :tail as a func (above)
	file inode.c:1-100	# parse :tail as a line-range (above)

module
    The given string is compared against the module name
    of each callsite.  The module name is the string as
    seen in ``lsmod``, i.e. without the directory or the ``.ko``
    suffix and with ``-`` changed to ``_``.

    Examples::

	module,sunrpc	# with ',' as token separator
	module nfsd
	module drm*	# both drm, drm_kms_helper

format
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

class <c_name>
    The c_name is validated against each module, which may have
    declared a list of class_names it knows.  If the c_name is known
    by a module, site matching and site flags adjustment proceeds.
    Examples::

	class DRM_UT_KMS	# a DRM.debug category
	class JUNK		# silent non-match
	// class TLD_*		# NOTICE: no wildcard in class names

line
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

trace_name/label <t_name>
    This matches the t_name against each callsite's current trace-dest
    (default is 0).  It lets a user select and enable a previously
    labelled set of callsites.

The flags-spec is a change operation followed by one or more flag
characters.  The change operation is one of the characters::

  -    disable these flags
  +    enable these flags
  =    set these flags

The debug-output flags are::

  p    print to syslog
  T    write to tracefs (see below)
  _    no flags. useful with =_, legal with +_, -_

  prefix flags compose each site's dynamic-prefix, in order:
  t    thread ID, or <intr>
  m    module name
  f    the function name
  s    the source file name
  l    line number

The p,T flags can be followed by a named trace-instance, which
defaults to "0" for the global trace (/sys/kernel/tracing/trace)::

  # because match-spec can be empty, these are legal commands.
  =T       # enable tracing/trace (implied)
  =T:0     # enable global tracing/trace (explicit)
  =pT      # enable syslog and global trace (implied)
  =T:0.mf  # enable global trace-buf, with "module:function:" prefix
  =T:foo   # enable tracing/instances/foo, after open-ing it
  =_       # clear all flags (set them to off)
  =:foo    # clear all flags, set all trace-dests to foo (if it is open'd)
  =_:foo   # set all trace-dests to foo (if it is open'd)
  =:0      # reset all trace-dests to global trace-buf

Debug output to Syslog and/or Tracefs
=====================================

Dynamic Debug can direct pr_debugs to both syslog and tracefs, using
the +p, +T flags respectively.  This allows users to migrate away from
syslog if they see utility in doing so.

By default, =T writes to the global /sys/kernel/tracing/trace buffer,
and =T:0 does so explicitly, using the buffer's dyndbg-name :0.

Alternatively =T:named_trace.flags writes those pr_debugs to a named
trace (in tracing/instances/$named_trace) instead of the global
trace-buf (tracing/trace), iff it was previously opened.

Example 1:

   echo <<CMD_BLK > /proc/dynamic_debug/control
    class DRM_UT_KMS +T:0		# user wants KMS in global
    open drm_bulk			# misc bucket
     class DRM_UT_CORE +T:drm_bulk	# assemble misc contents
     class DRM_UT_DRIVER +T:drm_bulk
    open drm_screens			# screen changes
     class DRM_UT_LEASE +T:drm_screens	# assemble the contents
     class DRM_UT_DP    +T:drm_screens
     class DRM_UT_DRMRES +T:drm_screens
     class DRM_UT_STATE  +T:drm_screens
    open trash
     class junk +T:trash
    open drm_vblank			# isolate hi-rate
     class DRM_UT_VBL   +T
    open 0				# restore global default
   CMD_BLK

With this you can segregate groupings of messages to private buffers,
for any number of reasons:

 - create a flight-recorder buffer
 - isolate hi-rate traffic
 - simplify buffer management and overwrite
 - assemble "related" sets of prdbgs by +T:name.
 - select & enable them later, with keyword: trace_name/label
 - just label some traffic as trash/uninteresting
 - trace-cmd can merge them for viewing
 - 63 private buffers are supported + global

Note: This CMD_BLK example uses +T (not =T) to set callsite state;
this preserves existing p-flags, allowing their independent use for
syslog stuff.  If you are doing this, note also that the prefix flags
are shared; they affect messages to both destinations.

Example 2:

If the CMD_BLK above used -T:named instead of +T:named, it would
disable those trace-messages, but still set the trace-destinations on
the selected pr_debugs.

This allows a user to compose a "labelled" set of pr_debugs, to
capture "related" activity (as the user sees it).  Then later,
they can enable the composed/named/labelled groups at once::

  ddcmd trace_name vblank -T		# turn off hi-rate, save work
  ddcmd trace_name drm_screens +T	# track plug/unplug events ?

Example 3:

With the set-default-dest-on-open feature, the =T:tname syntax (as
used in example 1: CMD_BLK) is unnecessary, because each =T
immediately follows an "open $named", so the tname is implicit.

This plays nicely with the keep-presets feature used in example 2, by
only updating the site's name to the last open'd one if it is "0".

The last "open 0" cmd in the BLK resets the default trace-dest to the
global one, so that the next user is not surprised.

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

virtme-ng example
=================

  vng -v --user root -p 4 \
      -a dynamic_debug.verbose=3 \
      -a \*.dyndbg=class,DRM_UT_CORE,+fmltT:drm_core_dbg%class,DRM_UT_KMS,+fmlT:drm_kms_dbg

That last line adds boot-time presets to enable (+T:name) private
trace-instances, for 2 of DRM's debug classes/categories.  The ``\*.``
module wildcard insures that the setting/command is applied to all
modules as they're loaded, and those that USE the DRM_UT_* classes
will get the setting.

The example above requires that private tracebufs can be auto-opened,
otherwize the dyndbg=value would need additional terms or a 2nd
boot-option:

  vng -v --user root -p 4 \
      -a dynamic_debug.verbose=3 \
      -a dyndbg=open,drm_core_dbg%open,drm_kms_dbg
      -a \*.dyndbg=class,DRM_UT_CORE,+fmltT:drm_core_dbg%class,DRM_UT_KMS,+fmlT:drm_kms_dbg

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
  :#> ddcmd 'file *usb* +p' > /proc/dynamic_debug/control

  // enable all messages
  :#> ddcmd '+p' > /proc/dynamic_debug/control

  // add module, function to all enabled messages
  :#> ddcmd '+mf' > /proc/dynamic_debug/control

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

For ``print_hex_dump_debug()`` and ``print_hex_dump_bytes()``, only
the ``p`` flag has meaning, other flags are ignored.
