/* radare - LGPL - Copyright 2009-2020 - nibble, pancake */
#if 0
* Use RList
* Support callback for null command (why?)
* Show help of commands
  - long commands not yet tested at all
  - added interface to export command list into an autocompletable
    argc, argv for dietline
* r_cmd must provide a nesting char table indexing for commands
  - this is already partially done
  - this is pretty similar to r_db
  - every module can register their own commands
  - commands can be listed like in a tree
#endif

#define INTERACTIVE_MAX_REP 1024

#include <r_core.h>
#include <r_anal.h>
#include <r_cons.h>
#include <r_cmd.h>
#include <stdint.h>
#include <sys/types.h>
#include <ctype.h>
#include <stdarg.h>
#if __UNIX__
#include <sys/utsname.h>
#endif

#if USE_TREESITTER
#include <tree_sitter/api.h>
TSLanguage *tree_sitter_r2cmd ();
#endif

R_API void r_save_panels_layout(RCore *core, const char *_name);
R_API void r_load_panels_layout(RCore *core, const char *_name);

#define DEFINE_CMD_DESCRIPTOR(core, cmd_) \
	{ \
		RCmdDescriptor *d = R_NEW0 (RCmdDescriptor); \
		if (d) { \
			d->cmd = #cmd_; \
			d->help_msg = help_msg_##cmd_; \
			r_list_append ((core)->cmd_descriptors, d); \
		} \
	}

#define DEFINE_CMD_DESCRIPTOR_WITH_DETAIL(core, cmd_) \
	{ \
		RCmdDescriptor *d = R_NEW0 (RCmdDescriptor); \
		if (d) { \
			d->cmd = #cmd_; \
			d->help_msg = help_msg_##cmd_; \
			d->help_detail = help_detail_##cmd_; \
			r_list_append ((core)->cmd_descriptors, d); \
		} \
	}

#define DEFINE_CMD_DESCRIPTOR_WITH_DETAIL2(core, cmd_) \
	{ \
		RCmdDescriptor *d = R_NEW0 (RCmdDescriptor); \
		if (d) { \
			d->cmd = #cmd_; \
			d->help_msg = help_msg_##cmd_; \
			d->help_detail = help_detail_##cmd_; \
			d->help_detail2 = help_detail2_##cmd_; \
			r_list_append ((core)->cmd_descriptors, d); \
		} \
	}

#define DEFINE_CMD_DESCRIPTOR_SPECIAL(core, cmd_, named_cmd) \
	{ \
		RCmdDescriptor *d = R_NEW0 (RCmdDescriptor); \
		if (d) { \
			d->cmd = #cmd_; \
			d->help_msg = help_msg_##named_cmd; \
			r_list_append ((core)->cmd_descriptors, d); \
		} \
	}

static int r_core_cmd_subst_i(RCore *core, char *cmd, char* colon, bool *tmpseek);

static int bb_cmpaddr(const void *_a, const void *_b) {
	const RAnalBlock *a = _a, *b = _b;
	return a->addr > b->addr ? 1 : (a->addr < b->addr ? -1 : 0);
}

static void cmd_debug_reg(RCore *core, const char *str);
#include "cmd_quit.c"
#include "cmd_hash.c"
#include "cmd_debug.c"
#include "cmd_log.c"
#include "cmd_flag.c"
#include "cmd_zign.c"
#include "cmd_project.c"
#include "cmd_write.c"
#include "cmd_cmp.c"
#include "cmd_eval.c"
#include "cmd_anal.c"
#include "cmd_open.c"
#include "cmd_meta.c"
#include "cmd_type.c"
#include "cmd_egg.c"
#include "cmd_info.c"
#include "cmd_macro.c"
#include "cmd_magic.c"
#include "cmd_mount.c"
#include "cmd_seek.c"
#include "cmd_search.c" // defines incDigitBuffer... used by cmd_print
#include "cmd_print.c"
#include "cmd_help.c"
#include "cmd_colon.c"

static const char *help_msg_dollar[] = {
	"Usage:", "$alias[=cmd] [args...]", "Alias commands and strings (See ?$? for help on $variables)",
	"$", "", "list all defined aliases",
	"$*", "", "list all the aliases as r2 commands in base64",
	"$**", "", "same as above, but using plain text",
	"$", "foo:=123", "alias for 'f foo=123'",
	"$", "foo-=4", "alias for 'f foo-=4'",
	"$", "foo+=4", "alias for 'f foo+=4'",
	"$", "foo", "alias for 's foo' (note that command aliases can override flag resolution)",
	"$", "dis=base64:AAA==", "alias this base64 encoded text to be printed when $dis is called",
	"$", "dis=$hello world", "alias this text to be printed when $dis is called",
	"$", "dis=-", "open cfg.editor to set the new value for dis alias",
	"$", "dis=af;pdf", "create command - analyze to show function",
	"$", "test=#!pipe node /tmp/test.js", "create command - rlangpipe script",
	"$", "dis=", "undefine alias",
	"$", "dis", "execute the previously defined alias",
	"$", "dis?", "show commands aliased by $dis",
	"$", "dis?n", "show commands aliased by $dis, without a new line",
	NULL
};

static const char *help_msg_star[] = {
	"Usage:", "*<addr>[=[0x]value]", "Pointer read/write data/values",
	"*", "entry0=cc", "write trap in entrypoint",
	"*", "entry0+10=0x804800", "write value in delta address",
	"*", "entry0", "read byte at given address",
	"TODO: last command should honor asm.bits", "", "",
	NULL
};

static const char *help_msg_dot[] = {
	"Usage:", ".[r2cmd] | [file] | [!command] | [(macro)]", "# define macro or interpret r2, r_lang,\n"
	"    cparse, d, es6, exe, go, js, lsp, pl, py, rb, sh, vala or zig file",
	".", "", "repeat last command backward",
	".", "r2cmd", "interpret the output of the command as r2 commands",
	"..", " [file]", "run the output of the execution of a script as r2 commands",
	"...", "", "repeat last command forward (same as \\n)",
	".:", "8080", "listen for commands on given tcp port",
	".--", "", "terminate tcp server for remote commands",
	".", " foo.r2", "interpret script",
	".-", "", "open cfg.editor and interpret tmp file",
	".*", " file ...", "same as #!pipe open cfg.editor and interpret tmp file",
	".!", "rabin -ri $FILE", "interpret output of command",
	".", "(foo 1 2 3)", "run macro 'foo' with args 1, 2, 3",
	"./", " ELF", "interpret output of command /m ELF as r. commands",
	NULL
};

static const char *help_msg_equal[] = {
	"Usage:", " =[:!+-=ghH] [...]", " # connect with other instances of r2",
	"\nremote commands:", "", "",
	"=", "", "list all open connections",
	"=<", "[fd] cmd", "send output of local command to remote fd", // XXX may not be a special char
	"=", "[fd] cmd", "exec cmd at remote 'fd' (last open is default one)",
	"=!", " cmd", "run command via r_io_system",
	"=+", " [proto://]host:port", "connect to remote host:port (*rap://, raps://, tcp://, udp://, http://)",
	"=-", "[fd]", "remove all hosts or host 'fd'",
	"==", "[fd]", "open remote session with host 'fd', 'q' to quit",
	"=!=", "", "disable remote cmd mode",
	"!=!", "", "enable remote cmd mode",
	"\nservers:","","",
	".:", "9000", "start the tcp server (echo x|nc ::1 9090 or curl ::1:9090/cmd/x)",
	"=:", "port", "start the rap server (o rap://9999)",
	"=g", "[?]", "start the gdbserver",
	"=h", "[?]", "start the http webserver",
	"=H", "[?]", "start the http webserver (and launch the web browser)",
	"\nother:","","",
	"=&", ":port", "start rap server in background (same as '&_=h')",
	"=", ":host:port cmd", "run 'cmd' command on remote server",
	"\nexamples:","","",
	"=+", "tcp://localhost:9090/", "connect to: r2 -c.:9090 ./bin",
	// "=+", "udp://localhost:9090/", "connect to: r2 -c.:9090 ./bin",
	"=+", "rap://localhost:9090/", "connect to: r2 rap://:9090",
	"=+", "http://localhost:9090/cmd/", "connect to: r2 -c'=h 9090' bin",
	"o ", "rap://:9090/", "start the rap server on tcp port 9090",
	NULL
};

#if 0
static const char *help_msg_equalh[] = {
	"Usage:",  "=h[---*&] [port]", " # manage http connections",
	"=h", " port", "listen for http connections (r2 -qc=H /bin/ls)",
	"=h-", "", "stop background webserver",
	"=h--", "", "stop foreground webserver",
	"=h*", "", "restart current webserver",
	"=h&", " port", "start http server in background",
	NULL
};
#endif

static const char *help_msg_equalh[] = {
	"Usage:", " =[hH] [...]", " # http server",
	"http server:", "", "",
	"=h", " port", "listen for http connections (r2 -qc=H /bin/ls)",
	"=h-", "", "stop background webserver",
	"=h--", "", "stop foreground webserver",
	"=h*", "", "restart current webserver",
	"=h&", " port", "start http server in background",
	"=H", " port", "launch browser and listen for http",
	"=H&", " port", "launch browser and listen for http in background",
	NULL
};

static const char *help_msg_equalg[] = {
	"Usage:", " =[g] [...]", " # gdb server",
	"gdbserver:", "", "",
	"=g", " port file [args]", "listen on 'port' debugging 'file' using gdbserver",
	"=g!", " port file [args]", "same as above, but debug protocol messages (like gdbserver --remote-debug)",
	NULL
};

static const char *help_msg_b[] = {
	"Usage:",  "b[f] [arg]\n", "Get/Set block size",
	"b", " 33", "set block size to 33",
	"b", " eip+4", "numeric argument can be an expression",
	"b", "", "display current block size",
	"b", "+3", "increase blocksize by 3",
	"b", "-16", "decrease blocksize by 16",
	"b*", "", "display current block size in r2 command",
	"bf", " foo", "set block size to flag size",
	"bj", "", "display block size information in JSON",
	"bm", " 1M", "set max block size",
	NULL
};

static const char *help_msg_k[] = {
	"Usage:", "k[s] [key[=value]]", "Sdb Query",
	"k", " anal/**", "list namespaces under anal",
	"k", " anal/meta/*", "list kv from anal > meta namespaces",
	"k", " anal/meta/meta.0x80404", "get value for meta.0x80404 key",
	"k", " foo", "show value",
	"k", " foo=bar", "set value",
	"k", "", "list keys",
	"kd", " [file.sdb] [ns]", "dump namespace to disk",
	"kj", "", "List all namespaces and sdb databases in JSON format",
	"ko", " [file.sdb] [ns]", "open file into namespace",
	"ks", " [ns]", "enter the sdb query shell",
	//"kl", " ha.sdb", "load keyvalue from ha.sdb",
	//"ks", " ha.sdb", "save keyvalue to ha.sdb",
	NULL,
};

static const char *help_msg_r[] = {
	"Usage:", "r[+-][ size]", "Resize file",
	"r", "", "display file size",
	"r", " size", "expand or truncate file to given size",
	"r-", "num", "remove num bytes, move following data down",
	"r+", "num", "insert num bytes, move following data up",
	"rb", "oldbase @ newbase", "rebase all flags, bin.info, breakpoints and analysis",
	"rm" ," [file]", "remove file",
	"rh" ,"", "show size in human format",
	"r2" ," [file]", "launch r2 (same for rax2, rasm2, ...)",
	"reset" ,"", "reset console settings (clear --hard)",
	NULL
};

static const char *help_msg_u[] = {
	"Usage:", "u", "uname or undo write/seek",
	"u", "", "show system uname",
	"uw", "", "alias for wc (requires: e io.cache=true)",
	"us", "", "alias for s- (seek history)",
	"uc", "", "undo core commands (uc?, ucl, uc*, ..)",
	"uniq", "", "filter rows to avoid duplicates",
	"uname", "", "uname - show system information",
	NULL
};

static const char *help_msg_y[] = {
	"Usage:", "y[ptxy] [len] [[@]addr]", " # See wd? for memcpy, same as 'yf'.",
	"y!", "", "open cfg.editor to edit the clipboard",
	"y", " 16 0x200", "copy 16 bytes into clipboard from 0x200",
	"y", " 16 @ 0x200", "copy 16 bytes into clipboard from 0x200",
	"y", " 16", "copy 16 bytes into clipboard",
	"y", "", "show yank buffer information (srcoff len bytes)",
	"y*", "", "print in r2 commands what's been yanked",
	"yf", " 64 0x200", "copy file 64 bytes from 0x200 from file",
	"yfa", " file copy", "copy all bytes from file (opens w/ io)",
	"yfx", " 10203040", "yank from hexpairs (same as ywx)",
	"yj", "", "print in JSON commands what's been yanked",
	"yp", "", "print contents of clipboard",
	"yq", "", "print contents of clipboard in hexpairs",
	"ys", "", "print contents of clipboard as string",
	"yt", " 64 0x200", "copy 64 bytes from current seek to 0x200",
	"ytf", " file", "dump the clipboard to given file",
	"yw", " hello world", "yank from string",
	"ywx", " 10203040", "yank from hexpairs (same as yfx)",
	"yx", "", "print contents of clipboard in hexadecimal",
	"yy", " 0x3344", "paste clipboard",
	"yz", " [len]", "copy nul-terminated string (up to blocksize) into clipboard",
	NULL
};

static const char *help_msg_triple_exclamation[] = {
	"Usage:", "!!![-*][cmd] [arg|$type...]", " # user-defined autocompletion for commands",
	"!!!", "", "list all autocompletions",
	"!!!?", "", "show this help",
	"!!!", "-*", "remove all user-defined autocompletions",
	"!!!", "-\\*", "remove autocompletions matching this glob expression",
	"!!!", "-foo", "remove autocompletion named 'foo'",
	"!!!", "foo", "add 'foo' for autocompletion",
	"!!!", "bar $flag", "add 'bar' for autocompletion with $flag as argument",
	NULL
};

static const char *help_msg_vertical_bar[] = {
	"Usage:", "[cmd] | [program|H|T|.|]", "",
	"", "[cmd] |?", "show this help",
	"", "[cmd] |", "disable scr.html and scr.color",
	"", "[cmd] |H", "enable scr.html, respect scr.color",
	"", "[cmd] |T", "use scr.tts to speak out the stdout",
	"", "[cmd] | [program]", "pipe output of command to program",
	"", "[cmd] |.", "alias for .[cmd]",
	NULL
};

R_API void r_core_cmd_help(const RCore *core, const char *help[]) {
	r_cons_cmd_help (help, core->print->flags & R_PRINT_FLAGS_COLOR);
}

struct duplicate_flag_t {
	RList *ret;
	const char *word;
};

static bool duplicate_flag(RFlagItem *flag, void *u) {
	struct duplicate_flag_t *user = (struct duplicate_flag_t *)u;
	/* filter per flag spaces */
	if (r_str_glob (flag->name, user->word)) {
		RFlagItem *cloned_item = r_flag_item_clone (flag);
		if (!cloned_item) {
			return false;
		}
		r_list_append (user->ret, cloned_item);
	}
	return true;
}

static void recursive_help_go(RCore *core, int detail, RCmdDescriptor *desc) {
	int i;
	if (desc->help_msg) {
		r_core_cmd_help (core, desc->help_msg);
	}
	if (detail >= 1) {
		if (desc->help_detail) {
			r_core_cmd_help (core, desc->help_detail);
		}
		if (detail >= 2 && desc->help_detail2) {
			r_core_cmd_help (core, desc->help_detail2);
		}
	}
	for (i = 32; i < R_ARRAY_SIZE (desc->sub); i++) {
		if (desc->sub[i]) {
			recursive_help_go (core, detail, desc->sub[i]);
		}
	}
}

static void recursive_help(RCore *core, int detail, const char *cmd_prefix) {
	const ut8 *p;
	RCmdDescriptor *desc = &core->root_cmd_descriptor;
	for (p = (const ut8 *)cmd_prefix; *p && *p < R_ARRAY_SIZE (desc->sub); p++) {
		if (!(desc = desc->sub[*p])) {
			return;
		}
	}
	recursive_help_go (core, detail, desc);
}

static bool lastcmd_repeat(RCore *core, int next) {
	int res = -1;
	// Fix for backtickbug px`~`
	if (!core->lastcmd || core->cons->context->cmd_depth < 1) {
		return false;
	}
	switch (*core->lastcmd) {
	case '.':
		if (core->lastcmd[1] == '(') { // macro call
			res = r_core_cmd0 (core, core->lastcmd);
		}
		break;
	case 'd': // debug
		res = r_core_cmd0 (core, core->lastcmd);
		switch (core->lastcmd[1]) {
		case 's':
		case 'c':
			r_core_cmd0 (core, "sr PC;pd 1");
		}
		break;
	case 'p': // print
	case 'x':
	case '$':
		if (!strncmp (core->lastcmd, "pd", 2)) {
			if (core->lastcmd[2]== ' ') {
				r_core_cmdf (core, "so %s", core->lastcmd + 3);
			} else {
				r_core_cmd0 (core, "so `pi~?`");
			}
		} else {
			if (next) {
				r_core_seek (core, core->offset + core->blocksize, 1);
			} else {
				if (core->blocksize > core->offset) {
					r_core_seek (core, 0, 1);
				} else {
					r_core_seek (core, core->offset - core->blocksize, 1);
				}
			}
		}
		res = r_core_cmd0 (core, core->lastcmd);
		break;
	}
	return res != -1;
}

static int r_core_cmd_nullcallback(void *data) {
	RCore *core = (RCore*) data;
	if (core->cons->context->breaked) {
		core->cons->context->breaked = false;
		return 0;
	}
	if (!core->cmdrepeat) {
		return 0;
	}
	lastcmd_repeat (core, true);
	return 1;
}

static int cmd_uname(void *data, const char *input) { // "uniq"
	RSysInfo *si = r_sys_info();
	if (si) {
		r_cons_printf ("%s", si->sysname);
		if (strstr (input, "-r")) {
			r_cons_printf (" %s", si->release);
		}
		r_cons_newline ();
		r_sys_info_free (si);
	}
	return 0;
}

static int cmd_uniq(void *data, const char *input) { // "uniq"
	RCore *core = (RCore *)data;
	const char *arg = strchr (input, ' ');
	if (arg) {
		arg = r_str_trim_head_ro (arg + 1);
	}
	switch (*input) {
	case '?': // "uniq?"
		eprintf ("Usage: uniq # uniq to list unique strings in file\n");
		break;
	default: // "uniq"
		if (!arg) {
			arg = "";
		}
		if (r_fs_check (core->fs, arg)) {
			r_core_cmdf (core, "md %s", arg);
		} else {
			char *res = r_syscmd_uniq (arg);
			if (res) {
				r_cons_print (res);
				free (res);
			}
		}
		break;
	}
	return 0;
}

static int cmd_head (void *data, const char *_input) { // "head"
	RCore *core = (RCore *)data;
	int lines = 5;
	char *input = strdup (_input);
	char *arg = strchr (input, ' ');
	char *tmp, *count;
	if (arg) {
		arg = (char *)r_str_trim_head_ro (arg + 1); 	// contains "count filename"
		count = strchr (arg, ' ');
		if (count) {
			*count = 0;	// split the count and file name
			tmp = (char *)r_str_trim_head_ro (count + 1);
			lines = atoi (arg);
			arg = tmp;
		}
	}
	switch (*input) {
	case '?': // "head?"
		eprintf ("Usage: head [file] # to list first n lines in file\n");
		break;
	default: // "head"
		if (!arg) {
			arg = "";
		}
		if (r_fs_check (core->fs, arg)) {
			r_core_cmdf (core, "md %s", arg);
		} else {
			char *res = r_syscmd_head (arg, lines);
			if (res) {
				r_cons_print (res);
				free (res);
			}
		}
		break;
	}
	free (input);
	return 0;
}

static int cmd_undo(void *data, const char *input) {
	RCore *core = (RCore *)data;
	switch (input[0]) {
	case '?': // "u?"
		r_core_cmd_help (data, help_msg_u);
		return 1;
	case 'c': // "uc"
		switch (input[1]) {
		case ' ': {
			char *cmd = strdup (input + 2);
			char *rcmd = strchr (cmd, ',');
			if (rcmd) {
				*rcmd++ = 0;
				RCoreUndo *undo = r_core_undo_new (core->offset, cmd, rcmd);
				r_core_undo_push (core, undo);
			} else {
				eprintf ("Usage: uc [cmd] [revert-cmd]");
			}
			free (cmd);
			}
			break;
		case '?':
			eprintf ("Usage: uc [cmd],[revert-cmd]\n");
			eprintf (" uc. - list all reverts in current\n");
			eprintf (" uc* - list all core undos\n");
			eprintf (" uc  - list all core undos\n");
			eprintf (" uc- - undo last action\n");
			break;
		case '.': {
			RCoreUndoCondition cond = {
				.addr = core->offset,
				.minstamp = 0,
				.glob = NULL
			};
			r_core_undo_print (core, 1, &cond);
			} break;
		case '*':
			r_core_undo_print (core, 1, NULL);
			break;
		case '-': // "uc-"
			r_core_undo_pop (core);
			break;
		default:
			r_core_undo_print (core, 0, NULL);
			break;
		}
		return 1;
	case 's': // "us"
		r_core_cmdf (data, "s-%s", input + 1);
		return 1;
	case 'w': // "uw"
		r_core_cmdf (data, "wc%s", input + 1);
		return 1;
	case 'n': // "un"
		if (input[1] == 'a') { // "uname"
			(void)cmd_uname (core, input);
		} else if (input[1] == 'i' && input[2] == 'q') {
			(void)cmd_uniq (core, input);
		}
		return 1;
	}
#if __UNIX__
	struct utsname un;
	uname (&un);
	r_cons_printf ("%s %s %s %s\n", un.sysname,
		un.nodename, un.release, un.machine);
#elif __WINDOWS__
	r_cons_printf ("windows\n");
#else
	r_cons_printf ("unknown\n");
#endif
	return 0;
}

static int cmd_alias(void *data, const char *input) {
	RCore *core = (RCore *)data;
	if (*input == '?') {
		r_core_cmd_help (core, help_msg_dollar);
		return 0;
	}
	int i = strlen (input);
	char *buf = malloc (i + 2);
	if (!buf) {
		return 0;
	}
	*buf = '$'; // prefix aliases with a dollar
	memcpy (buf + 1, input, i + 1);
	char *q = strchr (buf, ' ');
	char *def = strchr (buf, '=');
	char *desc = strchr (buf, '?');
	char *nonl = strchr (buf, 'n');

	int defmode = 0;
	if (def && def > buf) {
		char *prev = def - 1;
		switch (*prev) {
		case ':':
			defmode = *prev;
			*prev = 0;
			break;
		case '+':
			defmode = *prev;
			*prev = 0;
			break;
		case '-':
			defmode = *prev;
			*prev = 0;
			break;
		}
	}

	/* create alias */
	if ((def && q && (def < q)) || (def && !q)) {
		*def++ = 0;
		size_t len = strlen (def);
		if (defmode) {
			ut64 at = r_num_math (core->num, def);
			switch (defmode) {
			case ':':
				r_flag_set (core->flags, buf + 1, at, 1);
				return 1;
			case '+':
				at = r_num_get (core->num, buf + 1) + at;
				r_flag_set (core->flags, buf + 1, at, 1);
				return 1;
			case '-':
				at = r_num_get (core->num, buf + 1) - at;
				r_flag_set (core->flags, buf + 1, at, 1);
				return 1;
			}
		}
		/* Remove quotes */
		if (len > 0 && (def[0] == '\'') && (def[len - 1] == '\'')) {
			def[len - 1] = 0x00;
			def++;
		}
		if (!q || (q && q > def)) {
			if (*def) {
				if (!strcmp (def, "-")) {
					char *v = r_cmd_alias_get (core->rcmd, buf, 0);
					char *n = r_cons_editor (NULL, v);
					if (n) {
						r_cmd_alias_set (core->rcmd, buf, n, 0);
						free (n);
					}
				} else {
					r_cmd_alias_set (core->rcmd, buf, def, 0);
				}
			} else {
				r_cmd_alias_del (core->rcmd, buf);
			}
		}
	/* Show command for alias */
	} else if (desc && !q) {
		*desc = 0;
		char *v = r_cmd_alias_get (core->rcmd, buf, 0);
		if (v) {
			if (nonl == desc + 1) {
				r_cons_print (v);
			} else {
				r_cons_println (v);
			}
			free (buf);
			return 1;
		} else {
			eprintf ("unknown key '%s'\n", buf);
		}
	} else if (buf[1] == '*') {
		/* Show aliases */
		int i, count = 0;
		char **keys = r_cmd_alias_keys (core->rcmd, &count);
		for (i = 0; i < count; i++) {
			char *v = r_cmd_alias_get (core->rcmd, keys[i], 0);
			char *q = r_base64_encode_dyn (v, -1);
			if (buf[2] == '*') {
				r_cons_printf ("%s=%s\n", keys[i], v);
			} else {
				r_cons_printf ("%s=base64:%s\n", keys[i], q);
			}
			free (q);
		}
	} else if (!buf[1]) {
		int i, count = 0;
		char **keys = r_cmd_alias_keys (core->rcmd, &count);
		for (i = 0; i < count; i++) {
			r_cons_println (keys[i]);
		}
	} else {
		/* Execute alias */
		if (q) {
			*q = 0;
		}
		char *v = r_cmd_alias_get (core->rcmd, buf, 0);
		if (v) {
			if (*v == '$') {
				r_cons_strcat (v + 1);
				r_cons_newline ();
			} else if (q) {
				char *out = r_str_newf ("%s %s", v, q + 1);
				r_core_cmd0 (core, out);
				free (out);
			} else {
				r_core_cmd0 (core, v);
			}
		} else {
			ut64 at = r_num_get (core->num, buf + 1);
			if (at != UT64_MAX) {
				r_core_seek (core, at, 1);
			} else {
				eprintf ("Unknown alias '%s'\n", buf + 1);
			}
		}
	}
	free (buf);
	return 0;
}

static int getArg(char ch, int def) {
	switch (ch) {
	case '&':
	case '-':
		return ch;
	}
	return def;
}

// wtf dupe for local vs remote?
static void aliascmd(RCore *core, const char *str) {
	switch (str[0]) {
	case '\0': // "=$"
		r_core_cmd0 (core, "$");
		break;
	case '-': // "=$-"
		if (str[1]) {
			r_cmd_alias_del (core->rcmd, str + 2);
		} else {
			r_cmd_alias_del (core->rcmd, NULL);
		//	r_cmd_alias_reset (core->rcmd);
		}
		break;
	case '?': // "=$?"
		eprintf ("Usage: =$[-][remotecmd]  # remote command alias\n");
		eprintf (" =$dr   # makes 'dr' alias for =!dr\n");
		eprintf (" =$-dr  # unset 'dr' alias\n");
		break;
	default:
		r_cmd_alias_set (core->rcmd, str, "", 1);
		break;
	}
}

static int cmd_rap(void *data, const char *input) {
	RCore *core = (RCore *)data;
	switch (*input) {
	case '\0': // "="
		r_core_rtr_list (core);
		break;
	case 'j': // "=j"
		eprintf ("TODO: list connections in json\n");
		break;
	case '!': // "=!"
		if (input[1] == '=') {
			// swap core->cmdremote = core->cmdremote? 0: 1;
			core->cmdremote = input[2]? 1: 0;
			r_cons_println (r_str_bool (core->cmdremote));
		} else {
			char *res = r_io_system (core->io, input + 1);
			if (res) {
				r_cons_printf ("%s\n", res);
				free (res);
			}
		}
		break;
	case '$': // "=$"
		// XXX deprecate?
		aliascmd (core, input + 1);
		break;
	case '+': // "=+"
		r_core_rtr_add (core, input + 1);
		break;
	case '-': // "=-"
		r_core_rtr_remove (core, input + 1);
		break;
	//case ':': r_core_rtr_cmds (core, input + 1); break;
	case '<': // "=<"
		r_core_rtr_pushout (core, input + 1);
		break;
	case '=': // "=="
		r_core_rtr_session (core, input + 1);
		break;
	case 'g': // "=g"
		if (input[1] == '?') {
			r_core_cmd_help (core, help_msg_equalg);
		} else {
			r_core_rtr_gdb (core, getArg (input[1], 'g'), input + 1);
		}
		break;
	case 'h': // "=h"
		if (input[1] == '?') {
			r_core_cmd_help (core, help_msg_equalh);
		} else {
			r_core_rtr_http (core, getArg (input[1], 'h'), 'h', input + 1);
		}
		break;
	case 'H': // "=H"
		if (input[1] == '?') {
			r_core_cmd_help (core, help_msg_equalh);
		} else {
			const char *arg = r_str_trim_head_ro (input + 1);
			r_core_rtr_http (core, getArg (input[1], 'H'), 'H', arg);
		}
		break;
	case '?': // "=?"
		r_core_cmd_help (core, help_msg_equal);
		break;
	default:
		r_core_rtr_cmd (core, input);
		break;
	}
	return 0;
}

static int cmd_rap_run(void *data, const char *input) {
	RCore *core = (RCore *)data;
	char *res = r_io_system (core->io, input);
	if (res) {
		int ret = atoi (res);
		free (res);
		return ret;
	}
	return false;
}

static int cmd_yank(void *data, const char *input) {
	ut64 n;
	RCore *core = (RCore *)data;
	switch (input[0]) {
	case ' ': // "y "
		r_core_yank (core, core->offset, r_num_math (core->num, input + 1));
		break;
	case 'l': // "yl"
		core->num->value = r_buf_size (core->yank_buf);
		break;
	case 'y': // "yy"
		while (input[1] == ' ') {
			input++;
		}
		n = input[1]? r_num_math (core->num, input + 1): core->offset;
		r_core_yank_paste (core, n, 0);
		break;
	case 'x': // "yx"
		r_core_yank_hexdump (core, r_num_math (core->num, input + 1));
		break;
	case 'z': // "yz"
		r_core_yank_string (core, core->offset, r_num_math (core->num, input + 1));
		break;
	case 'w': // "yw" ... we have yf which makes more sense than 'w'
		switch (input[1]) {
		case ' ':
			r_core_yank_set (core, 0, (const ut8*)input + 2, strlen (input + 2));
			break;
		case 'x':
			if (input[2] == ' ') {
				char *out = strdup (input + 3);
				int len = r_hex_str2bin (input + 3, (ut8*)out);
				if (len > 0) {
					r_core_yank_set (core, core->offset, (const ut8*)out, len);
				} else {
					eprintf ("Invalid length\n");
				}
				free (out);
			} else {
				eprintf ("Usage: ywx [hexpairs]\n");
			}
			// r_core_yank_write_hex (core, input + 2);
			break;
		default:
			eprintf ("Usage: ywx [hexpairs]\n");
			break;
		}
		break;
	case 'p': // "yp"
		r_core_yank_cat (core, r_num_math (core->num, input + 1));
		break;
	case 's': // "ys"
		r_core_yank_cat_string (core, r_num_math (core->num, input + 1));
		break;
	case 't': // "wt"
		if (input[1] == 'f') { // "wtf"
			ut64 tmpsz;
			const char *file = r_str_trim_head_ro (input + 2);
			const ut8 *tmp = r_buf_data (core->yank_buf, &tmpsz);
			if (!r_file_dump (file, tmp, tmpsz, false)) {
				eprintf ("Cannot dump to '%s'\n", file);
			}
		} else if (input[1] == ' ') {
			r_core_yank_to (core, input + 1);
		} else {
			eprintf ("Usage: wt[f] [arg] ..\n");
		}
		break;
	case 'f': // "yf"
		switch (input[1]) {
		case ' ': // "yf"
			r_core_yank_file_ex (core, input + 1);
			break;
		case 'x': // "yfx"
			r_core_yank_hexpair (core, input + 2);
			break;
		case 'a': // "yfa"
			r_core_yank_file_all (core, input + 2);
			break;
		default:
			eprintf ("Usage: yf[xa] [arg]\n");
			eprintf ("yf [file]     - copy blocksize from file into the clipboard\n");
			eprintf ("yfa [path]    - yank the whole file\n");
			eprintf ("yfx [hexpair] - yank from hexpair string\n");
			break;
		}
		break;
	case '!': // "y!"
		{
			char *sig = r_core_cmd_str (core, "y*");
			if (!sig || !*sig) {
				free (sig);
				sig = strdup ("wx 10203040");
			}
			char *data = r_core_editor (core, NULL, sig);
			(void) strtok (data, ";\n");
			r_core_cmdf (core, "y%s", data);
			free (sig);
			free (data);
		}
		break;
	case '*': // "y*"
	case 'j': // "yj"
	case 'q': // "yq"
	case '\0': // "y"
		r_core_yank_dump (core, 0, input[0]);
		break;
	default:
		r_core_cmd_help (core, help_msg_y);
		break;
	}
	return true;
}

static int lang_run_file(RCore *core, RLang *lang, const char *file) {
	r_core_sysenv_begin (core, NULL);
	return r_lang_run_file (core->lang, file);
}

static char *langFromHashbang(RCore *core, const char *file) {
	int fd = r_sandbox_open (file, O_RDONLY, 0);
	if (fd != -1) {
		char firstLine[128] = {0};
		int len = r_sandbox_read (fd, (ut8*)firstLine, sizeof (firstLine) - 1);
		firstLine[len] = 0;
		if (!strncmp (firstLine, "#!/", 3)) {
			// I CAN HAS A HASHBANG
			char *nl = strchr (firstLine, '\n');
			if (nl) {
				*nl = 0;
			}
			nl = strchr (firstLine, ' ');
			if (nl) {
				*nl = 0;
			}
			nl = strdup (firstLine + 2);
			r_sandbox_close (fd);
			return nl;
		}
		r_sandbox_close (fd);
	}
	return NULL;
}

R_API bool r_core_run_script(RCore *core, const char *file) {
	bool ret = false;
	RListIter *iter;
	RLangPlugin *p;
	char *name;

	r_list_foreach (core->scriptstack, iter, name) {
		if (!strcmp (file, name)) {
			eprintf ("WARNING: ignored nested source: %s\n", file);
			return false;
		}
	}
	r_list_push (core->scriptstack, strdup (file));

	if (!strcmp (file, "-")) {
		char *out = r_core_editor (core, NULL, NULL);
		if (out) {
			ret = r_core_cmd_lines (core, out);
			free (out);
		}
	} else if (r_str_endswith (file, ".html")) {
		const bool httpSandbox = r_config_get_i (core->config, "http.sandbox");
		char *httpIndex = strdup (r_config_get (core->config, "http.index"));
		r_config_set_i (core->config, "http.sandbox", 0);
		char *absfile = r_file_abspath (file);
		r_config_set (core->config, "http.index", absfile);
		free (absfile);
		r_core_cmdf (core, "=H");
		r_config_set_i (core->config, "http.sandbox", httpSandbox);
		r_config_set (core->config, "http.index", httpIndex);
		free (httpIndex);
		ret = true;
	} else if (r_str_endswith (file, ".c")) {
		r_core_cmd_strf (core, "#!c %s", file);
		ret = true;
	} else if (r_file_is_c (file)) {
		const char *dir = r_config_get (core->config, "dir.types");
		char *out = r_parse_c_file (core->anal, file, dir, NULL);
		if (out) {
			r_cons_strcat (out);
			sdb_query_lines (core->anal->sdb_types, out);
			free (out);
		}
		ret = out != NULL;
	} else {
		p = r_lang_get_by_extension (core->lang, file);
		if (p) {
			r_lang_use (core->lang, p->name);
			ret = lang_run_file (core, core->lang, file);
		} else {
// XXX this is an ugly hack, we need to use execve here and specify args properly
#if __WINDOWS__
#define cmdstr(x) r_str_newf (x" %s", file);
#else
#define cmdstr(x) r_str_newf (x" '%s'", file);
#endif
			const char *p = r_str_lchr (file, '.');
			if (p) {
				const char *ext = p + 1;
				/* TODO: handle this inside r_lang_pipe with new APIs */
				if (!strcmp (ext, "js")) {
					char *cmd = cmdstr ("node");
					r_lang_use (core->lang, "pipe");
					lang_run_file (core, core->lang, cmd);
					free (cmd);
					ret = 1;
				} else if (!strcmp (ext, "exe")) {
#if __WINDOWS__
					char *cmd = r_str_newf ("%s", file);
#else
					char *cmd = cmdstr ("wine");
#endif
					r_lang_use (core->lang, "pipe");
					lang_run_file (core, core->lang, cmd);
					free (cmd);
					ret = 1;
				} else if (!strcmp (ext, "zig")) {
					char *cmd = cmdstr ("zig run");
					r_lang_use (core->lang, "pipe");
					lang_run_file (core, core->lang, cmd);
					free (cmd);
					ret = 1;
				} else if (!strcmp (ext, "d")) {
					char *cmd = cmdstr ("dmd -run");
					r_lang_use (core->lang, "pipe");
					lang_run_file (core, core->lang, cmd);
					free (cmd);
					ret = 1;
				} else if (!strcmp (ext, "lsp")) {
					char *cmd = cmdstr ("newlisp -n");
					r_lang_use (core->lang, "pipe");
					lang_run_file (core, core->lang, cmd);
					free (cmd);
					ret = 1;
				} else if (!strcmp (ext, "go")) {
					char *cmd = cmdstr ("go run");
					r_lang_use (core->lang, "pipe");
					lang_run_file (core, core->lang, cmd);
					free (cmd);
					ret = 1;
				} else if (!strcmp (ext, "es6")) {
					char *cmd = cmdstr ("babel-node");
					r_lang_use (core->lang, "pipe");
					lang_run_file (core, core->lang, cmd);
					free (cmd);
					ret = 1;
				} else if (!strcmp (ext, "rb")) {
					char *cmd = cmdstr ("ruby");
					r_lang_use (core->lang, "pipe");
					lang_run_file (core, core->lang, cmd);
					free (cmd);
					ret = 1;
				} else if (!strcmp (ext, "vala")) {
					r_lang_use (core->lang, "vala");
					lang_run_file (core, core->lang, file);
					ret = 1;
				} else if (!strcmp (ext, "sh")) {
					char *shell = r_sys_getenv ("SHELL");
					if (!shell) {
						shell = strdup ("sh");
					}
					if (shell) {
						r_lang_use (core->lang, "pipe");
						char *cmd = r_str_newf ("%s '%s'", shell, file);
						if (cmd) {
							lang_run_file (core, core->lang, cmd);
							free (cmd);
						}
						free (shell);
					}
					ret = 1;
				} else if (!strcmp (ext, "pl")) {
					char *cmd = cmdstr ("perl");
					r_lang_use (core->lang, "pipe");
					lang_run_file (core, core->lang, cmd);
					free (cmd);
					ret = 1;
				} else if (!strcmp (ext, "py")) {
					char *cmd = cmdstr ("python");
					r_lang_use (core->lang, "pipe");
					lang_run_file (core, core->lang, cmd);
					free (cmd);
					ret = 1;
				}
			} else {
				char *abspath = r_file_path (file);
				char *lang = langFromHashbang (core, file);
				if (lang) {
					r_lang_use (core->lang, "pipe");
					char *cmd = r_str_newf ("%s '%s'", lang, file);
					lang_run_file (core, core->lang, cmd);
					free (lang);
					free (cmd);
					ret = 1;
				}
				free (abspath);
			}
			if (!ret) {
				ret = r_core_cmd_file (core, file);
			}
		}
	}
	free (r_list_pop (core->scriptstack));
	return ret;
}

static int cmd_ls(void *data, const char *input) { // "ls"
	RCore *core = (RCore *)data;
	const char *arg = strchr (input, ' ');
	if (arg) {
		arg = r_str_trim_head_ro (arg + 1);
	}
	switch (*input) {
	case '?': // "l?"
		eprintf ("Usage: l[es] # ls to list files, le[ss] to less a file\n");
		break;
	case 'e': // "le"
		if (arg) {
			r_core_cmdf (core, "cat %s~..", arg);
		} else {
			eprintf ("Usage: less [file]\n");
		}
		break;
	default: // "ls"
		if (!arg) {
			arg = "";
		}
		if (r_fs_check (core->fs, arg)) {
			r_core_cmdf (core, "md %s", arg);
		} else {
			char *res = r_syscmd_ls (arg);
			if (res) {
				r_cons_print (res);
				free (res);
			}
		}
		break;
	}
	return 0;
}

static int cmd_join(void *data, const char *input) { // "join"
	RCore *core = (RCore *)data;
	char *tmp = strdup (input);
	const char *arg1 = strchr (tmp, ' ');
	if (!arg1) {
		goto beach;
	}
	arg1 = r_str_trim_head_ro (arg1);
	if (!arg1) {
		goto beach;
	}
	char *end = strchr (arg1, ' ');
	if (!end) {
		goto beach;
	}
	*end = '\0';
	const char *arg2 = end+1;
	if (!arg2) {
		goto beach;
	}
	arg2 = r_str_trim_head_ro (arg2);
	switch (*input) {
	case '?': // "join?"
		goto beach;
	default: // "join"
		if (!arg1) {
			arg1 = "";
		}
		if (!arg2) {
			arg2 = "";
		}
		if (!r_fs_check (core->fs, arg1) && !r_fs_check (core->fs, arg2)) {
			char *res = r_syscmd_join (arg1, arg2);
			if (res) {
				r_cons_print (res);
				free (res);
			}
		}
		break;
	}
	free (tmp);
	return 0;
beach:
	eprintf ("Usage: join [file1] [file2] # join the contents of the two files\n");
	free (tmp);
	return 0;
}

static int cmd_stdin(void *data, const char *input) {
	RCore *core = (RCore *)data;
	if (input[0] == '?') {
		r_cons_printf ("Usage: '-' '.-' '. -' do the same\n");
		return false;
	}
	return r_core_run_script (core, "-");
}

static int cmd_interpret(void *data, const char *input) {
	char *str, *ptr, *eol, *rbuf, *filter, *inp;
	const char *host, *port, *cmd;
	RCore *core = (RCore *)data;

	switch (*input) {
	case '\0': // "."
		lastcmd_repeat (core, 0);
		break;
	case ':': // ".:"
		if ((ptr = strchr (input + 1, ' '))) {
			/* .:port cmd */
			/* .:host:port cmd */
			cmd = ptr + 1;
			*ptr = 0;
			eol = strchr (input + 1, ':');
			if (eol) {
				*eol = 0;
				host = input + 1;
				port = eol + 1;
			} else {
				host = "localhost";
				port = input + ((input[1] == ':')? 2: 1);
			}
			rbuf = r_core_rtr_cmds_query (core, host, port, cmd);
			if (rbuf) {
				r_cons_print (rbuf);
				free (rbuf);
			}
		} else {
			r_core_rtr_cmds (core, input + 1);
		}
		break;
	case '.': // ".." same as \n
		if (input[1] == '.') { // "..." run the last command repeated
			// same as \n with e cmd.repeat=true
			lastcmd_repeat (core, 1);
		} else if (input[1]) {
			char *str = r_core_cmd_str_pipe (core, r_str_trim_head_ro (input));
			if (str) {
				r_core_cmd (core, str, 0);
				free (str);
			}
		} else {
			eprintf ("Usage: .. ([file])\n");
		}
		break;
	case '*': // ".*"
		{
			const char *a = r_str_trim_head_ro (input + 1);
			char *s = strdup (a);
			char *sp = strchr (s, ' ');
			if (sp) {
				*sp = 0;
			}
			if (R_STR_ISNOTEMPTY (s)) {
				r_core_run_script (core, s);
			}
			free (s);
		}
		break;
	case '-': // ".-"
		if (input[1] == '?') {
			r_cons_printf ("Usage: '-' '.-' '. -' do the same\n");
		} else {
			r_core_run_script (core, "-");
		}
		break;
	case ' ': // ". "
		{
			const char *script_file = r_str_trim_head_ro (input + 1);
			if (*script_file == '$') {
				r_core_cmd0 (core, script_file);
			} else {
				if (!r_core_run_script (core, script_file)) {
					eprintf ("Cannot find script '%s'\n", script_file);
					core->num->value = 1;
				} else {
					core->num->value = 0;
				}
			}
		}
		break;
	case '!': // ".!"
		/* from command */
		r_core_cmd_command (core, input + 1);
		break;
	case '(': // ".("
		r_cmd_macro_call (&core->rcmd->macro, input + 1);
		break;
	case '?': // ".?"
		r_core_cmd_help (core, help_msg_dot);
		break;
	default:
		if (*input >= 0 && *input <= 9) {
			eprintf ("|ERROR| No .[0..9] to avoid infinite loops\n");
			break;
		}
		inp = strdup (input);
		filter = strchr (inp, '~');
		if (filter) {
			*filter = 0;
		}
		int tmp_html = r_cons_singleton ()->is_html;
		r_cons_singleton ()->is_html = 0;
		ptr = str = r_core_cmd_str (core, inp);
		r_cons_singleton ()->is_html = tmp_html;

		if (filter) {
			*filter = '~';
		}
		r_cons_break_push (NULL, NULL);
		if (ptr) {
			for (;;) {
				if (r_cons_is_breaked ()) {
					break;
				}
				eol = strchr (ptr, '\n');
				if (eol) {
					*eol = '\0';
				}
				if (*ptr) {
					char *p = r_str_append (strdup (ptr), filter);
					r_core_cmd0 (core, p);
					free (p);
				}
				if (!eol) {
					break;
				}
				ptr = eol + 1;
			}
		}
		r_cons_break_pop ();
		free (str);
		free (inp);
		break;
	}
	return 0;
}

static int callback_foreach_kv(void *user, const char *k, const char *v) {
	r_cons_printf ("%s=%s\n", k, v);
	return 1;
}

R_API int r_line_hist_sdb_up(RLine *line) {
	if (!line->sdbshell_hist_iter || !line->sdbshell_hist_iter->n) {
		return false;
	}
	line->sdbshell_hist_iter = line->sdbshell_hist_iter->n;
	strncpy (line->buffer.data, line->sdbshell_hist_iter->data, R_LINE_BUFSIZE - 1);
	line->buffer.index = line->buffer.length = strlen (line->buffer.data);
	return true;
}

R_API int r_line_hist_sdb_down(RLine *line) {
	if (!line->sdbshell_hist_iter || !line->sdbshell_hist_iter->p) {
		return false;
	}
	line->sdbshell_hist_iter = line->sdbshell_hist_iter->p;
	strncpy (line->buffer.data, line->sdbshell_hist_iter->data, R_LINE_BUFSIZE - 1);
	line->buffer.index = line->buffer.length = strlen (line->buffer.data);
	return true;
}

static int cmd_kuery(void *data, const char *input) {
	char buf[1024], *out;
	RCore *core = (RCore*)data;
	const char *sp, *p = "[sdb]> ";
	Sdb *s = core->sdb;

	char *cur_pos, *cur_cmd, *next_cmd = NULL;
	char *temp_pos, *temp_cmd, *temp_storage = NULL;

	switch (input[0]) {

	case 'j':
		out = sdb_querys (s, NULL, 0, "anal/**");
		if (!out) {
			r_cons_println ("No Output from sdb");
			break;
		}

		r_cons_printf ("{\"anal\":{");

		while (*out) {
			cur_pos = strchr (out, '\n');
			if (!cur_pos) {
					break;
			}
			cur_cmd = r_str_ndup (out, cur_pos - out);

			r_cons_printf ("\n\n\"%s\" : [", cur_cmd);

			next_cmd = r_str_newf ("anal/%s/*", cur_cmd);
			temp_storage = sdb_querys (s, NULL, 0, next_cmd);

			if (!temp_storage) {
				r_cons_println ("\nEMPTY\n");
				r_cons_printf ("],\n\n");
				out += cur_pos - out + 1;
				continue;
			}

			while (*temp_storage) {
				temp_pos = strchr (temp_storage, '\n');
				if (!temp_pos) {
					break;
				}

				temp_cmd = r_str_ndup (temp_storage, temp_pos - temp_storage);
				r_cons_printf ("\"%s\",", temp_cmd);
				temp_storage += temp_pos - temp_storage + 1;
			}

			r_cons_printf ("],\n\n");
			out += cur_pos - out + 1;
		}

		r_cons_printf ("}}");
		free (next_cmd);
		free (temp_storage);
		break;

	case ' ':
		out = sdb_querys (s, NULL, 0, input + 1);
		if (out) {
			r_cons_print (out);
		}
		free (out);
		break;
	//case 's': r_pair_save (s, input + 3); break;
	//case 'l': r_pair_load (sdb, input + 3); break;
	case '\0':
		sdb_foreach (s, callback_foreach_kv, NULL);
		break;
	// TODO: add command to list all namespaces // sdb_ns_foreach ?
	case 's': // "ks"
		if (core->http_up) {
			return false;
		}
		if (!r_cons_is_interactive ()) {
			return false;
		}
		if (input[1] == ' ') {
			char *n, *o, *p = strdup (input + 2);
			// TODO: slash split here? or inside sdb_ns ?
			for (n = o = p; n; o = n) {
				n = strchr (o, '/'); // SDB_NS_SEPARATOR NAMESPACE
				if (n) {
					*n++ = 0;
				}
				s = sdb_ns (s, o, 1);
			}
			free (p);
		}
		if (!s) {
			s = core->sdb;
		}
		RLine *line = core->cons->line;
		if (!line->sdbshell_hist) {
			line->sdbshell_hist = r_list_newf (free);
			r_list_append (line->sdbshell_hist, r_str_new ("\0"));
		}
		RList *sdb_hist = line->sdbshell_hist;
		r_line_set_hist_callback (line, &r_line_hist_sdb_up, &r_line_hist_sdb_down);
		for (;;) {
			r_line_set_prompt (p);
			if (r_cons_fgets (buf, sizeof (buf), 0, NULL) < 1) {
				break;
			}
			if (!*buf) {
				break;
			}
			if (sdb_hist) {
				if ((r_list_length (sdb_hist) == 1) || (r_list_length (sdb_hist) > 1 && strcmp (r_list_get_n (sdb_hist, 1), buf))) {
					r_list_insert (sdb_hist, 1, strdup (buf));
				}
				line->sdbshell_hist_iter = sdb_hist->head;
			}
			out = sdb_querys (s, NULL, 0, buf);
			if (out) {
				r_cons_println (out);
				r_cons_flush ();
			}
		}
		r_line_set_hist_callback (core->cons->line, &r_line_hist_cmd_up, &r_line_hist_cmd_down);
		break;
	case 'o':
		if (r_sandbox_enable (0)) {
			eprintf ("This command is disabled in sandbox mode\n");
			return 0;
		}
		if (input[1] == ' ') {
			char *fn = strdup (input + 2);
			if (!fn) {
				eprintf("Unable to allocate memory\n");
				return 0;
			}
			char *ns = strchr (fn, ' ');
			if (ns) {
				Sdb *db;
				*ns++ = 0;
				if (r_file_exists (fn)) {
					db = sdb_ns_path (core->sdb, ns, 1);
					if (db) {
						Sdb *newdb = sdb_new (NULL, fn, 0);
						if (newdb) {
							sdb_drain  (db, newdb);
						} else {
							eprintf ("Cannot open sdb '%s'\n", fn);
						}
					} else {
						eprintf ("Cannot find sdb '%s'\n", ns);
					}
				} else {
					eprintf ("Cannot open file\n");
				}
			} else {
				eprintf ("Missing sdb namespace\n");
			}
			free (fn);
		} else {
			eprintf ("Usage: ko [file] [namespace]\n");
		}
		break;
	case 'd':
		if (r_sandbox_enable (0)) {
			eprintf ("This command is disabled in sandbox mode\n");
			return 0;
		}
		if (input[1] == ' ') {
			char *fn = strdup (input + 2);
			char *ns = strchr (fn, ' ');
			if (ns) {
				*ns++ = 0;
				Sdb *db = sdb_ns_path (core->sdb, ns, 0);
				if (db) {
					sdb_file (db, fn);
					sdb_sync (db);
				} else {
					eprintf ("Cannot find sdb '%s'\n", ns);
				}
			} else {
				eprintf ("Missing sdb namespace\n");
			}
			free (fn);
		} else {
			eprintf ("Usage: kd [file] [namespace]\n");
		}
		break;
	case '?':
		r_core_cmd_help (core, help_msg_k);
		break;
	}

	if (input[0] == '\0') {
		/* nothing more to do, the command has been parsed. */
		return 0;
	}

	sp = strchr (input + 1, ' ');
	if (sp) {
		char *inp = strdup (input);
		inp [(size_t)(sp - input)] = 0;
		s = sdb_ns (core->sdb, inp + 1, 1);
		out = sdb_querys (s, NULL, 0, sp + 1);
		if (out) {
			r_cons_println (out);
			free (out);
		}
		free (inp);
		return 0;
	}
	return 0;
}

static int cmd_bsize(void *data, const char *input) {
	ut64 n;
	RFlagItem *flag;
	RCore *core = (RCore *)data;
	switch (input[0]) {
	case 'm': // "bm"
		n = r_num_math (core->num, input + 1);
		if (n > 1) {
			core->blocksize_max = n;
		} else {
			r_cons_printf ("0x%x\n", (ut32)core->blocksize_max);
		}
		break;
	case '+': // "b+"
		n = r_num_math (core->num, input + 1);
		r_core_block_size (core, core->blocksize + n);
		break;
	case '-': // "b-"
		n = r_num_math (core->num, input + 1);
		r_core_block_size (core, core->blocksize - n);
		break;
	case 'f': // "bf"
		if (input[1] == ' ') {
			flag = r_flag_get (core->flags, input + 2);
			if (flag) {
				r_core_block_size (core, flag->size);
			} else {
				eprintf ("bf: cannot find flag named '%s'\n", input + 2);
			}
		} else {
			eprintf ("Usage: bf [flagname]\n");
		}
		break;
	case 'j': // "bj"
		r_cons_printf ("{\"blocksize\":%d,\"blocksize_limit\":%d}\n", core->blocksize, core->blocksize_max);
		break;
	case '*': // "b*"
		r_cons_printf ("b 0x%x\n", core->blocksize);
		break;
	case '\0': // "b"
		r_cons_printf ("0x%x\n", core->blocksize);
		break;
	case ' ':
		r_core_block_size (core, r_num_math (core->num, input));
		break;
	default:
	case '?': // "b?"
		r_core_cmd_help (core, help_msg_b);
		break;
	}
	return 0;
}

static int __runMain(RMainCallback cb, const char *arg) {
	char *a = r_str_trim_dup (arg);
	int argc = 0;
	char **args = r_str_argv (a, &argc);
	int res = cb (argc, args);
	free (args);
	free (a);
	return res;
}

static bool cmd_r2cmd(RCore *core, const char *_input) {
	char *input = r_str_newf ("r%s", _input);
	int rc = 0;
	if (r_str_startswith (input, "rax2")) {
		rc = __runMain (core->r_main_rax2, input);
	} else if (r_str_startswith (input, "radare2")) {
		r_sys_cmdf ("%s", input);
		// rc = __runMain (core->r_main_radare2, input);
	} else if (r_str_startswith (input, "rasm2")) {
		r_sys_cmdf ("%s", input);
		// rc = __runMain (core->r_main_rasm2, input);
	} else if (r_str_startswith (input, "rabin2")) {
		r_sys_cmdf ("%s", input);
		// rc = __runMain (core->r_main_rabin2, input);
	} else if (r_str_startswith (input, "ragg2")) {
		r_sys_cmdf ("%s", input);
		// rc = __runMain (core->r_main_ragg2, input);
	} else if (r_str_startswith (input, "r2pm")) {
		r_sys_cmdf ("%s", input);
		// rc = __runMain (core->r_main_r2pm, input);
	} else if (r_str_startswith (input, "radiff2")) {
		rc = __runMain (core->r_main_radiff2, input);
	} else {
		const char *r2cmds[] = {
			"rax2", "r2pm", "rasm2", "rabin2", "rahash2", "rafind2", "rarun2", "ragg2", "radare2", "r2", NULL
		};
		int i;
		for (i = 0; r2cmds[i]; i++) {
			if (r_str_startswith (input, r2cmds[i])) {
				free (input);
				return true;
			}
		}
		return false;
	}
	free (input);
	core->num->value = rc;
	return true;
}

static int cmd_rebase(RCore *core, const char *input) {
	ut64 addr = r_num_math (core->num, input);
	if (!addr) {
		r_cons_printf ("Usage: rb oldbase @ newbase\n");
		return 0;
	}
	// old base = addr
	// new base = core->offset
	r_debug_bp_rebase (core->dbg, addr, core->offset);
	r_bin_set_baddr (core->bin, core->offset);
	r_flag_move (core->flags, addr, core->offset);
	r_core_cmd0 (core, ".is*");
	r_core_cmd0 (core, ".iM*");
	r_core_cmd0 (core, ".ii*");
	r_core_cmd0 (core, ".iz*");
	// TODO: r_anal_move :??
	// TODO: differentiate analysis by map ranges (associated with files or memory maps)
	return 0;
}

static int cmd_resize(void *data, const char *input) {
	RCore *core = (RCore *)data;
	ut64 newsize = 0;
	st64 delta = 0;
	int grow, ret;

	if (cmd_r2cmd (core, input)) {
		return true;
	}

	ut64 oldsize = (core->file) ? r_io_fd_size (core->io, core->file->fd): 0;
	switch (*input) {
	case 'a': // "r..."
		if (r_str_startswith (input, "adare2")) {
			__runMain (core->r_main_radare2, input - 1);
		}
		return true;
	case 'b': // "rb" rebase
		return cmd_rebase (core, input + 1);
	case '2': // "r2" // XXX should be handled already in cmd_r2cmd()
		// TODO: use argv[0] instead of 'radare2'
		// TODO: { char **argv = { "r2", NULL }; r_main_radare2 (1, argv); }
		r_sys_cmdf ("radare%s", input);
		return true;
	case 'm': // "rm"
		if (input[1] == ' ') {
			const char *file = r_str_trim_head_ro (input + 2);
			if (*file == '$') {
				r_cmd_alias_del (core->rcmd, file);
			} else {
				r_file_rm (file);
			}
		} else {
			eprintf ("Usage: rm [file]   # removes a file\n");
		}
		return true;
	case '\0':
		if (core->file) {
			if (oldsize != -1) {
				r_cons_printf ("%"PFMT64d"\n", oldsize);
			}
		}
		return true;
	case 'h':
		if (core->file) {
			if (oldsize != -1) {
				char humansz[8];
				r_num_units (humansz, sizeof (humansz), oldsize);
				r_cons_printf ("%s\n", humansz);
			}
		}
		return true;
	case '+': // "r+"
	case '-': // "r-"
		delta = (st64)r_num_math (core->num, input);
		newsize = oldsize + delta;
		break;
	case ' ': // "r "
		newsize = r_num_math (core->num, input + 1);
		if (newsize == 0) {
			if (input[1] == '0') {
				eprintf ("Invalid size\n");
			}
			return false;
		}
		break;
	case 'e':
		write (1, Color_RESET_TERMINAL, strlen (Color_RESET_TERMINAL));
		return true;
	case '?': // "r?"
	default:
		r_core_cmd_help (core, help_msg_r);
		return true;
	}

	grow = (newsize > oldsize);
	if (grow) {
		ret = r_io_resize (core->io, newsize);
		if (ret < 1) {
			eprintf ("r_io_resize: cannot resize\n");
		}
	}
	if (delta && core->offset < newsize) {
		r_io_shift (core->io, core->offset, grow?newsize:oldsize, delta);
	}
	if (!grow) {
		ret = r_io_resize (core->io, newsize);
		if (ret < 1) {
			eprintf ("r_io_resize: cannot resize\n");
		}
	}
	if (newsize < core->offset+core->blocksize || oldsize < core->offset + core->blocksize) {
		r_core_block_read (core);
	}
	return true;
}

static int cmd_panels(void *data, const char *input) {
	RCore *core = (RCore*) data;
	if (core->vmode) {
		return false;
	}
	if (*input == '?') {
		eprintf ("Usage: v[*i]\n");
		eprintf ("v.test    # save current layout with name test\n");
		eprintf ("v test    # load saved layout with name test\n");
		eprintf ("vi ...    # launch 'cfg.editor'\n");
		return false;
	}
	if (*input == ' ') {
		if (core->panels) {
			r_load_panels_layout (core, input + 1);
		}
		r_config_set (core->config, "scr.layout", input + 1);
		return true;
	}
	if (*input == '=') {
		r_save_panels_layout (core, input + 1);
		r_config_set (core->config, "scr.layout", input + 1);
		return true;
	}
	if (*input == 'i') {
		char *sp = strchr (input, ' ');
		if (sp) {
			char *r = r_core_editor (core, sp + 1, NULL);
			if (r) {
				free (r);
			} else {
				eprintf ("Cannot open file (%s)\n", sp + 1);
			}
		}
		////r_sys_cmdf ("v%s", input);
		return false;
	}
	r_core_visual_panels_root (core, core->panels_root);
	return true;
}

static int cmd_visual(void *data, const char *input) {
	RCore *core = (RCore*) data;
	if (core->http_up) {
		return false;
	}
	if (!r_cons_is_interactive ()) {
		return false;
	}
	return r_core_visual ((RCore *)data, input);
}

static int cmd_pipein(void *user, const char *input) {
	char *buf = strdup (input);
	int len = r_str_unescape (buf);
	r_cons_readpush (buf, len);
	free (buf);
	return 0;
}

static int cmd_tasks(void *data, const char *input) {
	RCore *core = (RCore*) data;
	switch (input[0]) {
	case '\0': // "&"
	case 'j': // "&j"
		r_core_task_list (core, *input);
		break;
	case 'b': { // "&b"
		if (r_sandbox_enable (0)) {
			eprintf ("This command is disabled in sandbox mode\n");
			return 0;
		}
		int tid = r_num_math (core->num, input + 1);
		if (tid) {
			r_core_task_break (&core->tasks, tid);
		}
		break;
	}
	case '&': { // "&&"
		if (r_sandbox_enable (0)) {
			eprintf ("This command is disabled in sandbox mode\n");
			return 0;
		}
		int tid = r_num_math (core->num, input + 1);
		r_core_task_join (&core->tasks, core->tasks.current_task, tid ? tid : -1);
		break;
	}
	case '=': { // "&="
		// r_core_task_list (core, '=');
		int tid = r_num_math (core->num, input + 1);
		if (tid) {
			RCoreTask *task = r_core_task_get_incref (&core->tasks, tid);
			if (task) {
				if (task->res) {
					r_cons_println (task->res);
				}
				r_core_task_decref (task);
			} else {
				eprintf ("Cannot find task\n");
			}
		}
		break;
	}
	case '-': // "&-"
		if (r_sandbox_enable (0)) {
			eprintf ("This command is disabled in sandbox mode\n");
			return 0;
		}
		if (input[1] == '*') {
			r_core_task_del_all_done (&core->tasks);
		} else {
			r_core_task_del (&core->tasks, r_num_math (core->num, input + 1));
		}
		break;
	case '?': // "&?"
	default:
		helpCmdTasks (core);
		break;
	case ' ': // "& "
	case '_': // "&_"
	case 't': { // "&t"
		if (r_sandbox_enable (0)) {
			eprintf ("This command is disabled in sandbox mode\n");
			return 0;
		}
		RCoreTask *task = r_core_task_new (core, true, input + 1, NULL, core);
		if (!task) {
			break;
		}
		task->transient = input[0] == 't';
		r_core_task_enqueue (&core->tasks, task);
		break;
	}
	}
	return 0;
}

static int cmd_pointer(void *data, const char *input) {
	RCore *core = (RCore*) data;
	int ret = true;
	char *str, *eq;
	input = r_str_trim_head_ro (input);
	while (*input == ' ') {
		input++;
	}
	if (!*input || *input == '?') {
		r_core_cmd_help (core, help_msg_star);
		return ret;
	}
	str = strdup (input);
	eq = strchr (str, '=');
	if (eq) {
		*eq++ = 0;
		if (!strncmp (eq, "0x", 2)) {
			ret = r_core_cmdf (core, "wv %s@%s", eq, str);
		} else {
			ret = r_core_cmdf (core, "wx %s@%s", eq, str);
		}
	} else {
		ret = r_core_cmdf (core, "?v [%s]", input);
	}
	free (str);
	return ret;
}

static int cmd_env(void *data, const char *input) {
	RCore *core = (RCore*)data;
	int ret = true;
	switch (*input) {
	case '?':
		cmd_help_percent (core);
		break;
	default:
		ret = r_core_cmdf (core, "env %s", input);
	}
	return ret;
}

static struct autocomplete_flag_map_t {
	const char* name;
	const char* desc;
	int type;
} autocomplete_flags [] = {
	{ "$dflt", "default autocomplete flag", R_CORE_AUTOCMPLT_DFLT },
	{ "$flag", "shows known flag hints", R_CORE_AUTOCMPLT_FLAG },
	{ "$flsp", "shows known flag-spaces hints", R_CORE_AUTOCMPLT_FLSP },
	{ "$zign", "shows known zignatures hints", R_CORE_AUTOCMPLT_ZIGN },
	{ "$eval", "shows known evals hints", R_CORE_AUTOCMPLT_EVAL },
	{ "$prjt", "shows known projects hints", R_CORE_AUTOCMPLT_PRJT },
	{ "$mins", NULL, R_CORE_AUTOCMPLT_MINS },
	{ "$brkp", "shows known breakpoints hints", R_CORE_AUTOCMPLT_BRKP },
	{ "$macro", NULL, R_CORE_AUTOCMPLT_MACR },
	{ "$file", "hints file paths", R_CORE_AUTOCMPLT_FILE },
	{ "$thme", "shows known themes hints", R_CORE_AUTOCMPLT_THME },
	{ "$optn", "allows the selection for multiple options", R_CORE_AUTOCMPLT_OPTN },
	{ "$ms", "shows mount hints", R_CORE_AUTOCMPLT_MS},
	{ "$sdb", "shows sdb hints", R_CORE_AUTOCMPLT_SDB},
	{ NULL, NULL, 0 }
};

static inline void print_dict(RCoreAutocomplete* a, int sub) {
	if (!a) {
		return;
	}
	int i, j;
	const char* name = "unknown";
	for (i = 0; i < a->n_subcmds; i++) {
		RCoreAutocomplete* b = a->subcmds[i];
		if (b->locked) {
			continue;
		}
		for (j = 0; j < R_CORE_AUTOCMPLT_END; j++) {
			if (b->type == autocomplete_flags[j].type) {
				name = autocomplete_flags[j].name;
				break;
			}
		}
		eprintf ("[%3d] %s: '%s'\n", sub, name, b->cmd);
		print_dict (a->subcmds[i], sub + 1);
	}
}

static int autocomplete_type(const char* strflag) {
	int i;
	for (i = 0; i < R_CORE_AUTOCMPLT_END; i++) {
		if (autocomplete_flags[i].desc && !strncmp (strflag, autocomplete_flags[i].name, 5)) {
			return autocomplete_flags[i].type;
		}
	}
	eprintf ("Invalid flag '%s'\n", strflag);
	return R_CORE_AUTOCMPLT_END;
}

static void cmd_autocomplete(RCore *core, const char *input) {
	RCoreAutocomplete* b = core->autocomplete;
	input = r_str_trim_head_ro (input);
	char arg[256];
	if (!*input) {
		print_dict (core->autocomplete, 0);
		return;
	}
	if (*input == '?') {
		r_core_cmd_help (core, help_msg_triple_exclamation);
		int i;
		r_cons_printf ("|Types:\n");
		for (i = 0; i < R_CORE_AUTOCMPLT_END; i++) {
			if (autocomplete_flags[i].desc) {
				r_cons_printf ("| %s     %s\n",
					autocomplete_flags[i].name,
					autocomplete_flags[i].desc);
			}
		}
		return;
	}
	if (*input == '-') {
		const char *arg = input + 1;
		if (!*input) {
			eprintf ("Use !!!-* or !!!-<cmd>\n");
			return;
		}
		r_core_autocomplete_remove (b, arg);
		return;
	}
	while (b) {
		const char* end = r_str_trim_head_wp (input);
		if (!end) {
			break;
		}
		if ((end - input) >= sizeof (arg)) {
			// wtf?
			eprintf ("Exceeded the max arg length (255).\n");
			return;
		}
		if (end == input) {
			break;
		}
		memcpy (arg, input, end - input);
		arg[end - input] = 0;
		RCoreAutocomplete* a = r_core_autocomplete_find (b, arg, true);
		input = r_str_trim_head_ro (end);
		if (input && *input && !a) {
			if (b->type == R_CORE_AUTOCMPLT_DFLT && !(b = r_core_autocomplete_add (b, arg, R_CORE_AUTOCMPLT_DFLT, false))) {
				eprintf ("ENOMEM\n");
				return;
			} else if (b->type != R_CORE_AUTOCMPLT_DFLT) {
				eprintf ("Cannot add autocomplete to '%s'. type not $dflt\n", b->cmd);
				return;
			}
		} else if ((!input || !*input) && !a) {
			if (arg[0] == '$') {
				int type = autocomplete_type (arg);
				if (type != R_CORE_AUTOCMPLT_END && !b->locked && !b->n_subcmds) {
					b->type = type;
				} else if (b->locked || b->n_subcmds) {
					if (!b->cmd) {
						return;
					}
					eprintf ("Changing type of '%s' is forbidden.\n", b->cmd);
				}
			} else {
				if (!r_core_autocomplete_add (b, arg, R_CORE_AUTOCMPLT_DFLT, false)) {
					eprintf ("ENOMEM\n");
					return;
				}
			}
			return;
		} else if ((!input || !*input) && a) {
			// eprintf ("Cannot add '%s'. Already exists.\n", arg);
			return;
		} else {
			b = a;
		}
	}
	eprintf ("Invalid usage of !!!\n");
}

static int cmd_last(void *data, const char *input) {
	switch (*input) {
	case 0:
		r_cons_last ();
		break;
	default:
		eprintf ("Usage: _  print last output\n");
	}
	return 0;
}

static int cmd_system(void *data, const char *input) {
	RCore *core = (RCore*)data;
	ut64 n;
	int ret = 0;
	switch (*input) {
	case '-': //!-
		if (input[1]) {
			r_line_hist_free();
			r_line_hist_save (R2_HOME_HISTORY);
		} else {
			r_line_hist_free();
		}
		break;
	case '=': //!=
		if (input[1] == '?') {
			r_cons_printf ("Usage: !=[!]  - enable/disable remote commands\n");
		} else {
			if (!r_sandbox_enable (0)) {
				core->cmdremote = input[1]? 1: 0;
				r_cons_println (r_str_bool (core->cmdremote));
			}
		}
		break;
	case '!': //!!
		if (input[1] == '!') { // !!! & !!!-
			cmd_autocomplete (core, input + 2);
		} else if (input[1] == '?') {
			cmd_help_exclamation (core);
		} else if (input[1] == '*') {
			char *cmd = r_str_trim_dup (input + 1);
			(void)r_core_cmdf (core, "\"#!pipe %s\"", cmd);
			free (cmd);
		} else {
			if (r_sandbox_enable (0)) {
				eprintf ("This command is disabled in sandbox mode\n");
				return 0;
			}
			if (input[1]) {
				int olen;
				char *out = NULL;
				char *cmd = r_core_sysenv_begin (core, input);
				if (cmd) {
					void *bed = r_cons_sleep_begin ();
					ret = r_sys_cmd_str_full (cmd + 1, NULL, &out, &olen, NULL);
					r_cons_sleep_end (bed);
					r_core_sysenv_end (core, input);
					r_cons_memcat (out, olen);
					free (out);
					free (cmd);
				} //else eprintf ("Error setting up system environment\n");
			} else {
				eprintf ("History saved to "R2_HOME_HISTORY"\n");
				r_line_hist_save (R2_HOME_HISTORY);
			}
		}
		break;
	case '\0':
		r_line_hist_list ();
		break;
	case '?': //!?
		cmd_help_exclamation (core);
		break;
	case '*':
		// TODO: use the api
		{
		char *cmd = r_str_trim_dup (input + 1);
		cmd = r_str_replace (cmd, " ", "\\ ", true);
		cmd = r_str_replace (cmd, "\\ ", " ", false);
		cmd = r_str_replace (cmd, "\"", "'", false);
		ret = r_core_cmdf (core, "\"#!pipe %s\"", cmd);
		free (cmd);
		}
		break;
	default:
		n = atoi (input);
		if (*input == '0' || n > 0) {
			const char *cmd = r_line_hist_get (n);
			if (cmd) {
				r_core_cmd0 (core, cmd);
			}
			//else eprintf ("Error setting up system environment\n");
		} else {
			char *cmd = r_core_sysenv_begin (core, input);
			if (cmd) {
				void *bed = r_cons_sleep_begin ();
				ret = r_sys_cmd (cmd);
				r_cons_sleep_end (bed);
				r_core_sysenv_end (core, input);
				free (cmd);
			} else {
				eprintf ("Error setting up system environment\n");
			}
		}
		break;
	}
	return ret;
}

#if __WINDOWS__
#include <tchar.h>
#define __CLOSE_DUPPED_PIPES() \
		close (1);             \
		close (fd_out);        \
		fd_out = -1;

static void r_w32_cmd_pipe(RCore *core, char *radare_cmd, char *shell_cmd) {
	STARTUPINFO si = {0};
	PROCESS_INFORMATION pi = {0};
	SECURITY_ATTRIBUTES sa;
	HANDLE pipe[2] = {NULL, NULL};
	int fd_out = -1, cons_out = -1;
	char *_shell_cmd = NULL;
	LPTSTR _shell_cmd_ = NULL;
	DWORD mode;
	TCHAR *systemdir = NULL;
	GetConsoleMode (GetStdHandle (STD_OUTPUT_HANDLE), &mode);

	sa.nLength = sizeof (SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;
	if (!CreatePipe (&pipe[0], &pipe[1], &sa, 0)) {
		r_sys_perror ("r_w32_cmd_pipe/CreatePipe");
		goto err_r_w32_cmd_pipe;
	}
	if (!SetHandleInformation (pipe[1], HANDLE_FLAG_INHERIT, 0)) {
		r_sys_perror ("r_w32_cmd_pipe/SetHandleInformation");
		goto err_r_w32_cmd_pipe;
	}
	si.hStdError = GetStdHandle (STD_ERROR_HANDLE);
	si.hStdOutput = GetStdHandle (STD_OUTPUT_HANDLE);
	si.hStdInput = pipe[0];
	si.dwFlags |= STARTF_USESTDHANDLES;
	si.cb = sizeof (si);
	_shell_cmd = shell_cmd;
	while (*_shell_cmd && isspace ((ut8)*_shell_cmd)) {
		_shell_cmd++;
	}
	char *tmp = r_str_newf ("/Q /c \"%s\"", _shell_cmd);
	if (!tmp) {
		goto err_r_w32_cmd_pipe;
	}
	_shell_cmd = tmp;
	_shell_cmd_ = r_sys_conv_utf8_to_win (_shell_cmd);
	free (tmp);
	if (!_shell_cmd_) {
		goto err_r_w32_cmd_pipe;
	}
	systemdir = calloc (MAX_PATH, sizeof (TCHAR));
	if (!systemdir) {
		goto err_r_w32_cmd_pipe;
	}
	int ret = GetSystemDirectory (systemdir, MAX_PATH);
	if (!ret) {
		r_sys_perror ("r_w32_cmd_pipe/systemdir");
		goto err_r_w32_cmd_pipe;
	}
	_tcscat_s (systemdir, MAX_PATH, TEXT("\\cmd.exe"));
	// exec windows process
	if (!CreateProcess (systemdir, _shell_cmd_, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		r_sys_perror ("r_w32_cmd_pipe/CreateProcess");
		goto err_r_w32_cmd_pipe;
	}
	fd_out = _open_osfhandle ((intptr_t)pipe[1], _O_WRONLY|_O_TEXT);
	if (fd_out == -1) {
		perror ("_open_osfhandle");
		goto err_r_w32_cmd_pipe;
	}
	cons_out = dup (1);
	dup2 (fd_out, 1);
	// exec radare command
	r_core_cmd (core, radare_cmd, 0);

	HANDLE th = CreateThread (NULL, 0,(LPTHREAD_START_ROUTINE) r_cons_flush, NULL, 0, NULL);
	if (!th) {
		__CLOSE_DUPPED_PIPES ();
		goto err_r_w32_cmd_pipe;
	}
	while (true) {
		int ret = WaitForSingleObject (th, 50);
		if (!ret) {
			// Successfully written everything to pipe
			__CLOSE_DUPPED_PIPES ();
			WaitForSingleObject (pi.hProcess, INFINITE);
			break;
		}
		ret = WaitForSingleObject (pi.hProcess, 50);
		if (!ret) {
			// Process exited before we finished writing to pipe
			DWORD exit;
			if (GetExitCodeThread (th, &exit) && exit == STILL_ACTIVE) {
				CancelSynchronousIo (th);
			}
			WaitForSingleObject (th, INFINITE);
			__CLOSE_DUPPED_PIPES ();
			break;
		}
	}
	CloseHandle (th);
err_r_w32_cmd_pipe:
	if (pi.hProcess) {
		CloseHandle (pi.hProcess);
	}
	if (pi.hThread) {
		CloseHandle (pi.hThread);
	}
	if (pipe[0]) {
		CloseHandle (pipe[0]);
	}
	if (fd_out != -1) {
		close (fd_out);
	}
	if (cons_out != -1) {
		dup2 (cons_out, 1);
		close (cons_out);
	}
	free (systemdir);
	free (_shell_cmd_);
	SetConsoleMode (GetStdHandle (STD_OUTPUT_HANDLE), mode);
}
#undef __CLOSE_DUPPED_PIPES
#endif

R_API int r_core_cmd_pipe(RCore *core, char *radare_cmd, char *shell_cmd) {
#if __UNIX__
	int stdout_fd, fds[2];
	int child;
#endif
	int si, olen, ret = -1, pipecolor = -1;
	char *str, *out = NULL;

	if (r_sandbox_enable (0)) {
		eprintf ("Pipes are not allowed in sandbox mode\n");
		return -1;
	}
	si = r_cons_is_interactive ();
	r_config_set_i (core->config, "scr.interactive", 0);
	if (!r_config_get_i (core->config, "scr.color.pipe")) {
		pipecolor = r_config_get_i (core->config, "scr.color");
		r_config_set_i (core->config, "scr.color", COLOR_MODE_DISABLED);
	}
	if (*shell_cmd=='!') {
		r_cons_grep_parsecmd (shell_cmd, "\"");
		olen = 0;
		out = NULL;
		// TODO: implement foo
		str = r_core_cmd_str (core, radare_cmd);
		r_sys_cmd_str_full (shell_cmd + 1, str, &out, &olen, NULL);
		free (str);
		r_cons_memcat (out, olen);
		free (out);
		ret = 0;
	}
#if __UNIX__
	r_str_trim_head (radare_cmd);
	r_str_trim_head (shell_cmd);

	r_sys_signal (SIGPIPE, SIG_IGN);
	stdout_fd = dup (1);
	if (stdout_fd != -1) {
		if (pipe (fds) == 0) {
			child = r_sys_fork ();
			if (child == -1) {
				eprintf ("Cannot fork\n");
				close (stdout_fd);
			} else if (child) {
				dup2 (fds[1], 1);
				close (fds[1]);
				close (fds[0]);
				r_core_cmd (core, radare_cmd, 0);
				r_cons_flush ();
				close (1);
				wait (&ret);
				dup2 (stdout_fd, 1);
				close (stdout_fd);
			} else {
				close (fds[1]);
				dup2 (fds[0], 0);
				//dup2 (1, 2); // stderr goes to stdout
				r_sandbox_system (shell_cmd, 0);
				close (stdout_fd);
			}
		} else {
			eprintf ("r_core_cmd_pipe: Could not pipe\n");
		}
	}
#elif __WINDOWS__
	r_w32_cmd_pipe (core, radare_cmd, shell_cmd);
#else
#ifdef _MSC_VER
#pragma message ("r_core_cmd_pipe UNIMPLEMENTED FOR THIS PLATFORM")
#else
#warning r_core_cmd_pipe UNIMPLEMENTED FOR THIS PLATFORM
#endif
	eprintf ("r_core_cmd_pipe: unimplemented for this platform\n");
#endif
	if (pipecolor != -1) {
		r_config_set_i (core->config, "scr.color", pipecolor);
	}
	r_config_set_i (core->config, "scr.interactive", si);
	return ret;
}

static char *parse_tmp_evals(RCore *core, const char *str) {
	char *s = strdup (str);
	int i, argc = r_str_split (s, ',');
	char *res = strdup ("");
	if (!s || !res) {
		free (s);
		free (res);
		return NULL;
	}
	for (i = 0; i < argc; i++) {
		char *eq, *kv = (char *)r_str_word_get0 (s, i);
		if (!kv) {
			break;
		}
		eq = strchr (kv, '=');
		if (eq) {
			*eq = 0;
			const char *ov = r_config_get (core->config, kv);
			if (!ov) {
				continue;
			}
			char *cmd = r_str_newf ("e %s=%s;", kv, ov);
			if (!cmd) {
				free (s);
				free (res);
				return NULL;
			}
			res = r_str_prepend (res, cmd);
			free (cmd);
			r_config_set (core->config, kv, eq + 1);
			*eq = '=';
		} else {
			eprintf ("Missing '=' in e: expression (%s)\n", kv);
		}
	}
	free (s);
	return res;
}

static int r_core_cmd_subst(RCore *core, char *cmd) {
	ut64 rep = strtoull (cmd, NULL, 10);
	int ret = 0, orep;
	char *cmt, *colon = NULL, *icmd = NULL;
	bool tmpseek = false;
	bool original_tmpseek = core->tmpseek;

	if (r_str_startswith (cmd, "GET /cmd/")) {
		memmove (cmd, cmd + 9, strlen (cmd + 9) + 1);
		char *http = strstr (cmd, "HTTP");
		if (http) {
			*http = 0;
			http--;
			if (*http == ' ') {
				*http = 0;
			}
		}
		r_cons_printf ("HTTP/1.0 %d %s\r\n%s"
				"Connection: close\r\nContent-Length: %d\r\n\r\n",
				200, "OK", "", -1);
		return r_core_cmd0 (core, cmd);
	}

	/* must store a local orig_offset because there can be
	 * nested call of this function */
	ut64 orig_offset = core->offset;
	icmd = strdup (cmd);
	if (!icmd) {
		goto beach;
	}

	if (core->max_cmd_depth - core->cons->context->cmd_depth == 1) {
		core->prompt_offset = core->offset;
	}
	cmd = (char *)r_str_trim_head_ro (icmd);
	r_str_trim_tail (cmd);
	// lines starting with # are ignored (never reach cmd_hash()), except #! and #?
	if (!*cmd) {
		if (core->cmdrepeat > 0) {
			lastcmd_repeat (core, true);
			ret = r_core_cmd_nullcallback (core);
		}
		goto beach;
	}
	if (!icmd || (cmd[0] == '#' && cmd[1] != '!' && cmd[1] != '?')) {
		goto beach;
	}
	cmt = *icmd ? (char *)r_str_firstbut (icmd, '#', "\""): NULL;
	if (cmt && (cmt[1] == ' ' || cmt[1] == '\t')) {
		*cmt = 0;
	}
	if (*cmd != '"') {
		if (!strchr (cmd, '\'')) { // allow | awk '{foo;bar}' // ignore ; if there's a single quote
			if ((colon = strchr (cmd, ';'))) {
				*colon = 0;
			}
		}
	} else {
		colon = NULL;
	}
	if (rep > 0) {
		while (IS_DIGIT (*cmd)) {
			cmd++;
		}
		// do not repeat null cmd
		if (!*cmd) {
			goto beach;
		}
	}
	if (rep < 1) {
		rep = 1;
	}
	// XXX if output is a pipe then we don't want to be interactive
	if (rep > 1 && r_sandbox_enable (0)) {
		eprintf ("Command repeat sugar disabled in sandbox mode (%s)\n", cmd);
		goto beach;
	} else {
		if (rep > INTERACTIVE_MAX_REP) {
			if (r_cons_is_interactive ()) {
				if (!r_cons_yesno ('n', "Are you sure to repeat this %"PFMT64d" times? (y/N)", rep)) {
					goto beach;
				}
			}
		}
	}
	// TODO: store in core->cmdtimes to speedup ?
	const char *cmdrep = core->cmdtimes ? core->cmdtimes: "";
	orep = rep;

	r_cons_break_push (NULL, NULL);

	int ocur_enabled = core->print && core->print->cur_enabled;
	while (rep-- && *cmd) {
		if (core->print) {
			core->print->cur_enabled = false;
			if (ocur_enabled && core->seltab >= 0) {
				if (core->seltab == core->curtab) {
					core->print->cur_enabled = true;
				}
			}
		}
		if (r_cons_is_breaked ()) {
			break;
		}
		char *cr = strdup (cmdrep);
		core->break_loop = false;
		ret = r_core_cmd_subst_i (core, cmd, colon, (rep == orep - 1) ? &tmpseek : NULL);
		if (ret && *cmd == 'q') {
			free (cr);
			goto beach;
		}
		if (core->break_loop) {
			free (cr);
			break;
		}
		if (cr && *cr && orep > 1) {
			// XXX: do not flush here, we need r_cons_push () and r_cons_pop()
			r_cons_flush ();
			// XXX: we must import register flags in C
			(void)r_core_cmd0 (core, ".dr*");
			(void)r_core_cmd0 (core, cr);
		}
		free (cr);
	}

	r_cons_break_pop ();

	if (tmpseek) {
		r_core_seek (core, orig_offset, 1);
		core->tmpseek = original_tmpseek;
	}
	if (core->print) {
		core->print->cur_enabled = ocur_enabled;
	}
	if (colon && colon[1]) {
		for (++colon; *colon == ';'; colon++) {
			;
		}
		r_core_cmd_subst (core, colon);
	} else {
		if (!*icmd) {
			r_core_cmd_nullcallback (core);
		}
	}
beach:
	free (icmd);
	return ret;
}

static char *find_eoq(char *p) {
	for (; *p; p++) {
		if (*p == '"') {
			break;
		}
		if (*p == '\\' && p[1] == '"') {
			p++;
		}
	}
	return p;
}

static char* findSeparator(char *p) {
	char *q = strchr (p, '+');
	if (q) {
		return q;
	}
	return strchr (p, '-');
}

static void tmpenvs_free(void *item) {
	r_sys_setenv (item, NULL);
	free (item);
}

static bool set_tmp_arch(RCore *core, char *arch, char **tmparch) {
	r_return_val_if_fail (tmparch, false);
	*tmparch = strdup (r_config_get (core->config, "asm.arch"));
	r_config_set (core->config, "asm.arch", arch);
	core->fixedarch = true;
	return true;
}

static bool set_tmp_bits(RCore *core, int bits, char **tmpbits, int *cmd_ignbithints) {
	r_return_val_if_fail (tmpbits, false);
	*tmpbits = strdup (r_config_get (core->config, "asm.bits"));
	r_config_set_i (core->config, "asm.bits", bits);
	core->fixedbits = true;
	// XXX: why?
	*cmd_ignbithints = r_config_get_i (core->config, "anal.ignbithints");
	r_config_set_i (core->config, "anal.ignbithints", 1);
	return true;
}

static int r_core_cmd_subst_i(RCore *core, char *cmd, char *colon, bool *tmpseek) {
	RList *tmpenvs = r_list_newf (tmpenvs_free);
	const char *quotestr = "`";
	const char *tick = NULL;
	char *ptr, *ptr2, *str;
	char *arroba = NULL;
	char *grep = NULL;
	RIODesc *tmpdesc = NULL;
	int pamode = !core->io->va;
	int i, ret = 0, pipefd;
	bool usemyblock = false;
	int scr_html = -1;
	int scr_color = -1;
	bool eos = false;
	bool haveQuote = false;
	bool oldfixedarch = core->fixedarch;
	bool oldfixedbits = core->fixedbits;
	bool cmd_tmpseek = false;
	ut64 tmpbsz = core->blocksize;
	int cmd_ignbithints = -1;

	if (!cmd) {
		r_list_free (tmpenvs);
		return 0;
	}
	r_str_trim (cmd);

	char *$0 = strstr (cmd, "$(");
	if ($0) {
		char *$1 = strchr ($0 + 2, ')');
		if ($1) {
			*$0 = '`';
			*$1 = '`';
			memmove ($0 + 1, $0 + 2, strlen ($0 + 2) + 1);
		} else {
			eprintf ("Unterminated $() block\n");
		}
	}

	/* quoted / raw command */
	switch (*cmd) {
	case '.':
		if (cmd[1] == '"') { /* interpret */
			r_list_free (tmpenvs);
			return r_cmd_call (core->rcmd, cmd);
		}
		break;
	case '"':
		for (; *cmd; ) {
			int pipefd = -1;
			ut64 oseek = UT64_MAX;
			char *line, *p;
			haveQuote = *cmd == '"';
			if (haveQuote) {
				cmd++;
				p = *cmd ? find_eoq (cmd) : NULL;
				if (!p || !*p) {
					eprintf ("Missing \" in (%s).", cmd);
					r_list_free (tmpenvs);
					return false;
				}
				*p++ = 0;
				if (!*p) {
					eos = true;
				}
			} else {
				char *sc = strchr (cmd, ';');
				if (sc) {
					*sc = 0;
				}
				r_core_cmd0 (core, cmd);
				if (!sc) {
					break;
				}
				cmd = sc + 1;
				continue;
			}
			char op0 = 0;
			if (*p) {
				// workaround :D
				if (p[0] == '@') {
					p--;
				}
				while (p[1] == ';' || IS_WHITESPACE (p[1])) {
					p++;
				}
				if (p[1] == '@' || (p[1] && p[2] == '@')) {
					char *q = strchr (p + 1, '"');
					if (q) {
						op0 = *q;
						*q = 0;
					}
					haveQuote = q != NULL;
					oseek = core->offset;
					r_core_seek (core, r_num_math (core->num, p + 2), 1);
					if (q) {
						*p = '"';
						p = q;
					} else {
						p = strchr (p + 1, ';');
					}
				}
				if (p && *p && p[1] == '>') {
					str = p + 2;
					while (*str == '>') {
						str++;
					}
					str = (char *)r_str_trim_head_ro (str);
					r_cons_flush ();
					const bool append = p[2] == '>';
					pipefd = r_cons_pipe_open (str, 1, append);
				}
			}
			line = strdup (cmd);
			line = r_str_replace (line, "\\\"", "\"", true);
			if (p && *p && p[1] == '|') {
				str = p + 2;
				while (IS_WHITESPACE (*str)) {
					str++;
				}
				r_core_cmd_pipe (core, cmd, str);
			} else {
				r_cmd_call (core->rcmd, line);
			}
			free (line);
			if (oseek != UT64_MAX) {
				r_core_seek (core, oseek, 1);
			}
			if (pipefd != -1) {
				r_cons_flush ();
				r_cons_pipe_close (pipefd);
			}
			if (!p) {
				break;
			}
			if (eos) {
				break;
			}
			if (haveQuote) {
				if (*p == ';') {
					cmd = p + 1;
				} else {
					if (*p == '"') {
						cmd = p;
					} else {
						*p = op0;
						cmd = p;
					}
				}
			} else {
				cmd = p + 1;
			}
		}
		r_list_free (tmpenvs);
		return true;
	case '(':
		if (cmd[1] != '*' && !strstr (cmd, ")()")) {
			r_list_free (tmpenvs);
			return r_cmd_call (core->rcmd, cmd);
		}
		break;
	case '?':
		if (cmd[1] == '>') {
			r_core_cmd_help (core, help_msg_greater_sign);
			r_list_free (tmpenvs);
			return true;
		}
	}

// TODO must honor `
	/* comments */
	if (*cmd != '#') {
		ptr = (char *)r_str_firstbut (cmd, '#', "`\""); // TODO: use quotestr here
		if (ptr && (ptr[1] == ' ' || ptr[1] == '\t')) {
			*ptr = '\0';
		}
	}

	/* multiple commands */
	// TODO: must honor " and ` boundaries
	//ptr = strrchr (cmd, ';');
	if (*cmd != '#') {
		ptr = (char *)r_str_lastbut (cmd, ';', quotestr);
		if (colon && ptr) {
			int ret ;
			*ptr = '\0';
			if (r_core_cmd_subst (core, cmd) == -1) {
				r_list_free (tmpenvs);
				return -1;
			}
			cmd = ptr + 1;
			ret = r_core_cmd_subst (core, cmd);
			*ptr = ';';
			r_list_free (tmpenvs);
			return ret;
			//r_cons_flush ();
		}
	}

	// TODO must honor " and `
	/* pipe console to shell process */
	//ptr = strchr (cmd, '|');
	ptr = (char *)r_str_lastbut (cmd, '|', quotestr);
	if (ptr) {
		if (ptr > cmd) {
			char *ch = ptr - 1;
			if (*ch == '\\') {
				memmove (ch, ptr, strlen (ptr) + 1);
				goto escape_pipe;
			}
		}
		char *ptr2 = strchr (cmd, '`');
		if (!ptr2 || (ptr2 && ptr2 > ptr)) {
			if (!tick || (tick && tick > ptr)) {
				*ptr = '\0';
				cmd = r_str_trim_nc (cmd);
				if (!strcmp (ptr + 1, "?")) { // "|?"
					r_core_cmd_help (core, help_msg_vertical_bar);
					r_list_free (tmpenvs);
					return ret;
				} else if (!strncmp (ptr + 1, "H", 1)) { // "|H"
					scr_html = r_config_get_i (core->config, "scr.html");
					r_config_set_i (core->config, "scr.html", true);
				} else if (!strcmp (ptr + 1, "T")) { // "|T"
					scr_color = r_config_get_i (core->config, "scr.color");
					r_config_set_i (core->config, "scr.color", COLOR_MODE_DISABLED);
					core->cons->use_tts = true;
				} else if (!strcmp (ptr + 1, ".")) { // "|."
					ret = *cmd ? r_core_cmdf (core, ".%s", cmd) : 0;
					r_list_free (tmpenvs);
					return ret;
				} else if (ptr[1]) { // "| grep .."
					int value = core->num->value;
					if (*cmd) {
						r_core_cmd_pipe (core, cmd, ptr + 1);
					} else {
						char *res = r_io_system (core->io, ptr + 1);
						if (res) {
							r_cons_printf ("%s\n", res);
							free (res);
						}
					}
					core->num->value = value;
					r_list_free (tmpenvs);
					return 0;
				} else { // "|"
					scr_html = r_config_get_i (core->config, "scr.html");
					r_config_set_i (core->config, "scr.html", 0);
					scr_color = r_config_get_i (core->config, "scr.color");
					r_config_set_i (core->config, "scr.color", COLOR_MODE_DISABLED);
				}
			}
		}
	}
escape_pipe:

	// TODO must honor " and `
	/* bool conditions */
	ptr = (char *)r_str_lastbut (cmd, '&', quotestr);
	//ptr = strchr (cmd, '&');
	while (ptr && *ptr && ptr[1] == '&') {
		*ptr = '\0';
		ret = r_cmd_call (core->rcmd, cmd);
		if (ret == -1) {
			eprintf ("command error(%s)\n", cmd);
			if (scr_html != -1) {
				r_config_set_i (core->config, "scr.html", scr_html);
			}
			if (scr_color != -1) {
				r_config_set_i (core->config, "scr.color", scr_color);
			}
			r_list_free (tmpenvs);
			return ret;
		}
		for (cmd = ptr + 2; cmd && *cmd == ' '; cmd++) {
			;
		}
		ptr = strchr (cmd, '&');
	}

	ptr = strstr (cmd, "?*");
	if (ptr && (ptr == cmd || ptr[-1] != '~')) {
		ptr[0] = 0;
		if (*cmd != '#') {
			int detail = 0;
			if (cmd < ptr && ptr[-1] == '?') {
				detail++;
				if (cmd < ptr - 1 && ptr[-2] == '?') {
					detail++;
				}
			}
			r_cons_break_push (NULL, NULL);
			recursive_help (core, detail, cmd);
			r_cons_break_pop ();
			r_cons_grep_parsecmd (ptr + 2, "`");
			if (scr_html != -1) {
				r_config_set_i (core->config, "scr.html", scr_html);
			}
			if (scr_color != -1) {
				r_config_set_i (core->config, "scr.color", scr_color);
			}
			r_list_free (tmpenvs);
			return 0;
		}
	}

	/* pipe console to file */
	ptr = (char *)r_str_firstbut (cmd, '>', "\"");
	// TODO honor `
	if (ptr) {
		if (ptr > cmd) {
			char *ch = ptr - 1;
			if (*ch == '\\') {
				memmove (ch, ptr, strlen (ptr) + 1);
				goto escape_redir;
			}
		}
		if (ptr[0] && ptr[1] == '?') {
			r_core_cmd_help (core, help_msg_greater_sign);
			r_list_free (tmpenvs);
			return true;
		}
		int fdn = 1;
		int pipecolor = r_config_get_i (core->config, "scr.color.pipe");
		int use_editor = false;
		int ocolor = r_config_get_i (core->config, "scr.color");
		*ptr = '\0';
		str = ptr + 1 + (ptr[1] == '>');
		r_str_trim (str);
		if (!*str) {
			eprintf ("No output?\n");
			goto next2;
		}
		/* r_cons_flush() handles interactive output (to the terminal)
		 * differently (e.g. asking about too long output). This conflicts
		 * with piping to a file. Disable it while piping. */
		if (ptr > (cmd + 1) && IS_WHITECHAR (ptr[-2])) {
			char *fdnum = ptr - 1;
			if (*fdnum == 'H') { // "H>"
				scr_html = r_config_get_i (core->config, "scr.html");
				r_config_set_i (core->config, "scr.html", true);
				pipecolor = true;
				*fdnum = 0;
			} else {
				if (IS_DIGIT (*fdnum)) {
					fdn = *fdnum - '0';
				}
				*fdnum = 0;
			}
		}
		r_cons_set_interactive (false);
		if (!strcmp (str, "-")) {
			use_editor = true;
			str = r_file_temp ("dumpedit");
			r_config_set_i (core->config, "scr.color", COLOR_MODE_DISABLED);
		}
		const bool appendResult = (ptr[1] == '>');
		if (*str == '$') {
			// pipe to alias variable
			// register output of command as an alias
			char *o = r_core_cmd_str (core, cmd);
			if (appendResult) {
				char *oldText = r_cmd_alias_get (core->rcmd, str, 1);
				if (oldText) {
					char *two = r_str_newf ("%s%s", oldText, o);
					if (two) {
						r_cmd_alias_set (core->rcmd, str, two, 1);
						free (two);
					}
				} else {
					char *n = r_str_newf ("$%s", o);
					r_cmd_alias_set (core->rcmd, str, n, 1);
					free (n);
				}
			} else {
				char *n = r_str_newf ("$%s", o);
				r_cmd_alias_set (core->rcmd, str, n, 1);
				free (n);
			}
			ret = 0;
			free (o);
		} else if (fdn > 0) {
			// pipe to file (or append)
			pipefd = r_cons_pipe_open (str, fdn, appendResult);
			if (pipefd != -1) {
				if (!pipecolor) {
					r_config_set_i (core->config, "scr.color", COLOR_MODE_DISABLED);
				}
				ret = r_core_cmd_subst (core, cmd);
				r_cons_flush ();
				r_cons_pipe_close (pipefd);
			}
		}
		r_cons_set_last_interactive ();
		if (!pipecolor) {
			r_config_set_i (core->config, "scr.color", ocolor);
		}
		if (use_editor) {
			const char *editor = r_config_get (core->config, "cfg.editor");
			if (editor && *editor) {
				r_sys_cmdf ("%s '%s'", editor, str);
				r_file_rm (str);
			} else {
				eprintf ("No cfg.editor configured\n");
			}
			r_config_set_i (core->config, "scr.color", ocolor);
			free (str);
		}
		if (scr_html != -1) {
			r_config_set_i (core->config, "scr.html", scr_html);
		}
		if (scr_color != -1) {
			r_config_set_i (core->config, "scr.color", scr_color);
		}
		core->cons->use_tts = false;
		r_list_free (tmpenvs);
		return ret;
	}
escape_redir:
next2:
	/* sub commands */
	ptr = strchr (cmd, '`');
	if (ptr) {
		if (ptr > cmd) {
			char *ch = ptr - 1;
			if (*ch == '\\') {
				memmove (ch, ptr, strlen (ptr) + 1);
				goto escape_backtick;
			}
		}
		bool empty = false;
		int oneline = 1;
		if (ptr[1] == '`') {
			memmove (ptr, ptr + 1, strlen (ptr));
			oneline = 0;
			empty = true;
		}
		ptr2 = strchr (ptr + 1, '`');
		if (empty) {
			/* do nothing */
		} else if (!ptr2) {
			eprintf ("parse: Missing backtick in expression.\n");
			goto fail;
		} else {
			int value = core->num->value;
			*ptr = '\0';
			*ptr2 = '\0';
			if (ptr[1] == '!') {
				str = r_core_cmd_str_pipe (core, ptr + 1);
			} else {
				// Color disabled when doing backticks ?e `pi 1`
				int ocolor = r_config_get_i (core->config, "scr.color");
				r_config_set_i (core->config, "scr.color", 0);
				core->cmd_in_backticks = true;
				str = r_core_cmd_str (core, ptr + 1);
				core->cmd_in_backticks = false;
				r_config_set_i (core->config, "scr.color", ocolor);
			}
			if (!str) {
				goto fail;
			}
			// ignore contents if first char is pipe or comment
			if (*str == '|' || *str == '*') {
				eprintf ("r_core_cmd_subst_i: invalid backticked command\n");
				free (str);
				goto fail;
			}
			if (oneline && str) {
				for (i = 0; str[i]; i++) {
					if (str[i] == '\n') {
						str[i] = ' ';
					}
				}
			}
			str = r_str_append (str, ptr2 + 1);
			cmd = r_str_append (strdup (cmd), str);
			core->num->value = value;
			ret = r_core_cmd_subst (core, cmd);
			free (cmd);
			if (scr_html != -1) {
				r_config_set_i (core->config, "scr.html", scr_html);
			}
			free (str);
			r_list_free (tmpenvs);
			return ret;
		}
	}
escape_backtick:
	// TODO must honor " and `
	if (*cmd != '"' && *cmd) {
		const char *s = strstr (cmd, "~?");
		if (s) {
			bool showHelp = false;
			if (cmd == s) {
				// ~?
				// ~??
				showHelp = true;
			} else {
				// pd~?
				// pd~??
				if (!strcmp (s, "~??")) {
					showHelp = true;
				}
			}
			if (showHelp) {
				r_cons_grep_help ();
				r_list_free (tmpenvs);
				return true;
			}
		}
	}
	if (*cmd != '.') {
		grep = r_cons_grep_strip (cmd, quotestr);
	}

	/* temporary seek commands */
	// if (*cmd != '(' && *cmd != '"') {
	if (*cmd != '"') {
		ptr = strchr (cmd, '@');
		if (ptr == cmd + 1 && *cmd == '?') {
			ptr = NULL;
		}
	} else {
		ptr = NULL;
	}

	cmd_tmpseek = core->tmpseek = ptr != NULL;
	int rc = 0;
	if (ptr) {
		char *f, *ptr2 = strchr (ptr + 1, '!');
		ut64 addr = core->offset;
		bool addr_is_set = false;
		char *tmpbits = NULL;
		const char *offstr = NULL;
		bool is_bits_set = false;
		bool is_arch_set = false;
		char *tmpeval = NULL;
		char *tmpasm = NULL;
		bool flgspc_changed = false;
		int tmpfd = -1;
		int sz, len;
		ut8 *buf;

		*ptr++ = '\0';
repeat_arroba:
		arroba = (ptr[0] && ptr[1] && ptr[2])?
				 strchr (ptr + 2, '@'): NULL;
		if (arroba) {
			*arroba = 0;
		}

		for (; *ptr == ' '; ptr++) {
			//nothing to see here
		}
		if (*ptr && ptr[1] == ':') {
			/* do nothing here */
		} else {
			ptr--;
		}

		r_str_trim_tail (ptr);

		if (ptr[1] == '?') {
			r_core_cmd_help (core, help_msg_at);
		} else if (ptr[1] == '%') { // "@%"
			char *k = strdup (ptr + 2);
			char *v = strchr (k, '=');
			if (v) {
				*v++ = 0;
				r_sys_setenv (k, v);
				r_list_append (tmpenvs, k);
			} else {
				free (k);
			}
		} else if (ptr[1] == '.') { // "@."
			if (ptr[2] == '.') { // "@.."
				if (ptr[3] == '.') { // "@..."
					ut64 addr = r_num_tail (core->num, core->offset, ptr + 4);
					r_core_block_size (core, R_ABS ((st64)addr - (st64)core->offset));
					goto fuji;
				} else {
					addr = r_num_tail (core->num, core->offset, ptr + 3);
					r_core_seek (core, addr, 1);
					cmd_tmpseek = core->tmpseek = true;
					goto fuji;
				}
			} else {
				// WAT DU
				eprintf ("TODO: what do you expect for @. import offset from file maybe?\n");
			}
		} else if (ptr[0] && ptr[1] == ':' && ptr[2]) {
			switch (ptr[0]) {
			case 'F': // "@F:" // temporary flag space
				flgspc_changed = r_flag_space_push (core->flags, ptr + 2);
				break;
			case 'B': // "@B:#" // seek to the last instruction in current bb
				{
					int index = (int)r_num_math (core->num, ptr + 2);
					RAnalBlock *bb = r_anal_bb_from_offset (core->anal, core->offset);
					if (bb) {
						// handle negative indices
						if (index < 0) {
							index = bb->ninstr + index;
						}

						if (index >= 0 && index < bb->ninstr) {
							ut16 inst_off = r_anal_bb_offset_inst (bb, index);
							r_core_seek (core, bb->addr + inst_off, 1);
							cmd_tmpseek = core->tmpseek = true;
						} else {
							eprintf ("The current basic block has %d instructions\n", bb->ninstr);
						}
					} else {
						eprintf ("Can't find a basic block for 0x%08"PFMT64x"\n", core->offset);
					}
					break;
				}
				break;
			case 'f': // "@f:" // slurp file in block
				f = r_file_slurp (ptr + 2, &sz);
				if (f) {
					{
						RBuffer *b = r_buf_new_with_bytes ((const ut8*)f, sz);
						RIODesc *d = r_io_open_buffer (core->io, b, R_PERM_RWX, 0);
						if (d) {
							if (tmpdesc) {
								r_io_desc_close (tmpdesc);
							}
							tmpdesc = d;
							if (pamode) {
								r_config_set_i (core->config, "io.va", 1);
							}
							r_io_map_new (core->io, d->fd, d->perm, 0, core->offset, r_buf_size (b));
						}
					}
				} else {
					eprintf ("cannot open '%s'\n", ptr + 3);
				}
				break;
			case 'r': // "@r:" // regname
				if (ptr[1] == ':') {
					ut64 regval;
					char *mander = strdup (ptr + 2);
					char *sep = findSeparator (mander);
					if (sep) {
						char ch = *sep;
						*sep = 0;
						regval = r_debug_reg_get (core->dbg, mander);
						*sep = ch;
						char *numexpr = r_str_newf ("0x%"PFMT64x"%s", regval, sep);
						regval = r_num_math (core->num, numexpr);
						free (numexpr);
					} else {
						regval = r_debug_reg_get (core->dbg, ptr + 2);
					}
					r_core_seek (core, regval, 1);
					cmd_tmpseek = core->tmpseek = true;
					free (mander);
				}
				break;
			case 'b': // "@b:" // bits
				is_bits_set = set_tmp_bits (core, r_num_math (core->num, ptr + 2), &tmpbits, &cmd_ignbithints);
				break;
			case 'i': // "@i:"
				{
					ut64 addr = r_num_math (core->num, ptr + 2);
					if (addr) {
						r_core_cmdf (core, "so %s", ptr + 2);
				//		r_core_seek (core, core->offset, 1);
						cmd_tmpseek = core->tmpseek = true;
					}
				}
				break;
			case 'e': // "@e:"
				{
					char *cmd = parse_tmp_evals (core, ptr + 2);
					if (!tmpeval) {
						tmpeval = cmd;
					} else {
						tmpeval = r_str_prepend (tmpeval, cmd);
						free (cmd);
					}
				}
				break;
			case 'x': // "@x:" // hexpairs
				if (ptr[1] == ':') {
					buf = malloc (strlen (ptr + 2) + 1);
					if (buf) {
						len = r_hex_str2bin (ptr + 2, buf);
						r_core_block_size (core, R_ABS (len));
						if (len > 0) {
							RBuffer *b = r_buf_new_with_bytes (buf, len);
							RIODesc *d = r_io_open_buffer (core->io, b, R_PERM_RWX, 0);
							if (d) {
								if (tmpdesc) {
									r_io_desc_close (tmpdesc);
								}
								tmpdesc = d;
								if (pamode) {
									r_config_set_i (core->config, "io.va", 1);
								}
								r_io_map_new (core->io, d->fd, d->perm, 0, core->offset, r_buf_size (b));
								r_core_block_size (core, len);
								r_core_block_read (core);
							}
						} else {
							eprintf ("Error: Invalid hexpairs for @x:\n");
						}
						free (buf);
					} else {
						eprintf ("cannot allocate\n");
					}
				} else {
					eprintf ("Invalid @x: syntax\n");
				}
				break;
			case 'k': // "@k"
				 {
					char *out = sdb_querys (core->sdb, NULL, 0, ptr + ((ptr[1])? 2: 1));
					if (out) {
						r_core_seek (core, r_num_math (core->num, out), 1);
						free (out);
						usemyblock = true;
					}
				 }
				break;
			case 'o': // "@o:3"
				if (ptr[1] == ':') {
					tmpfd = core->io->desc ? core->io->desc->fd : -1;
					r_io_use_fd (core->io, atoi (ptr + 2));
				}
				break;
			case 'a': // "@a:"
				if (ptr[1] == ':') {
					char *q = strchr (ptr + 2, ':');
					if (q) {
						*q++ = 0;
						int bits = r_num_math (core->num, q);
						is_bits_set = set_tmp_bits (core, bits, &tmpbits, &cmd_ignbithints);
					}
					is_arch_set = set_tmp_arch (core, ptr + 2, &tmpasm);
				} else {
					eprintf ("Usage: pd 10 @a:arm:32\n");
				}
				break;
			case 's': // "@s:" // wtf syntax
				{
					len = strlen (ptr + 2);
					r_core_block_size (core, len);
					const ut8 *buf = (const ut8*)r_str_trim_head_ro (ptr + 2);

					if (len > 0) {
						RBuffer *b = r_buf_new_with_bytes (buf, len);
						RIODesc *d = r_io_open_buffer (core->io, b, R_PERM_RWX, 0);
						if (!core->io->va) {
							r_config_set_i (core->config, "io.va", 1);
						}
						if (d) {
							if (tmpdesc) {
								r_io_desc_close (tmpdesc);
							}
							tmpdesc = d;
							if (pamode) {
								r_config_set_i (core->config, "io.va", 1);
							}
							r_io_map_new (core->io, d->fd, d->perm, 0, core->offset, r_buf_size (b));
							r_core_block_size (core, len);
							// r_core_block_read (core);
						}
					}
				}
break;
			default:
				goto ignore;
			}
			*ptr = '@';
			/* trim whitespaces before the @ */
			/* Fixes pd @x:9090 */
			char *trim = ptr - 2;
			while (trim > cmd) {
				if (!IS_WHITESPACE (*trim)) {
					break;
				}
				*trim = 0;
				trim--;
			}
			goto next_arroba;
		}
ignore:
		r_str_trim_head (ptr + 1);
		cmd = r_str_trim_nc (cmd);
		if (ptr2) {
			if (strlen (ptr + 1) == 13 && strlen (ptr2 + 1) == 6 &&
				!memcmp (ptr + 1, "0x", 2) &&
				!memcmp (ptr2 + 1, "0x", 2)) {
				/* 0xXXXX:0xYYYY */
			} else if (strlen (ptr + 1) == 9 && strlen (ptr2 + 1) == 4) {
				/* XXXX:YYYY */
			} else {
				*ptr2 = '\0';
				if (!ptr2[1]) {
					goto fail;
				}
				r_core_block_size (
					core, r_num_math (core->num, ptr2 + 1));
			}
		}

		r_str_trim_head (ptr + 1);
		offstr = ptr + 1;

		addr = r_num_math (core->num, offstr);
		addr_is_set = true;

		if (isalpha ((ut8)ptr[1]) && !addr) {
			if (!r_flag_get (core->flags, ptr + 1)) {
				eprintf ("Invalid address (%s)\n", ptr + 1);
				goto fail;
			}
		} else {
			char ch = *offstr;
			if (ch == '-' || ch == '+') {
				addr = core->offset + addr;
			}
		}
		// remap thhe tmpdesc if any
		if (addr) {
			RIODesc *d = tmpdesc;
			if (d) {
				r_io_map_new (core->io, d->fd, d->perm, 0, addr, r_io_desc_size (d));
			}
		}
next_arroba:
		if (arroba) {
			ptr = arroba + 1;
			*arroba = '@';
			arroba = NULL;
			goto repeat_arroba;
		}
		core->fixedblock = !!tmpdesc;
		if (core->fixedblock) {
			r_core_block_read (core);
		}
		if (ptr[1] == '@') {
			if (ptr[2] == '@') {
				char *rule = ptr + 3;
				while (*rule && *rule == ' ') {
					rule++;
				}
				ret = r_core_cmd_foreach3 (core, cmd, rule);
			} else {
				ret = r_core_cmd_foreach (core, cmd, ptr + 2);
			}
		} else {
			bool tmpseek = false;
			const char *fromvars[] = { "anal.from", "diff.from", "graph.from",
				"io.buffer.from", "lines.from", "search.from", "zoom.from", NULL };
			const char *tovars[] = { "anal.to", "diff.to", "graph.to",
				"io.buffer.to", "lines.to", "search.to", "zoom.to", NULL };
			ut64 curfrom[R_ARRAY_SIZE (fromvars) - 1], curto[R_ARRAY_SIZE (tovars) - 1];

			// "@(A B)"
			if (ptr[1] == '(') {
				char *range = ptr + 3;
				char *p = strchr (range, ' ');
				if (!p) {
					eprintf ("Usage: / ABCD @..0x1000 0x3000\n");
					free (tmpeval);
					free (tmpasm);
					free (tmpbits);
					goto fail;
				}
				*p = '\x00';
				ut64 from = r_num_math (core->num, range);
				ut64 to = r_num_math (core->num, p + 1);
				// save current ranges
				for (i = 0; fromvars[i]; i++) {
					curfrom[i] = r_config_get_i (core->config, fromvars[i]);
				}
				for (i = 0; tovars[i]; i++) {
					curto[i] = r_config_get_i (core->config, tovars[i]);
				}
				// set new ranges
				for (i = 0; fromvars[i]; i++) {
					r_config_set_i (core->config, fromvars[i], from);
				}
				for (i = 0; tovars[i]; i++) {
					r_config_set_i (core->config, tovars[i], to);
				}
				tmpseek = true;
			}
			if (usemyblock) {
				if (addr_is_set) {
					core->offset = addr;
				}
				ret = r_cmd_call (core->rcmd, r_str_trim_head_ro (cmd));
			} else {
				if (addr_is_set) {
					if (ptr[1]) {
						r_core_seek (core, addr, 1);
						r_core_block_read (core);
					}
				}
				ret = r_cmd_call (core->rcmd, r_str_trim_head_ro (cmd));

			}
			if (tmpseek) {
				// restore ranges
				for (i = 0; fromvars[i]; i++) {
					r_config_set_i (core->config, fromvars[i], curfrom[i]);
				}
				for (i = 0; tovars[i]; i++) {
					r_config_set_i (core->config, tovars[i], curto[i]);
				}
			}
		}
		if (ptr2) {
			*ptr2 = '!';
			r_core_block_size (core, tmpbsz);
		}
		if (is_arch_set) {
			core->fixedarch = oldfixedarch;
			r_config_set (core->config, "asm.arch", tmpasm);
			R_FREE (tmpasm);
		}
		if (tmpfd != -1) {
			// TODO: reuse tmpfd instead of
			r_io_use_fd (core->io, tmpfd);
		}
		if (tmpdesc) {
			if (pamode) {
				r_config_set_i (core->config, "io.va", 0);
			}
			r_io_desc_close (tmpdesc);
			tmpdesc = NULL;
		}
		if (is_bits_set) {
			r_config_set (core->config, "asm.bits", tmpbits);
			core->fixedbits = oldfixedbits;
		}
		if (tmpbsz != core->blocksize) {
			r_core_block_size (core, tmpbsz);
		}
		if (tmpeval) {
			r_core_cmd0 (core, tmpeval);
			R_FREE (tmpeval);
		}
		if (flgspc_changed) {
			r_flag_space_pop (core->flags);
		}
		*ptr = '@';
		rc = ret;
		goto beach;
	}
fuji:
	if (cmd) {
		r_str_trim_head (cmd);
		rc = r_cmd_call (core->rcmd, cmd);
	} else {
		rc = false;
	}
beach:
	r_cons_grep_process (grep);
	if (scr_html != -1) {
		r_cons_flush ();
		r_config_set_i (core->config, "scr.html", scr_html);
	}
	if (scr_color != -1) {
		r_config_set_i (core->config, "scr.color", scr_color);
	}
	r_list_free (tmpenvs);
	if (tmpdesc) {
		r_io_desc_close (tmpdesc);
		tmpdesc = NULL;
	}
	core->fixedarch = oldfixedarch;
	core->fixedbits = oldfixedbits;
	if (tmpseek) {
		*tmpseek = cmd_tmpseek;
	}
	if (cmd_ignbithints != -1) {
		r_config_set_i (core->config, "anal.ignbithints", cmd_ignbithints);
	}
	return rc;
fail:
	rc = -1;
	goto beach;
}

static int foreach_comment(void *user, const char *k, const char *v) {
	RAnalMetaUserItem *ui = user;
	RCore *core = ui->anal->user;
	const char *cmd = ui->user;
	if (!strncmp (k, "meta.C.", 7)) {
		char *cmt = (char *)sdb_decode (v, 0);
		if (cmt) {
			r_core_cmdf (core, "s %s", k + 7);
			r_core_cmd0 (core, cmd);
			free (cmt);
		}
	}
	return 1;
}

struct exec_command_t {
	RCore *core;
	const char *cmd;
};

static bool exec_command_on_flag(RFlagItem *flg, void *u) {
	struct exec_command_t *user = (struct exec_command_t *)u;
	r_core_block_size (user->core, flg->size);
	r_core_seek (user->core, flg->offset, 1);
	r_core_cmd0 (user->core, user->cmd);
	return true;
}

static void foreach_pairs(RCore *core, const char *cmd, const char *each) {
	const char *arg;
	int pair = 0;
	for (arg = each ; ; ) {
		char *next = strchr (arg, ' ');
		if (next) {
			*next = 0;
		}
		if (arg && *arg) {
			ut64 n = r_num_get (NULL, arg);
			if (pair%2) {
				r_core_block_size (core, n);
				r_core_cmd0 (core, cmd);
			} else {
				r_core_seek (core, n, 1);
			}
			pair++;
		}
		if (!next) {
			break;
		}
		arg = next + 1;
	}
}

R_API int r_core_cmd_foreach3(RCore *core, const char *cmd, char *each) { // "@@@"
	RDebug *dbg = core->dbg;
	RList *list, *head;
	RListIter *iter;
	int i;
	const char *filter = NULL;

	if (each[1] == ':') {
		filter = each + 2;
	}

	switch (each[0]) {
	case '=':
		foreach_pairs (core, cmd, each + 1);
		break;
	case '?':
		r_core_cmd_help (core, help_msg_at_at_at);
		break;
	case 'c':
		if (filter) {
			char *arg = r_core_cmd_str (core, filter);
			foreach_pairs (core, cmd, arg);
			free (arg);
		} else {
			eprintf ("Usage: @@@c:command   # same as @@@=`command`\n");
		}
		break;
	case 'C':
		r_meta_list_cb (core->anal, R_META_TYPE_COMMENT, 0, foreach_comment, (void*)cmd, UT64_MAX);
		break;
	case 'm':
		{
			int fd = r_io_fd_get_current (core->io);
			// only iterate maps of current fd
			RList *maps = r_io_map_get_for_fd (core->io, fd);
			RIOMap *map;
			if (maps) {
				RListIter *iter;
				r_list_foreach (maps, iter, map) {
					r_core_seek (core, map->itv.addr, 1);
					r_core_block_size (core, map->itv.size);
					r_core_cmd0 (core, cmd);
				}
				r_list_free (maps);
			}
		}
		break;
	case 'M':
		if (dbg && dbg->h && dbg->maps) {
			RDebugMap *map;
			r_list_foreach (dbg->maps, iter, map) {
				r_core_seek (core, map->addr, 1);
				//r_core_block_size (core, map->size);
				r_core_cmd0 (core, cmd);
			}
		}
		break;
	case 't':
		// iterate over all threads
		if (dbg && dbg->h && dbg->h->threads) {
			int origpid = dbg->pid;
			RDebugPid *p;
			list = dbg->h->threads (dbg, dbg->pid);
			if (!list) {
				return false;
			}
			r_list_foreach (list, iter, p) {
				r_core_cmdf (core, "dp %d", p->pid);
				r_cons_printf ("PID %d\n", p->pid);
				r_core_cmd0 (core, cmd);
			}
			r_core_cmdf (core, "dp %d", origpid);
			r_list_free (list);
		}
		break;
	case 'r':
		// registers
		{
			ut64 offorig = core->offset;
			for (i = 0; i < 128; i++) {
				RRegItem *item;
				ut64 value;
				head = r_reg_get_list (dbg->reg, i);
				if (!head) {
					continue;
				}
				r_list_foreach (head, iter, item) {
					if (item->size != core->anal->bits) {
						continue;
					}
					value = r_reg_get_value (dbg->reg, item);
					r_core_seek (core, value, 1);
					r_cons_printf ("%s: ", item->name);
					r_core_cmd0 (core, cmd);
				}
			}
			r_core_seek (core, offorig, 1);
		}
		break;
	case 'i': // @@@i
		// imports
		{
			RBinImport *imp;
			ut64 offorig = core->offset;
			list = r_bin_get_imports (core->bin);
			r_list_foreach (list, iter, imp) {
				char *impflag = r_str_newf ("sym.imp.%s", imp->name);
				ut64 addr = r_num_math (core->num, impflag);
				free (impflag);
				if (addr && addr != UT64_MAX) {
					r_core_seek (core, addr, 1);
					r_core_cmd0 (core, cmd);
				}
			}
			r_core_seek (core, offorig, 1);
		}
		break;
	case 'S': // "@@@S"
		{
			RBinObject *obj = r_bin_cur_object (core->bin);
			if (obj) {
				ut64 offorig = core->offset;
				ut64 bszorig = core->blocksize;
				RBinSection *sec;
				RListIter *iter;
				r_list_foreach (obj->sections, iter, sec) {
					r_core_seek (core, sec->vaddr, 1);
					r_core_block_size (core, sec->vsize);
					r_core_cmd0 (core, cmd);
				}
				r_core_block_size (core, bszorig);
				r_core_seek (core, offorig, 1);
			}
		}
#if ATTIC
		if (each[1] == 'S') {
			RListIter *it;
			RBinSection *sec;
			RBinObject *obj = r_bin_cur_object (core->bin);
			int cbsz = core->blocksize;
			r_list_foreach (obj->sections, it, sec){
				ut64 addr = sec->vaddr;
				ut64 size = sec->vsize;
				// TODO:
				//if (R_BIN_SCN_EXECUTABLE & sec->perm) {
				//	continue;
				//}
				r_core_seek_size (core, addr, size);
				r_core_cmd (core, cmd, 0);
			}
			r_core_block_size (core, cbsz);
		}
#endif
		break;
	case 's':
		if (each[1] == 't') { // strings
			list = r_bin_get_strings (core->bin);
			RBinString *s;
			if (list) {
				ut64 offorig = core->offset;
				ut64 obs = core->blocksize;
				r_list_foreach (list, iter, s) {
					r_core_block_size (core, s->size);
					r_core_seek (core, s->vaddr, 1);
					r_core_cmd0 (core, cmd);
				}
				r_core_block_size (core, obs);
				r_core_seek (core, offorig, 1);
			}
		} else {
			// symbols
			RBinSymbol *sym;
			ut64 offorig = core->offset;
			ut64 obs = core->blocksize;
			list = r_bin_get_symbols (core->bin);
			r_cons_break_push (NULL, NULL);
			r_list_foreach (list, iter, sym) {
				if (r_cons_is_breaked ()) {
					break;
				}
				r_core_block_size (core, sym->size);
				r_core_seek (core, sym->vaddr, 1);
				r_core_cmd0 (core, cmd);
			}
			r_cons_break_pop ();
			r_core_block_size (core, obs);
			r_core_seek (core, offorig, 1);
		}
		break;
	case 'f': // flags
		{
		// TODO: honor ^C
			char *glob = filter? r_str_trim_dup (filter): NULL;
			ut64 off = core->offset;
			ut64 obs = core->blocksize;
			struct exec_command_t u = { .core = core, .cmd = cmd };
			r_flag_foreach_glob (core->flags, glob, exec_command_on_flag, &u);
			r_core_seek (core, off, 0);
			r_core_block_size (core, obs);
			free (glob);
		}
		break;
	case 'F': // functions
		{
			ut64 obs = core->blocksize;
			ut64 offorig = core->offset;
			RAnalFunction *fcn;
			list = core->anal->fcns;
			r_cons_break_push (NULL, NULL);
			r_list_foreach (list, iter, fcn) {
				if (r_cons_is_breaked ()) {
					break;
				}
				if (!filter || r_str_glob (fcn->name, filter)) {
					r_core_seek (core, fcn->addr, 1);
					r_core_block_size (core, r_anal_function_linear_size (fcn));
					r_core_cmd0 (core, cmd);
				}
			}
			r_cons_break_pop ();
			r_core_block_size (core, obs);
			r_core_seek (core, offorig, 1);
		}
		break;
	case 'b':
		{
			RAnalFunction *fcn = r_anal_get_fcn_in (core->anal, core->offset, 0);
			ut64 offorig = core->offset;
			ut64 obs = core->blocksize;
			if (fcn) {
				RListIter *iter;
				RAnalBlock *bb;
				r_list_foreach (fcn->bbs, iter, bb) {
					r_core_seek (core, bb->addr, 1);
					r_core_block_size (core, bb->size);
					r_core_cmd0 (core, cmd);
				}
				r_core_block_size (core, obs);
				r_core_seek (core, offorig, 1);
			}
		}
		break;
	}
	return 0;
}

static void foreachOffset(RCore *core, const char *_cmd, const char *each) {
	char *cmd = strdup (_cmd);
	char *nextLine = NULL;
	ut64 addr;
	/* foreach list of items */
	while (each) {
		// skip spaces
		while (*each == ' ') {
			each++;
		}
		// stahp if empty string
		if (!*each) {
			break;
		}
		// find newline
		char *nl = strchr (each, '\n');
		if (nl) {
			*nl = 0;
			nextLine = nl + 1;
		} else {
			nextLine = NULL;
		}
		// chop comment in line
		nl = strchr (each, '#');
		if (nl) {
			*nl = 0;
		}
		// space separated numbers
		while (each && *each) {
			// find spaces
			while (*each == ' ') {
				each++;
			}
			char *str = strchr (each, ' ');
			if (str) {
				*str = '\0';
				addr = r_num_math (core->num, each);
				*str = ' ';
				each = str + 1;
			} else {
				if (!*each) {
					break;
				}
				addr = r_num_math (core->num, each);
				each = NULL;
			}
			r_core_seek (core, addr, 1);
			r_core_cmd (core, cmd, 0);
			r_cons_flush ();
		}
		each = nextLine;
	}
	free (cmd);
}

R_API int r_core_cmd_foreach(RCore *core, const char *cmd, char *each) {
	int i, j;
	char ch;
	char *word = NULL;
	char *str, *ostr = NULL;
	RListIter *iter;
	RFlagItem *flag;
	ut64 oseek, addr;

	for (; *cmd == ' '; cmd++) {
		;
	}

	oseek = core->offset;
	ostr = str = strdup (each);
	r_cons_break_push (NULL, NULL); //pop on return
	switch (each[0]) {
	case '/': // "@@/"
		{
		char *cmdhit = strdup (r_config_get (core->config, "cmd.hit"));
		r_config_set (core->config, "cmd.hit", cmd);
		r_core_cmd0 (core, each);
		r_config_set (core->config, "cmd.hit", cmdhit);
		free (cmdhit);
		}
		free (ostr);
		return 0;
	case '?': // "@@?"
		r_core_cmd_help (core, help_msg_at_at);
		break;
	case 'b': // "@@b" - function basic blocks
		{
			RListIter *iter;
			RAnalBlock *bb;
			RAnalFunction *fcn = r_anal_get_function_at (core->anal, core->offset);
			int bs = core->blocksize;
			if (fcn) {
				r_list_sort (fcn->bbs, bb_cmp);
				r_list_foreach (fcn->bbs, iter, bb) {
					r_core_block_size (core, bb->size);
					r_core_seek (core, bb->addr, 1);
					r_core_cmd (core, cmd, 0);
					if (r_cons_is_breaked ()) {
						break;
					}
				}
			}
			r_core_block_size (core, bs);
			goto out_finish;
		}
		break;
	case 's': // "@@s" - sequence
		{
			char *str = each + 1;
			if (*str == ':' || *str == ' ') {
				str++;
			}
			int count = r_str_split (str, ' ');
			if (count == 3) {
				ut64 cur;
				ut64 from = r_num_math (core->num, r_str_word_get0 (str, 0));
				ut64 to = r_num_math (core->num, r_str_word_get0 (str, 1));
				ut64 step = r_num_math (core->num, r_str_word_get0 (str, 2));
				for (cur = from; cur < to; cur += step) {
					(void)r_core_seek (core, cur, 1);
					r_core_cmd (core, cmd, 0);
					if (r_cons_is_breaked ()) {
						break;
					}
				}
			} else {
				eprintf ("Usage: cmd @@s:from to step\n");
			}
			goto out_finish;
		}
		break;
	case 'i': // "@@i" - function instructions
		{
			RListIter *iter;
			RAnalBlock *bb;
			int i;
			RAnalFunction *fcn = r_anal_get_function_at (core->anal, core->offset);
			if (fcn) {
				r_list_sort (fcn->bbs, bb_cmp);
				r_list_foreach (fcn->bbs, iter, bb) {
					for (i = 0; i < bb->op_pos_size; i++) {
						ut64 addr = bb->addr + bb->op_pos[i];
						r_core_seek (core, addr, 1);
						r_core_cmd (core, cmd, 0);
						if (r_cons_is_breaked ()) {
							break;
						}
					}
				}
			}
			goto out_finish;
		}
		break;
	case 'f': // "@@f"
		if (each[1] == ':') {
			RAnalFunction *fcn;
			RListIter *iter;
			if (core->anal) {
				r_list_foreach (core->anal->fcns, iter, fcn) {
					if (each[2] && strstr (fcn->name, each + 2)) {
						r_core_seek (core, fcn->addr, 1);
						r_core_cmd (core, cmd, 0);
						if (r_cons_is_breaked ()) {
							break;
						}
					}
				}
			}
			goto out_finish;
		} else {
			RAnalFunction *fcn;
			RListIter *iter;
			if (core->anal) {
				RConsGrep grep = core->cons->context->grep;
				r_list_foreach (core->anal->fcns, iter, fcn) {
					char *buf;
					r_core_seek (core, fcn->addr, 1);
					r_cons_push ();
					r_core_cmd (core, cmd, 0);
					buf = (char *)r_cons_get_buffer ();
					if (buf) {
						buf = strdup (buf);
					}
					r_cons_pop ();
					r_cons_strcat (buf);
					free (buf);
					if (r_cons_is_breaked ()) {
						break;
					}
				}
				core->cons->context->grep = grep;
			}
			goto out_finish;
		}
		break;
	case 't': // "@@t"
		{
			RDebugPid *p;
			int pid = core->dbg->pid;
			if (core->dbg->h && core->dbg->h->pids) {
				RList *list = core->dbg->h->pids (core->dbg, R_MAX (0, pid));
				r_list_foreach (list, iter, p) {
					r_cons_printf ("# PID %d\n", p->pid);
					r_debug_select (core->dbg, p->pid, p->pid);
					r_core_cmd (core, cmd, 0);
					r_cons_newline ();
				}
				r_list_free (list);
			}
			r_debug_select (core->dbg, pid, pid);
			goto out_finish;
		}
		break;
	case 'c': // "@@c:"
		if (each[1] == ':') {
			char *arg = r_core_cmd_str (core, each + 2);
			if (arg) {
				foreachOffset (core, cmd, arg);
				free (arg);
			}
		}
		break;
	case '=': // "@@="
		foreachOffset (core, cmd, str + 1);
		break;
	case 'd': // "@@d"
		if (each[1] == 'b' && each[2] == 't') {
			ut64 oseek = core->offset;
			RDebugFrame *frame;
			RListIter *iter;
			RList *list;
			list = r_debug_frames (core->dbg, UT64_MAX);
			i = 0;
			r_list_foreach (list, iter, frame) {
				switch (each[3]) {
				case 'b':
					r_core_seek (core, frame->bp, 1);
					break;
				case 's':
					r_core_seek (core, frame->sp, 1);
					break;
				default:
				case 'a':
					r_core_seek (core, frame->addr, 1);
					break;
				}
				r_core_cmd (core, cmd, 0);
				r_cons_newline ();
				i++;
			}
			r_core_seek (core, oseek, 0);
			r_list_free (list);
		} else {
			eprintf("Invalid for-each statement. Use @@=dbt[abs]\n");
		}
		break;
	case 'k': // "@@k"
		/* foreach list of items */
		{
		char *out = sdb_querys (core->sdb, NULL, 0, str + ((str[1])? 2: 1));
		if (out) {
			each = out;
			do {
				while (*each == ' ') {
					each++;
				}
				if (!*each) {
					break;
				}
				str = strchr (each, ' ');
				if (str) {
					*str = '\0';
					addr = r_num_math (core->num, each);
					*str = ' ';
				} else {
					addr = r_num_math (core->num, each);
				}
				//eprintf ("; 0x%08"PFMT64x":\n", addr);
				each = str + 1;
				r_core_seek (core, addr, 1);
				r_core_cmd (core, cmd, 0);
				r_cons_flush ();
			} while (str != NULL);
			free (out);
		}
		}
		break;
	case '.': // "@@."
		if (each[1] == '(') {
			char cmd2[1024];
			// XXX what's this 999 ?
			i = 0;
			for (core->rcmd->macro.counter = 0; i < 999; core->rcmd->macro.counter++) {
				if (r_cons_is_breaked ()) {
					break;
				}
				r_cmd_macro_call (&core->rcmd->macro, each + 2);
				if (!core->rcmd->macro.brk_value) {
					break;
				}
				addr = core->rcmd->macro._brk_value;
				sprintf (cmd2, "%s @ 0x%08"PFMT64x"", cmd, addr);
				eprintf ("0x%08"PFMT64x" (%s)\n", addr, cmd2);
				r_core_seek (core, addr, 1);
				r_core_cmd (core, cmd2, 0);
				i++;
			}
		} else {
			char buf[1024];
			char cmd2[1024];
			FILE *fd = r_sandbox_fopen (each + 1, "r");
			if (fd) {
				core->rcmd->macro.counter = 0;
				while (!feof (fd)) {
					buf[0] = '\0';
					if (!fgets (buf, sizeof (buf), fd)) {
						break;
					}
					addr = r_num_math (core->num, buf);
					eprintf ("0x%08"PFMT64x": %s\n", addr, cmd);
					sprintf (cmd2, "%s @ 0x%08"PFMT64x"", cmd, addr);
					r_core_seek (core, addr, 1); // XXX
					r_core_cmd (core, cmd2, 0);
					core->rcmd->macro.counter++;
				}
				fclose (fd);
			} else {
				eprintf ("cannot open file '%s' to read offsets\n", each + 1);
			}
		}
		break;
	default:
		core->rcmd->macro.counter = 0;
		for (; *each == ' '; each++) {
			;
		}
		i = 0;
		while (str[i]) {
			j = i;
			for (; str[j] && str[j] == ' '; j++) {
				; // skip spaces
			}
			for (i = j; str[i] && str[i] != ' '; i++) {
				; // find EOS
			}
			ch = str[i];
			str[i] = '\0';
			word = strdup (str + j);
			if (!word) {
				break;
			}
			str[i] = ch;
			{
				const RSpace *flagspace = r_flag_space_cur (core->flags);
				RList *match_flag_items = r_list_newf ((RListFree)r_flag_item_free);
				if (!match_flag_items) {
					break;
				}

				/* duplicate flags that match word, to be sure
				   the command is going to be executed on flags
				   values at the moment the command is called
				   (without side effects) */
				struct duplicate_flag_t u = {
					.ret = match_flag_items,
					.word = word,
				};
				r_flag_foreach_space (core->flags, flagspace, duplicate_flag, &u);

				/* for all flags that match */
				r_list_foreach (match_flag_items, iter, flag) {
					if (r_cons_is_breaked ()) {
						break;
					}

					char *buf = NULL;
					const char *tmp = NULL;
					r_core_seek (core, flag->offset, 1);
					r_cons_push ();
					r_core_cmd (core, cmd, 0);
					tmp = r_cons_get_buffer ();
					buf = tmp? strdup (tmp): NULL;
					r_cons_pop ();
					r_cons_strcat (buf);
					free (buf);
					r_core_task_yield (&core->tasks);
				}

				r_list_free (match_flag_items);
				core->rcmd->macro.counter++ ;
				R_FREE (word);
			}
		}
	}
	r_cons_break_pop ();
	// XXX: use r_core_seek here
	core->offset = oseek;

	free (word);
	free (ostr);
	return true;
out_finish:
	free (ostr);
	r_cons_break_pop ();
	return false;
}

R_API void run_pending_anal(RCore *core) {
	if (core && core->ev) {
		// allow incall events in the run_pending step
		core->ev->incall = false;
	}
	if (core && core->anal && core->anal->cmdtail) {
		char *res = r_strbuf_drain (core->anal->cmdtail);
		core->anal->cmdtail = r_strbuf_new (NULL);
		r_core_cmd_lines (core, res);
		free (res);
	}
}

static int run_cmd_depth(RCore *core, char *cmd);

#if USE_TREESITTER

struct tsr2cmd_state {
	TSParser *parser;
	RCore *core;
	char *input;
	char *saved_input;
	TSTree *tree;
	TSTree *saved_tree;
	bool log;
	bool split_lines;
	bool is_last_cmd;
	TSNode substitute_cmd;
};

struct tsr2cmd_edit {
	char *new_text;
	char *old_text;
	ut32 start;
	ut32 end;
	TSPoint start_point;
	TSPoint end_point;
};

struct parsed_args {
	char *argv_str;
	int argc;
	char **argv;
};

typedef bool (*ts_handler)(struct tsr2cmd_state *state, TSNode node);

struct ts_data_symbol_map {
	const char *name;
	void *data;
};

#define TS_START_END(node, start, end) do {		\
		start = ts_node_start_byte (node);	\
		end = ts_node_end_byte (node);		\
	} while (0)

static char *ts_node_sub_string(TSNode node, const char *cstr) {
	ut32 start, end;
	TS_START_END (node, start, end);
	return r_str_newf ("%.*s", end - start, cstr + start);
}

static char *ts_node_sub_parent_string(TSNode parent, TSNode node, const char *cstr) {
	ut32 start, end;
	TS_START_END (node, start, end);
	ut32 parent_start = ts_node_start_byte (parent);
	start -= parent_start;
	end -= parent_start;
	return r_str_newf ("%.*s", end - start, cstr + start);
}

#define DEFINE_IS_TS_FCN(name)                                      \
	TSSymbol ts_##name##_symbol;                                \
	static inline bool is_ts_##name(TSNode node) {              \
		return ts_node_symbol (node) == ts_##name##_symbol; \
	}

#define DEFINE_HANDLE_TS_FCN(name) \
	DEFINE_IS_TS_FCN (name) \
	static bool handle_ts_##name##_internal(struct tsr2cmd_state *state, TSNode node, char *node_string); \
	static bool handle_ts_##name(struct tsr2cmd_state *state, TSNode node) { \
		char *node_string = ts_node_sub_string (node, state->input);	\
		R_LOG_DEBUG (#name ": '%s'\n", node_string);		\
		bool res = handle_ts_##name##_internal (state, node, node_string); \
		free (node_string);					\
		return res;						\
	} \
	static bool handle_ts_##name##_internal(struct tsr2cmd_state *state, TSNode node, char *node_string)

static bool handle_ts_command(struct tsr2cmd_state *state, TSNode node);
static bool core_cmd_tsr2cmd(RCore *core, const char *cstr, bool split_lines, bool log);

DEFINE_IS_TS_FCN(fdn_redirect_operator)
DEFINE_IS_TS_FCN(fdn_append_operator)
DEFINE_IS_TS_FCN(html_redirect_operator)
DEFINE_IS_TS_FCN(html_append_operator)
DEFINE_IS_TS_FCN(cmd_substitution_arg)
DEFINE_IS_TS_FCN(args)
DEFINE_IS_TS_FCN(arg)
DEFINE_IS_TS_FCN(arg_identifier)
DEFINE_IS_TS_FCN(double_quoted_arg)
DEFINE_IS_TS_FCN(single_quoted_arg)
DEFINE_IS_TS_FCN(concatenation)

// NOTE: this should be in sync with SPECIAL_CHARACTERS in
//       radare2-shell-parser grammar, except for ", ' and
//       whitespaces, because we let cmd_substitution_arg create
//       new arguments
static const char *SPECIAL_CHARS_REGULAR = "@;~$#|`\"'()<>";
static const char *SPECIAL_CHARS_DOUBLE_QUOTED = "\"";
static const char *SPECIAL_CHARS_SINGLE_QUOTED = "'";

static struct tsr2cmd_edit *create_cmd_edit(struct tsr2cmd_state *state, TSNode arg, char *new_text) {
	struct tsr2cmd_edit *e = R_NEW0 (struct tsr2cmd_edit);
	ut32 command_start = ts_node_start_byte (state->substitute_cmd);
	TSPoint command_point = ts_node_start_point (state->substitute_cmd);
	e->new_text = new_text;
	e->old_text = ts_node_sub_parent_string (state->substitute_cmd, arg, state->input);
	e->start = ts_node_start_byte (arg) - command_start;
	e->end = ts_node_end_byte (arg) - command_start;
	e->start_point = ts_node_start_point (arg);
	e->end_point = ts_node_end_point (arg);
	if (e->start_point.row == command_point.row) {
		e->start_point.column -= command_point.column;
	}
	if (e->end_point.row == command_point.row) {
		e->end_point.column -= command_point.column;
	}
	e->start_point.row -= command_point.row;
	e->end_point.row -= command_point.row;
	return e;
}

static void replace_whitespaces(char *s, char ch) {
	while (*s) {
		if (isspace (*s)) {
			*s = ch;
		}
		s++;
	}
}

static char *escape_special_chars(char *s, const char *special_chars) {
	size_t s_len = strlen (s);
	char *d = R_NEWS (char, s_len * 2 + 1);
	int i, j = 0;
	for (i = 0; i < s_len; i++) {
		if (strchr (special_chars, s[i])) {
			d[j++] = '\\';
		}
		d[j++] = s[i];
	}
	d[j++] = '\0';
	free (s);
	return d;
}

static char *unescape_special_chars(char *s, const char *special_chars) {
	char *dst = R_NEWS (char, strlen (s) + 1);
	int i, j = 0;

	for (i = 0; s[i]; i++) {
		if (s[i] != '\\' || !strchr (special_chars, s[i + 1])) {
			dst[j++] = s[i];
			continue;
		}
		dst[j++] = s[i + 1];
		i++;
	}
	dst[j++] = '\0';
	return dst;
}

void free_tsr2cmd_edit(struct tsr2cmd_edit *edit) {
	free (edit->new_text);
	free (edit->old_text);
	free (edit);
}

static void parsed_args_free(struct parsed_args *a) {
	if (!a) {
		return;
	}

	int i;
	for (i = 0; i < a->argc; i++) {
		free (a->argv[i]);
	}
	free (a->argv);
	free (a->argv_str);
	free (a);
}

static char *do_handle_substitution_cmd(struct tsr2cmd_state *state, TSNode inn_cmd) {
	RCore *core = state->core;
	char *inn_str = ts_node_sub_parent_string (state->substitute_cmd, inn_cmd, state->input);

	// save current color and disable it
	int ocolor = r_config_get_i (core->config, "scr.color");
	r_config_set_i (core->config, "scr.color", 0);
	core->cmd_in_backticks = true;
	int value = core->num->value;

	// execute the sub command
	char *o_out = r_core_cmd_str (core, inn_str);

	// restore color and cmd_in_backticks
	core->num->value = value;
	core->cmd_in_backticks = false;
	r_config_set_i (core->config, "scr.color", ocolor);
	free (inn_str);

	// replace the output of the sub command with the current argument
	char *out = strdup (o_out);
	r_str_trim (out);
	R_LOG_DEBUG ("output of inner command: '%s'\n", out);
	free (o_out);

	// replace newlines and similar with spaces
	replace_whitespaces (out, ' ');
	return out;
}

static void handle_cmd_substitution_arg(struct tsr2cmd_state *state, TSNode arg, RList *edits) {
	TSNode inn_cmd = ts_node_child (arg, 1);
	r_return_if_fail (!ts_node_is_null (inn_cmd));
	char *out = do_handle_substitution_cmd (state, inn_cmd);
	// escape special chars to prevent creation of new tokens when parsing again
	const char *special_chars;
	if (is_ts_double_quoted_arg (ts_node_parent (arg))) {
		special_chars = SPECIAL_CHARS_DOUBLE_QUOTED;
	} else {
		special_chars = SPECIAL_CHARS_REGULAR;
	}
	out = escape_special_chars (out, special_chars);
	struct tsr2cmd_edit *e = create_cmd_edit (state, arg, out);
	r_list_append (edits, e);
}

static void handle_substitution_args(struct tsr2cmd_state *state, TSNode args, RList *edits) {
	if (is_ts_args (args) || is_ts_concatenation (args) || is_ts_double_quoted_arg (args)) {
		uint32_t n_children = ts_node_named_child_count (args);
		uint32_t i;
		for (i = 0; i < n_children; i++) {
			TSNode arg = ts_node_named_child (args, i);
			handle_substitution_args (state, arg, edits);
		}
	} else if (is_ts_cmd_substitution_arg (args)) {
		handle_cmd_substitution_arg (state, args, edits);
	} else if (is_ts_arg (args)) {
		TSNode arg = ts_node_named_child (args, 0);
		handle_substitution_args (state, arg, edits);
	}
}

static char *unescape_arg(struct tsr2cmd_state *state, TSNode arg, const char *special_chars) {
	char *arg_str = ts_node_sub_string (arg, state->input);
	char *unescaped_arg = unescape_special_chars (arg_str, special_chars);
	R_LOG_DEBUG ("original arg = '%s', unescaped arg = '%s'\n", arg_str, unescaped_arg);
	free (arg_str);
	return unescaped_arg;
}

static char *do_handle_ts_unescape_arg(struct tsr2cmd_state *state, TSNode arg) {
	if (is_ts_arg (arg)) {
		return do_handle_ts_unescape_arg (state, ts_node_named_child (arg, 0));
	} else if (is_ts_arg_identifier (arg)) {
		return unescape_arg (state, arg, SPECIAL_CHARS_REGULAR);
	} else if (is_ts_single_quoted_arg (arg) || is_ts_double_quoted_arg (arg)) {
		const char *special = is_ts_single_quoted_arg (arg)? SPECIAL_CHARS_SINGLE_QUOTED: SPECIAL_CHARS_DOUBLE_QUOTED;
		return unescape_arg (state, arg, special);
	} else if (is_ts_concatenation (arg)) {
		uint32_t i, n_children = ts_node_named_child_count (arg);
		RStrBuf *sb = r_strbuf_new (NULL);
		for (i = 0; i < n_children; i++) {
			TSNode sub_arg = ts_node_named_child (arg, i);
			char *s = do_handle_ts_unescape_arg (state, sub_arg);
			r_strbuf_append (sb, s);
		}
		return r_strbuf_drain (sb);
	} else {
		return ts_node_sub_string (arg, state->input);
	}
}

static char *create_argv_str(struct parsed_args *a) {
	RStrBuf *sb = r_strbuf_new (NULL);
	int i;
	for (i = 0; i < a->argc; i++) {
		if (i > 0) {
			r_strbuf_append (sb, " ");
		}
		r_strbuf_append (sb, a->argv[i]);
	}
	return r_strbuf_drain (sb);
}

static struct parsed_args *handle_ts_unescape_arg(struct tsr2cmd_state *state, TSNode arg) {
	struct parsed_args *a = R_NEW0 (struct parsed_args);
	a->argc = 1;
	a->argv = R_NEWS (char *, a->argc);
	a->argv[0] = do_handle_ts_unescape_arg (state, arg);
	a->argv_str = create_argv_str (a);
	return a;
}

static struct parsed_args *parse_args(struct tsr2cmd_state *state, TSNode args) {
	r_return_val_if_fail (!ts_node_is_null (args), NULL);
	if (is_ts_args (args)) {
		uint32_t n_children = ts_node_named_child_count (args);
		uint32_t i;
		struct parsed_args *a = R_NEW0 (struct parsed_args);
		a->argc = n_children;
		a->argv = R_NEWS (char *, a->argc);
		for (i = 0; i < n_children; i++) {
			TSNode arg = ts_node_named_child (args, i);
			a->argv[i] = do_handle_ts_unescape_arg (state, arg);
		}
		a->argv_str = create_argv_str (a);
		return a;
	} else {
		return handle_ts_unescape_arg (state, args);
	}
	return NULL;
}

static TSTree *apply_edits(struct tsr2cmd_state *state, RList *edits) {
	struct tsr2cmd_edit *edit;
	RListIter *it;

	R_LOG_DEBUG ("old input = '%s'\n", state->input);
	r_list_foreach (edits, it, edit) {
		R_LOG_DEBUG ("apply_edits: about to replace '%s' with '%s'\n", edit->old_text, edit->new_text);
		state->input = r_str_replace (state->input, edit->old_text, edit->new_text, 0);
	}
	R_LOG_DEBUG ("new input = '%s'\n", state->input);
	return ts_parser_parse_string (state->parser, NULL, state->input, strlen (state->input));
}

static void substitute_args_fini(struct tsr2cmd_state *state) {
	if (state->tree != state->saved_tree) {
		ts_tree_delete (state->tree);
	}
	state->tree = state->saved_tree;
	state->saved_tree = NULL;
	if (state->input != state->saved_input) {
		free (state->input);
	}
	state->input = state->saved_input;
	state->saved_input = NULL;
}

static void substitute_args_init(struct tsr2cmd_state *state, TSNode command) {
	state->saved_input = state->input;
	state->saved_tree = state->tree;
	state->substitute_cmd = command;
	state->input = ts_node_sub_string (state->substitute_cmd, state->input);
	R_LOG_DEBUG ("Shrinking input to '%s'\n", state->input);
}

static bool substitute_args_do(struct tsr2cmd_state *state, RList *edits, TSNode *new_command) {
	TSTree *new_tree = apply_edits (state, edits);
	if (!new_tree) {
		return false;
	}

	TSNode root = ts_tree_root_node (new_tree);
	if (ts_node_has_error (root)) {
		ts_tree_delete (new_tree);
		return false;
	}
	*new_command = ts_node_named_child (root, 0);
	state->tree = new_tree;
	return true;
}

static bool substitute_args(struct tsr2cmd_state *state, TSNode args, TSNode *new_command) {
	RList *edits = r_list_newf ((RListFree)free_tsr2cmd_edit);

	if (is_ts_args (args) || is_ts_arg (args)) {
		handle_substitution_args (state, args, edits);
	}

	bool res = substitute_args_do (state, edits, new_command);
	r_list_free (edits);
	return res;
}

static char *ts_node_handle_arg(struct tsr2cmd_state *state, TSNode command, TSNode arg, uint32_t child_idx) {
	TSNode new_command;
	substitute_args_init (state, command);
	bool ok = substitute_args (state, arg, &new_command);
	if (!ok) {
		R_LOG_ERROR ("Error while substituting arguments\n");
		substitute_args_fini (state);
		return NULL;
	}

	arg = ts_node_named_child (new_command, child_idx);
	struct parsed_args *a = parse_args (state, arg);
	if (a == NULL) {
		R_LOG_ERROR ("Cannot parse arg\n");
		return NULL;
	}
	char *str = strdup (a->argv_str);
	parsed_args_free (a);
	substitute_args_fini (state);
	return str;
}

static char *create_exec_string(char *cmd_str, struct parsed_args *pr_args, bool command_arg_space) {
	RStrBuf *sb = r_strbuf_new (cmd_str);
	if (pr_args) {
		if (command_arg_space) {
			r_strbuf_append (sb, " ");
		}
		r_strbuf_append (sb, pr_args->argv_str);
	}
	return r_strbuf_drain (sb);
}

DEFINE_HANDLE_TS_FCN(arged_command) {
	TSNode command = ts_node_child_by_field_name (node, "command", strlen ("command"));
	r_return_val_if_fail (!ts_node_is_null (command), false);
	char *command_str = ts_node_sub_string (command, state->input);
	R_LOG_DEBUG ("arged_command command: '%s'\n", command_str);
	TSNode args = ts_node_child_by_field_name (node, "args", strlen ("args"));
	bool res = false;

	// FIXME: this special handling should be removed once we have a proper
	//        command tree
	if (!strcmp (command_str, "|.")) {
		char *cmd_str = ts_node_sub_string (args, state->input);
		char *exec_string = r_str_newf (".%s", cmd_str);
		free (cmd_str);
		free (command_str);
		bool res = r_core_cmd (state->core, exec_string, false) != -1;
		free (exec_string);
		return res;
	}

	struct parsed_args *pr_args = NULL;
	if (!ts_node_is_null (args)) {
		TSNode new_command, new_args;
		substitute_args_init (state, node);
		bool ok = substitute_args (state, args, &new_command);
		if (!ok) {
			R_LOG_ERROR ("Error while substituting arguments\n");
			substitute_args_fini (state);
			res = false;
			goto err;
		}
		new_args = ts_node_named_child (new_command, 1);
		pr_args = parse_args (state, new_args);
		if (!pr_args) {
			res = false;
			goto err;
		}

		substitute_args_fini (state);

		int i;
		for (i = 0; i < pr_args->argc; i++) {
			R_LOG_DEBUG ("parsed_arg %d: '%s'\n", i, pr_args->argv[i]);
		}
	}

	bool command_arg_space = !ts_node_is_null (args) && ts_node_end_byte (command) < ts_node_start_byte (args);
	char *exec_string = create_exec_string (command_str, pr_args, command_arg_space);
	R_LOG_DEBUG ("arged_command exec_string = '%s'\n", exec_string);
	res = r_cmd_call (state->core->rcmd, exec_string) != -1;
	free (exec_string);

err:
	parsed_args_free (pr_args);
	free (command_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(legacy_quoted_command) {
	return run_cmd_depth (state->core, node_string) != -1;
}

DEFINE_HANDLE_TS_FCN(repeat_command) {
	TSNode number = ts_node_child_by_field_name (node, "arg", strlen ("arg"));
	char *number_str = ts_node_sub_string (number, state->input);
	int rep = atoi (number_str);
	free (number_str);

	TSNode command = ts_node_child_by_field_name (node, "command", strlen ("command"));
	if (rep > 1 && r_sandbox_enable (0)) {
		eprintf ("Command repeat sugar disabled in sandbox mode (%s)\n", node_string);
		return false;
	}
	if (rep > INTERACTIVE_MAX_REP && r_cons_is_interactive ()) {
		if (!r_cons_yesno ('n', "Are you sure to repeat this %" PFMT64d " times? (y/N)", rep)) {
			return false;
		}
	}

	bool res = true;
	for (int i = 0; i < rep; i++) {
		res &= handle_ts_command (state, command);
	}
	return res;
}

DEFINE_HANDLE_TS_FCN(redirect_command) {
	int pipecolor = r_config_get_i (state->core->config, "scr.color.pipe");
	int ocolor = r_config_get_i (state->core->config, "scr.color");
	int scr_html = -1;
	bool res = false, is_append = false, is_html = false;
	int fdn = 1;

	TSNode redirect_op = ts_node_child_by_field_name (node, "redirect_operator", strlen ("redirect_operator"));
	if (is_ts_fdn_redirect_operator (redirect_op)) {
	} else if (is_ts_fdn_append_operator (redirect_op)) {
		is_append = true;
	} else if (is_ts_html_redirect_operator (redirect_op)) {
		is_html = true;
	} else if (is_ts_html_append_operator (redirect_op)) {
		is_html = true;
		is_append = true;
	} else {
		R_LOG_ERROR ("This should never happen, redirect_operator is no known type");
		r_warn_if_reached ();
	}

	if (is_html) {
		scr_html = r_config_get_i (state->core->config, "scr.html");
		r_config_set_i (state->core->config, "scr.html", true);
		pipecolor = true;
	} else {
		TSNode fd_desc = ts_node_named_child (redirect_op, 0);
		if (!ts_node_is_null (fd_desc)) {
			char *fd_str = ts_node_sub_string (fd_desc, state->input);
			fdn = atoi (fd_str);
			free (fd_str);
		}
	}

	r_cons_set_interactive (false);
	// TODO: allow to use editor as the old behaviour

	// extract the string of the filename we need to write to
	TSNode arg = ts_node_child_by_field_name (node, "arg", strlen ("arg"));
	char *arg_str = ts_node_sub_string (arg, state->input);

	if (arg_str[0] == '$') {
		// redirect output of command to an alias variable
		R_LOG_DEBUG ("redirect_command: alias = '%s'\n", arg_str);
		TSNode command = ts_node_child_by_field_name (node, "command", strlen ("command"));
		char *command_str = ts_node_sub_string (command, state->input);

		char *output = r_core_cmd_str (state->core, command_str);
		char *old_alias_value = r_cmd_alias_get (state->core->rcmd, arg_str, 1);
		char *new_alias_value;
		const char *start_char = "$";
		if (is_append && old_alias_value) {
			start_char = "";
		} else {
			old_alias_value = "";
		}
		new_alias_value = r_str_newf ("%s%s%s", start_char, old_alias_value, output);
		r_cmd_alias_set (state->core->rcmd, arg_str, new_alias_value, 1);
		free (new_alias_value);
		free (command_str);
		res = true;
	} else {
		r_cons_flush ();
		R_LOG_DEBUG ("redirect_command: fdn = %d, is_append = %d\n", fdn, is_append);
		int pipefd = r_cons_pipe_open (arg_str, fdn, is_append);
		if (pipefd != -1) {
			if (!pipecolor) {
				r_config_set_i (state->core->config, "scr.color", COLOR_MODE_DISABLED);
			}
			TSNode command = ts_node_child_by_field_name (node, "command", strlen ("command"));
			res = handle_ts_command (state, command);
			r_cons_flush ();
			r_cons_pipe_close (pipefd);
		} else {
			R_LOG_WARN ("Could not open pipe to %d", fdn);
		}
	}
	free (arg_str);
	r_cons_set_last_interactive ();
	if (!pipecolor) {
		r_config_set_i (state->core->config, "scr.color", ocolor);
	}
	if (scr_html != -1) {
		r_config_set_i (state->core->config, "scr.html", scr_html);
	}
	state->core->cons->use_tts = false;
	return res;
}

DEFINE_HANDLE_TS_FCN(help_command) {
	// TODO: traverse command tree to print help
	// FIXME: once we have a command tree, this special handling should be removed
	if (!strcmp (node_string, "@?")) {
		r_core_cmd_help (state->core, help_msg_at);
	} else if (!strcmp (node_string, "@@?")) {
		r_core_cmd_help (state->core, help_msg_at_at);
	} else if (!strcmp (node_string, "@@@?")) {
		r_core_cmd_help (state->core, help_msg_at_at_at);
	} else if (!strcmp (node_string, "|?")) {
		r_core_cmd_help (state->core, help_msg_vertical_bar);
	} else if (!strcmp (node_string + strlen (node_string) - 2, "?*")) {
		size_t node_len = strlen (node_string);
		int detail = 0;
		if (node_len > 3 && node_string[node_len - 3] == '?') {
			detail++;
			if (node_len > 4 && node_string[node_len - 4] == '?') {
				detail++;
			}
		}
		node_string[node_len - 2 - detail] = '\0';
		recursive_help (state->core, detail, node_string);
		return true;
	} else {
		return r_cmd_call (state->core->rcmd, node_string) != -1;
	}
	return true;
}

DEFINE_HANDLE_TS_FCN(tmp_seek_command) {
	// TODO: handle offsets like "+30", "-13", etc.
	TSNode command = ts_node_named_child (node, 0);
	TSNode offset = ts_node_named_child (node, 1);
	char *offset_string = ts_node_handle_arg (state, node, offset, 1);
	ut64 orig_offset = state->core->offset;
	R_LOG_DEBUG ("tmp_seek_command, changing offset to %s\n", offset_string);
	r_core_seek (state->core, r_num_math (state->core->num, offset_string), 1);
	bool res = handle_ts_command (state, command);
	r_core_seek (state->core, orig_offset, 1);
	free (offset_string);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_blksz_command) {
	TSNode command = ts_node_named_child (node, 0);
	TSNode blksz = ts_node_named_child (node, 1);
	char *blksz_string = ts_node_handle_arg (state, node, blksz, 1);
	ut64 orig_blksz = state->core->blocksize;
	R_LOG_DEBUG ("tmp_blksz_command, changing blksz to %s\n", blksz_string);
	r_core_block_size (state->core, r_num_math (state->core->num, blksz_string));
	bool res = handle_ts_command (state, command);
	r_core_block_size (state->core, orig_blksz);
	free (blksz_string);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_fromto_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode from = ts_node_named_child (node, 1);
	TSNode to = ts_node_named_child (node, 2);
	char *from_str = ts_node_handle_arg (state, node, from, 1);
	char *to_str = ts_node_handle_arg (state, node, to, 2);
	R_LOG_DEBUG ("tmp_fromto_command, changing fromto to (%s, %s)\n", from_str, to_str);

	const char *fromvars[] = { "anal.from", "diff.from", "graph.from",
		"io.buffer.from", "lines.from", "search.from", "zoom.from", NULL };
	const char *tovars[] = { "anal.to", "diff.to", "graph.to",
		"io.buffer.to", "lines.to", "search.to", "zoom.to", NULL };
	ut64 from_val = r_num_math (core->num, from_str);
	ut64 to_val = r_num_math (core->num, to_str);

	RConfigHold *hc = r_config_hold_new (core->config);
	int i;
	for (i = 0; fromvars[i]; i++) {
		r_config_hold_i (hc, fromvars[i], NULL);
		r_config_set_i (core->config, fromvars[i], from_val);
	}
	for (i = 0; tovars[i]; i++) {
		r_config_hold_i (hc, tovars[i], NULL);
		r_config_set_i (core->config, tovars[i], to_val);
	}

	bool res = handle_ts_command (state, command);

	r_config_hold_restore (hc);

	r_config_hold_free (hc);
	free (from_str);
	free (to_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_arch_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	char *tmparch, *tmpbits;
	bool is_arch_set = false, is_bits_set = false;
	bool oldfixedarch = core->fixedarch, oldfixedbits = core->fixedbits;
	int cmd_ignbithints = -1;

	// change arch and bits
	char *q = strchr (arg_str, ':');
	if (q) {
		*q++ = '\0';
		int bits = r_num_math (core->num, q);
		is_bits_set = set_tmp_bits (core, bits, &tmpbits, &cmd_ignbithints);
	}
	is_arch_set = set_tmp_arch (core, arg_str, &tmparch);

	// execute command with changed settings
	bool res = handle_ts_command (state, command);

	// restore original settings
	if (is_arch_set) {
		core->fixedarch = oldfixedarch;
		r_config_set (core->config, "asm.arch", tmparch);
		free (tmparch);
	}
	if (is_bits_set) {
		r_config_set (core->config, "asm.bits", tmpbits);
		core->fixedbits = oldfixedbits;
		free (tmpbits);
	}
	if (cmd_ignbithints != -1) {
		r_config_set_i (core->config, "anal.ignbithints", cmd_ignbithints);
	}
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_bits_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	bool oldfixedbits = core->fixedbits;
	char *tmpbits;
	int cmd_ignbithints;

	int bits = r_num_math (core->num, arg_str);
	set_tmp_bits (core, bits, &tmpbits, &cmd_ignbithints);

	bool res = handle_ts_command (state, command);

	r_config_set (core->config, "asm.bits", tmpbits);
	core->fixedbits = oldfixedbits;
	r_config_set_i (core->config, "anal.ignbithints", cmd_ignbithints);

	free (tmpbits);
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_nthi_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);

	ut64 orig_offset = state->core->offset;
	int index = r_num_math (core->num, arg_str);
	RAnalBlock *bb = r_anal_bb_from_offset (core->anal, core->offset);
	if (bb) {
		// handle negative indices
		if (index < 0) {
			index = bb->ninstr + index;
		}

		if (index >= 0 && index < bb->ninstr) {
			ut16 inst_off = r_anal_bb_offset_inst (bb, index);
			r_core_seek (core, bb->addr + inst_off, 1);
		} else {
			eprintf ("The current basic block has just %d instructions\n", bb->ninstr);
		}
	} else {
		eprintf ("Can't find a basic block for 0x%08" PFMT64x "\n", core->offset);
	}

	bool res = handle_ts_command (state, command);

	r_core_seek (core, orig_offset, 1);

	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_eval_command) {
	// TODO: support cmd_substitution in tmp_eval_args
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode args = ts_node_named_child (node, 1);

	RConfigHold *hc = r_config_hold_new (core->config);
	uint32_t i, n_args = ts_node_named_child_count (args);
	for (i = 0; i < n_args; i++) {
		TSNode arg = ts_node_named_child (args, i);
		char *arg_str = ts_node_sub_string (arg, state->input);
		char *eq = strchr (arg_str, '=');
		if (eq) {
			*eq = 0;
			r_config_hold_s (hc, arg_str, NULL);
			r_config_set (core->config, arg_str, eq + 1);
		} else {
			eprintf ("Missing '=' in e: expression (%s)\n", arg_str);
		}
		free (arg_str);
	}

	bool res = handle_ts_command (state, command);

	r_config_hold_restore (hc);
	r_config_hold_free (hc);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_fs_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	r_flag_space_push (core->flags, arg_str);
	bool res = handle_ts_command (state, command);
	r_flag_space_pop (core->flags);
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_reli_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	ut64 orig_offset = state->core->offset;
	ut64 addr = r_num_math (core->num, arg_str);
	if (addr) {
		r_core_cmdf (core, "so %d", addr);
	}
	bool res = handle_ts_command (state, command);
	r_core_seek (state->core, orig_offset, 1);
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_kuery_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	ut64 orig_offset = state->core->offset;
	char *out = sdb_querys (core->sdb, NULL, 0, arg_str);
	if (out) {
		r_core_seek (core, r_num_math (core->num, out), 1);
		free (out);
	}
	bool res = handle_ts_command (state, command);
	r_core_seek (state->core, orig_offset, 1);
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_fd_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	int tmpfd = core->io->desc ? core->io->desc->fd : -1;
	r_io_use_fd (core->io, atoi (arg_str));
	bool res = handle_ts_command (state, command);
	r_io_use_fd (core->io, tmpfd);
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_reg_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	ut64 orig_offset = state->core->offset;
	// TODO: add support for operations (e.g. @r:PC+10)
	ut64 regval = r_debug_reg_get (core->dbg, arg_str);
	r_core_seek (core, regval, 1);
	bool res = handle_ts_command (state, command);
	r_core_seek (core, orig_offset, 1);
	free (arg_str);
	return res;
}

static bool handle_tmp_desc(struct tsr2cmd_state *state, TSNode command, const ut8 *buf, int sz) {
	RCore *core = state->core;
	int pamode = !core->io->va;
	bool res = false, o_fixedblock = core->fixedblock;
	RBuffer *b = r_buf_new_with_bytes (buf, sz);
	RIODesc *d = r_io_open_buffer (core->io, b, R_PERM_RWX, 0);
	if (!d) {
		eprintf ("Cannot open io buffer\n");
		goto out_buf;
	}
	if (pamode) {
		r_config_set_i (core->config, "io.va", 1);
	}
	r_io_map_new (core->io, d->fd, d->perm, 0, core->offset, r_buf_size (b));
	r_core_block_size (core, r_buf_size (b));
	core->fixedblock = true;
	r_core_block_read (core);

	res = handle_ts_command (state, command);

	core->fixedblock = o_fixedblock;
	if (pamode) {
		r_config_set_i (core->config, "io.va", 0);
	}
	r_io_desc_close (d);

out_buf:
	r_buf_free (b);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_file_command) {
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	int sz;
	bool res = false;

	char *f = r_file_slurp (arg_str, &sz);
	if (!f) {
		eprintf ("Cannot open '%s'\n", arg_str);
		goto out;
	}

	res = handle_tmp_desc (state, command, (ut8 *)f, sz);

	free (f);
out:
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_string_command) {
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	int sz;
	bool res = false;

	sz = strlen (arg_str);
	const ut8 *buf = (const ut8 *)arg_str;

	res = handle_tmp_desc (state, command, buf, sz);

	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(tmp_hex_command) {
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	int sz;
	bool res = false;

	size_t len = strlen (arg_str);
	ut8 *buf = R_NEWS (ut8, len + 1);
	sz = r_hex_str2bin (arg_str, buf);

	res = handle_tmp_desc (state, command, buf, sz);

	free (buf);
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_flags_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg(state, node, arg, 1);
	const RSpace *flagspace = r_flag_space_cur (core->flags);
	RFlagItem *flag;
	RListIter *iter;
	bool ret = true;
	RList *match_flag_items = r_list_newf ((RListFree)r_flag_item_free);
	if (!match_flag_items) {
		return true;
	}

	/* duplicate flags that match word, to be sure the command is going to
	   be executed on flags values at the moment the command is called
	   (without side effects) */
	struct duplicate_flag_t u = {
		.ret = match_flag_items,
		.word = arg_str,
	};
	r_flag_foreach_space (core->flags, flagspace, duplicate_flag, &u);

	/* for all flags that match */
	r_list_foreach (match_flag_items, iter, flag) {
		if (r_cons_is_breaked ()) {
			break;
		}

		char *buf = NULL;
		const char *tmp = NULL;
		R_LOG_DEBUG ("iter_flags_command: seek to %" PFMT64x "\n", flag->offset);
		r_core_seek (core, flag->offset, 1);
		r_cons_push ();
		ret &= handle_ts_command(state, command);
		tmp = r_cons_get_buffer ();
		buf = tmp? strdup (tmp): NULL;
		r_cons_pop ();
		r_cons_strcat (buf);
		free (buf);
		r_core_task_yield (&core->tasks);
	}

	r_list_free (match_flag_items);
	free (arg_str);
	return ret;
}

enum dbt_commands_mode {
	DBT_COMMANDS_MODE_ADDR,
	DBT_COMMANDS_MODE_BP,
	DBT_COMMANDS_MODE_SP,
};

static bool iter_dbt_commands(struct tsr2cmd_state *state, TSNode node, enum dbt_commands_mode mode) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	RList *list = r_debug_frames (core->dbg, UT64_MAX);
	ut64 orig_offset = core->offset;
	RDebugFrame *frame;
	RListIter *iter;
	bool res = true;

	r_list_foreach (list, iter, frame) {
		switch (mode) {
		case DBT_COMMANDS_MODE_ADDR:
			r_core_seek (core, frame->addr, 1);
			break;
		case DBT_COMMANDS_MODE_SP:
			r_core_seek (core, frame->sp, 1);
			break;
		case DBT_COMMANDS_MODE_BP:
			r_core_seek (core, frame->bp, 1);
			break;
		default:
			r_warn_if_reached ();
			return false;
		}
		res &= handle_ts_command (state, command);
		r_cons_newline ();
	}
	r_core_seek (core, orig_offset, 1);
	r_list_free (list);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_dbta_command) {
	return iter_dbt_commands (state, node, DBT_COMMANDS_MODE_ADDR);
}

DEFINE_HANDLE_TS_FCN(iter_dbtb_command) {
	return iter_dbt_commands (state, node, DBT_COMMANDS_MODE_BP);
}

DEFINE_HANDLE_TS_FCN(iter_dbts_command) {
	return iter_dbt_commands (state, node, DBT_COMMANDS_MODE_SP);
}

DEFINE_HANDLE_TS_FCN(iter_file_lines_command) {
	// TODO: old implementation has some unknown check on '('
	RCore *core = state->core;
	bool res = true;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg(state, node, arg, 1);
	ut64 orig_offset = core->offset;
	FILE *fd = r_sandbox_fopen (arg_str, "r");
	if (!fd) {
		res = false;
		goto arg_out;
	}

	core->rcmd->macro.counter = 0;
	while (!feof (fd)) {
		char buf[1024];
		buf[0] = '\0';
		if (!fgets (buf, sizeof (buf), fd)) {
			break;
		}
		ut64 addr = r_num_math (core->num, buf);
		r_core_seek (core, addr, 1);
		res &= handle_ts_command (state, command);
		core->rcmd->macro.counter++;
	}
	r_core_seek (core, orig_offset, 1);
	fclose (fd);

arg_out:
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_offsets_command) {
	RCore *core = state->core;
	bool res = true;
	TSNode command = ts_node_named_child (node, 0);
	if (ts_node_named_child_count (node) < 2) {
		// no offsets provided, all's good.
		return true;
	}

	TSNode args = ts_node_named_child (node, 1);
	ut64 orig_offset = core->offset;

	TSNode new_command;
	substitute_args_init (state, node);
	bool ok = substitute_args (state, args, &new_command);
	if (!ok) {
		R_LOG_ERROR ("Error while substituting arguments\n");
		substitute_args_fini (state);
		return false;
	}
	args = ts_node_named_child (new_command, 1);
	if (ts_node_is_null (args)) {
		// after replacing cmd substitution, no args are provided.
		substitute_args_fini (state);
		return true;
	}

	struct parsed_args *a = parse_args (state, args);
	if (a == NULL) {
		R_LOG_ERROR ("Cannot parse args\n");
		return false;
	}
	substitute_args_fini (state);

	int i;
	for (i = 0; i < a->argc; i++) {
		ut64 addr = r_num_math (core->num, a->argv[i]);
		R_LOG_DEBUG ("iter_offsets_command: seek to %" PFMT64x "\n", addr);
		r_core_seek (core, addr, 1);
		res &= handle_ts_command (state, command);
		r_cons_flush ();
	}

	r_core_seek (core, orig_offset, 1);
	parsed_args_free (a);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_sdbquery_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = ts_node_handle_arg (state, node, arg, 1);
	ut64 orig_offset = core->offset;

	char *out = sdb_querys (core->sdb, NULL, 0, arg_str);
	if (!out) {
		return false;
	}
	char *str, *each = out;
	ut64 addr;
	bool res = true;
	do {
		while (*each == ' ') {
			each++;
		}
		if (!*each) {
			break;
		}
		str = strchr (each, ' ');
		if (str) {
			*str = '\0';
			addr = r_num_math (core->num, each);
			*str = ' ';
		} else {
			addr = r_num_math (core->num, each);
		}
		each = str + 1;
		r_core_seek (core, addr, 1);
		res &= handle_ts_command (state, command);
		r_cons_flush ();
	} while (str != NULL);
	r_core_seek (core, orig_offset, 1);
	free (out);
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_threads_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	int pid = core->dbg->pid;
	if (!core->dbg->h || !core->dbg->h->pids) {
		return false;
	}

	bool res = true;
	RList *list = core->dbg->h->pids (core->dbg, R_MAX (0, pid));
	RListIter *iter;
	RDebugPid *p;
	r_list_foreach (list, iter, p) {
		r_cons_printf ("# PID %d\n", p->pid);
		r_debug_select (core->dbg, p->pid, p->pid);
		res &= handle_ts_command (state, command);
		r_cons_newline ();
	}
	r_list_free (list);
	r_debug_select (core->dbg, pid, pid);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_bbs_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	RListIter *iter;
	RAnalBlock *bb;
	int bs = core->blocksize;
	ut64 orig_offset = core->offset;
	bool res = true;
	RAnalFunction *fcn = r_anal_get_function_at (core->anal, core->offset);
	if (!fcn) {
		eprintf ("No function at current address\n");
		return false;
	}
	r_list_sort (fcn->bbs, bb_cmp);
	r_list_foreach (fcn->bbs, iter, bb) {
		r_core_block_size (core, bb->size);
		r_core_seek (core, bb->addr, 1);
		res &= handle_ts_command (state, command);
		if (r_cons_is_breaked ()) {
			break;
		}
	}
	r_core_block_size (core, bs);
	r_core_seek (core, orig_offset, 1);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_instrs_command) {
	TSNode command = ts_node_named_child (node, 0);
	RCore *core = state->core;
	RListIter *iter;
	RAnalBlock *bb;
	int i;
	bool res = true;
	ut64 orig_offset = core->offset;
	int bs = core->blocksize;
	RAnalFunction *fcn = r_anal_get_function_at (core->anal, core->offset);
	if (!fcn) {
		eprintf ("No function at current address\n");
		return false;
	}
	r_list_sort (fcn->bbs, bb_cmp);
	r_list_foreach (fcn->bbs, iter, bb) {
		for (i = 0; i < bb->ninstr; i++) {
			ut64 addr = bb->addr + r_anal_bb_offset_inst (bb, i);
			int sz = r_anal_bb_size_i (bb, i);
			r_core_block_size (core, sz);
			r_core_seek (core, addr, 1);
			res &= handle_ts_command (state, command);
			if (r_cons_is_breaked ()) {
				break;
			}
		}
	}

	r_core_block_size (core, bs);
	r_core_seek (core, orig_offset, 1);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_functions_command) {
	TSNode command = ts_node_named_child (node, 0);
	TSNode arg = ts_node_named_child (node, 1);
	char *arg_str = NULL;
	RCore *core = state->core;
	bool res = true;
	ut64 orig_offset = core->offset;
	int bs = core->blocksize;
	RAnalFunction *fcn;
	RListIter *iter;

	if (!ts_node_is_null (arg)) {
		arg_str = ts_node_handle_arg (state, node, arg, 1);
	}

	r_list_foreach (core->anal->fcns, iter, fcn) {
		if (arg_str && !strstr (fcn->name, arg_str)) {
			continue;
		}
		char *buf;
		r_core_block_size (core, r_anal_function_linear_size (fcn));
		r_core_seek (core, fcn->addr, 1);
		r_cons_push ();
		res &= handle_ts_command (state, command);
		buf = (char *)r_cons_get_buffer ();
		if (buf) {
			buf = strdup (buf);
		}
		r_cons_pop ();
		r_cons_strcat (buf);
		free (buf);
		if (r_cons_is_breaked ()) {
			break;
		}
	}

	r_core_block_size (core, bs);
	r_core_seek (core, orig_offset, 1);
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_step_command) {
	TSNode command = ts_node_named_child (node, 0);
	TSNode from_n = ts_node_named_child (node, 1);
	TSNode to_n = ts_node_named_child (node, 2);
	TSNode step_n = ts_node_named_child (node, 3);
	RCore *core = state->core;
	bool res = true;
	ut64 orig_offset = core->offset;
	int bs = core->blocksize;

	char *from_str = ts_node_handle_arg (state, node, from_n, 1);
	char *to_str = ts_node_handle_arg (state, node, to_n, 2);
	char *step_str = ts_node_handle_arg (state, node, step_n, 3);
	ut64 from = r_num_math (core->num, from_str);
	ut64 to = r_num_math (core->num, to_str);
	ut64 step = r_num_math (core->num, step_str);
	free (from_str);
	free (to_str);
	free (step_str);

	ut64 cur;
	for (cur = from; cur < to; cur += step) {
		r_core_seek (core, cur, 1);
		r_core_block_size (core, step);
		res &= handle_ts_command (state, command);
		if (r_cons_is_breaked ()) {
			break;
		}
	}

	r_core_block_size (core, bs);
	r_core_seek (core, orig_offset, 1);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_interpret_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode in_cmd = ts_node_named_child (node, 1);
	substitute_args_init (state, node);

	RList *edits = r_list_newf ((RListFree)free_tsr2cmd_edit);

	char *in_cmd_out = do_handle_substitution_cmd (state, in_cmd);
	in_cmd_out = escape_special_chars (in_cmd_out, SPECIAL_CHARS_REGULAR);
	struct tsr2cmd_edit *e = create_cmd_edit (state, in_cmd, in_cmd_out);
	r_list_append (edits, e);

	TSNode op = ts_node_child (node, 1);
	e = create_cmd_edit (state, op, strdup ("@@="));
	r_list_append (edits, e);

	TSNode new_command;
	bool ok = substitute_args_do (state, edits, &new_command);
	if (!ok) {
		r_list_free (edits);
		substitute_args_fini (state);
		return false;
	}
	TSNode args = ts_node_named_child (new_command, 1);

	struct parsed_args *a = parse_args (state, args);
	if (!a) {
		r_list_free (edits);
		substitute_args_fini (state);
		return false;
	}

	r_list_free (edits);
	substitute_args_fini (state);

	int i;
	ut64 orig_offset = core->offset;
	bool res = true;
	for (i = 0; i < a->argc; i++) {
		ut64 addr = r_num_math (core->num, a->argv[i]);
		R_LOG_DEBUG ("iter_interpret_command: seek to %" PFMT64x "\n", addr);
		r_core_seek (core, addr, 1);
		res &= handle_ts_command (state, command);
		r_cons_flush ();
	}

	r_core_seek (core, orig_offset, 1);
	parsed_args_free (a);
	return res;
}

DEFINE_HANDLE_TS_FCN(iter_hit_command) {
	RCore *core = state->core;
	TSNode command = ts_node_named_child (node, 0);
	TSNode search_cmd = ts_node_named_child (node, 1);
	char *command_str = ts_node_sub_string (command, state->input);
	char *cmdhit = strdup (r_config_get (core->config, "cmd.hit"));
	r_config_set (core->config, "cmd.hit", command_str);
	bool res = handle_ts_command (state, search_cmd);
	r_config_set (core->config, "cmd.hit", cmdhit);
	free (command_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(last_command) {
	TSNode command = ts_node_child_by_field_name (node, "command", strlen ("command"));
	char *command_str = ts_node_sub_string (command, state->input);
	bool res = false;
	state->is_last_cmd = true;
	if (!strcmp (command_str, ".")) {
		res = lastcmd_repeat (state->core, 0);
	} else if (!strcmp (command_str, "...")) {
		res = lastcmd_repeat (state->core, 1);
	} else {
		r_warn_if_reached ();
	}
	free (command_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(grep_command) {
	TSNode command = ts_node_child_by_field_name (node, "command", strlen ("command"));
	TSNode arg = ts_node_child_by_field_name (node, "specifier", strlen ("specifier"));
	char *arg_str = ts_node_sub_string (arg, state->input);
	bool res = handle_ts_command (state, command);
	R_LOG_DEBUG ("grep_command specifier: '%s'\n", arg_str);
	RStrBuf *sb = r_strbuf_new (arg_str);
	r_strbuf_prepend (sb, "~");
	char *specifier_str = r_cons_grep_strip (r_strbuf_get (sb), "`");
	r_strbuf_free (sb);
	R_LOG_DEBUG ("grep_command processed specifier: '%s'\n", specifier_str);
	r_cons_grep_process (specifier_str);
	free (arg_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(html_disable_command) {
	TSNode command = ts_node_child_by_field_name (node, "command", strlen ("command"));
	int scr_html = r_config_get_i (state->core->config, "scr.html");
	r_config_set_i (state->core->config, "scr.html", 0);
	int scr_color = r_config_get_i (state->core->config, "scr.color");
	r_config_set_i (state->core->config, "scr.color", COLOR_MODE_DISABLED);
	bool res = handle_ts_command (state, command);
	if (scr_html != -1) {
		r_cons_flush ();
		r_config_set_i (state->core->config, "scr.html", scr_html);
	}
	if (scr_color != -1) {
		r_config_set_i (state->core->config, "scr.color", scr_color);
	}
	return res;
}

DEFINE_HANDLE_TS_FCN(html_enable_command) {
	TSNode command = ts_node_child_by_field_name (node, "command", strlen ("command"));
	int scr_html = r_config_get_i (state->core->config, "scr.html");
	r_config_set_i (state->core->config, "scr.html", true);
	bool res = handle_ts_command (state, command);
	if (scr_html != -1) {
		r_cons_flush ();
		r_config_set_i (state->core->config, "scr.html", scr_html);
	}
	return res;
}

DEFINE_HANDLE_TS_FCN(pipe_command) {
	TSNode first_cmd = ts_node_named_child (node, 0);
	r_return_val_if_fail (!ts_node_is_null (first_cmd), false);
	TSNode second_cmd = ts_node_named_child (node, 1);
	r_return_val_if_fail (!ts_node_is_null (second_cmd), false);
	char *first_str = ts_node_sub_string (first_cmd, state->input);
	char *second_str = ts_node_sub_string (second_cmd, state->input);
	int value = state->core->num->value;
	bool res = r_core_cmd_pipe (state->core, first_str, second_str) != -1;
	state->core->num->value = value;
	free (first_str);
	free (second_str);
	return res;
}

DEFINE_HANDLE_TS_FCN(scr_tts_command) {
	TSNode command = ts_node_child_by_field_name (node, "command", strlen ("command"));
	int scr_color = r_config_get_i (state->core->config, "scr.color");
	r_config_set_i (state->core->config, "scr.color", COLOR_MODE_DISABLED);
	state->core->cons->use_tts = true;
	bool res = handle_ts_command (state, command);
	if (scr_color != -1) {
		r_config_set_i (state->core->config, "scr.color", scr_color);
	}
	return res;
}

DEFINE_HANDLE_TS_FCN(number_command) {
	ut64 addr = r_num_math (state->core->num, node_string);
	r_core_seek (state->core, addr, 1);
	return true;
}

static bool handle_ts_command(struct tsr2cmd_state *state, TSNode node) {
	bool ret = false;
	RCmd *cmd = state->core->rcmd;

	TSSymbol node_symbol = ts_node_symbol (node);
	ts_handler handler = ht_up_find (cmd->ts_symbols_ht, node_symbol, NULL);

	state->is_last_cmd = false;
	if (handler) {
		ret = handler (state, node);
	} else {
		R_LOG_WARN ("No handler for this kind of command `%s`\n", ts_node_type (node));
	}
	if (state->log && !state->is_last_cmd) {
		free (state->core->lastcmd);
		state->core->lastcmd = ts_node_sub_string (node, state->input);
	}
	return ret;
}

DEFINE_HANDLE_TS_FCN(commands) {
	ut32 child_count = ts_node_named_child_count (node);
	bool res = true;
	int i;

	R_LOG_DEBUG ("commands with %d childs\n", child_count);
	if (child_count == 0 && !*state->input) {
		if (state->core->cons->context->breaked) {
			state->core->cons->context->breaked = false;
			return false;
		}
		if (!state->core->cmdrepeat) {
			return false;
		}
		return lastcmd_repeat (state->core, true);
	}
	if (state->split_lines) {
		r_cons_break_push (NULL, NULL);
	}
	for (i = 0; i < child_count; i++) {
		if (state->split_lines && r_cons_is_breaked ()) {
			r_cons_break_pop ();
			return false;
		}
		TSNode command = ts_node_named_child (node, i);
		res &= handle_ts_command (state, command);
		if (!res) {
			char *command_str = ts_node_sub_string (command, state->input);
			eprintf ("Error while executing command: %s\n", command_str);
			free (command_str);
			return false;
		}
		if (state->split_lines) {
			r_cons_flush ();
			r_core_task_yield (&state->core->tasks);
		}
	}
	if (state->split_lines) {
		r_cons_break_pop ();
	}
	/* run pending analysis commands */
	run_pending_anal (state->core);
	return res;
}

#define HANDLER_RULE_OP(name) { #name, handle_ts_##name },
#define RULE_OP(name)

struct ts_data_symbol_map map[] = {
	#include "r2-shell-parser-cmds.inc"
	{ NULL, NULL },
};

#define RULE_OP(name) { #name, &ts_##name##_symbol },
#define HANDLER_RULE_OP(name) RULE_OP(name)

struct ts_data_symbol_map map_symbols[] = {
	#include "r2-shell-parser-cmds.inc"
	{ NULL, NULL },
};

static void ts_symbols_init(RCmd *cmd) {
	if (cmd->language) {
		return;
	}
	TSLanguage *lang = tree_sitter_r2cmd ();
	cmd->language = lang;
	cmd->ts_symbols_ht = ht_up_new0 ();
	struct ts_data_symbol_map *entry = map;
	while (entry->name) {
		TSSymbol symbol = ts_language_symbol_for_name (lang, entry->name, strlen (entry->name), true);
		ht_up_insert (cmd->ts_symbols_ht, symbol, entry->data);
		entry++;
	}

	entry = map_symbols;
	while (entry->name) {
		TSSymbol *sym_ptr = entry->data;
		*sym_ptr = ts_language_symbol_for_name (lang, entry->name, strlen (entry->name), true);
		entry++;
	}
}

static bool core_cmd_tsr2cmd(RCore *core, const char *cstr, bool split_lines, bool log) {
	char *input = strdup (r_str_trim_head_ro (cstr));

	ts_symbols_init (core->rcmd);

	TSParser *parser = ts_parser_new ();
	ts_parser_set_language (parser, (TSLanguage *)core->rcmd->language);

	TSTree *tree = ts_parser_parse_string (parser, NULL, input, strlen (input));
	TSNode root = ts_tree_root_node (tree);

	bool res = false;
	struct tsr2cmd_state state;
	state.parser = parser;
	state.core = core;
	state.input = input;
	state.tree = tree;
	state.log = log;
	state.split_lines = split_lines;

	if (state.log) {
		r_line_hist_add (state.input);
	}

	if (is_ts_commands (root) && !ts_node_has_error (root)) {
		res = handle_ts_commands (&state, root);
	} else {
		// TODO: print a more meaningful error message and use the ERROR
		// tokens to indicate where, probably, the error is.
		eprintf ("Error while parsing command: `%s`\n", input);
	}

	ts_tree_delete (tree);
	ts_parser_delete (parser);
	free (input);
	return res;
}
#endif

static int run_cmd_depth(RCore *core, char *cmd) {
	char *rcmd;
	int ret = false;

	if (core->cons->context->cmd_depth < 1) {
		eprintf ("r_core_cmd: That was too deep (%s)...\n", cmd);
		run_pending_anal (core);
		return false;
	}
	core->cons->context->cmd_depth--;
	for (rcmd = cmd;;) {
		char *ptr = strchr (rcmd, '\n');
		if (ptr) {
			*ptr = '\0';
		}
		ret = r_core_cmd_subst (core, rcmd);
		if (ret == -1) {
			eprintf ("|ERROR| Invalid command '%s' (0x%02x)\n", rcmd, *rcmd);
			break;
		}
		if (!ptr) {
			break;
		}
		rcmd = ptr + 1;
	}
	/* run pending analysis commands */
	run_pending_anal (core);
	core->cons->context->cmd_depth++;
	return ret;
}

R_API int r_core_cmd(RCore *core, const char *cstr, int log) {
	if (core->use_tree_sitter_r2cmd) {
#if USE_TREESITTER
		return core_cmd_tsr2cmd (core, cstr, false, log)? 0: 1;
#else
		R_LOG_WARN ("No compilation support for radare2-shell-parser\n");
#endif
	}

	int ret = false, i;

	if (core->cmdfilter) {
		const char *invalid_chars = ";|>`@";
		for (i = 0; invalid_chars[i]; i++) {
			if (strchr (cstr, invalid_chars[i])) {
				ret = true;
				goto beach;
			}
		}
		if (strncmp (cstr, core->cmdfilter, strlen (core->cmdfilter))) {
			ret = true;
			goto beach;
		}
	}
	if (core->cmdremote) {
		if (*cstr != '=' && *cstr != 'q' && strncmp (cstr, "!=", 2)) {
			char *res = r_io_system (core->io, cstr);
			if (res) {
				r_cons_printf ("%s\n", res);
				free (res);
			}
			goto beach; // false
		}
	}

	if (!cstr || (*cstr == '|' && cstr[1] != '?')) {
		// raw comment syntax
		goto beach; // false;
	}
	if (!strncmp (cstr, "/*", 2)) {
		if (r_sandbox_enable (0)) {
			eprintf ("This command is disabled in sandbox mode\n");
			goto beach; // false
		}
		core->incomment = true;
	} else if (!strncmp (cstr, "*/", 2)) {
		core->incomment = false;
		goto beach; // false
	}
	if (core->incomment) {
		goto beach; // false
	}
	if (log && (*cstr && (*cstr != '.' || !strncmp (cstr, ".(", 2)))) {
		free (core->lastcmd);
		core->lastcmd = strdup (cstr);
	}

	char *cmd = malloc (strlen (cstr) + 4096);
	if (!cmd) {
		goto beach;
	}
	r_str_cpy (cmd, cstr);
	if (log) {
		r_line_hist_add (cstr);
	}

	ret = run_cmd_depth (core, cmd);
	free (cmd);
	return ret;
beach:
	/* run pending analysis commands */
	run_pending_anal (core);
	return ret;
}

R_API int r_core_cmd_lines(RCore *core, const char *lines) {
	// FIXME: when cfg.newshell=true, just use core_cmd_tsr2cmd, which is
	// able to work on a full script and does not need to split lines. For
	// now, we avoid it because some commands still don't work with the new
	// parser and if a script contains even a single invalid line, it is not
	// parsed at all.
	int r, ret = true;
	char *nl, *data, *odata;

	if (!lines || !*lines) {
		return true;
	}
	data = odata = strdup (lines);
	if (!odata) {
		return false;
	}
	nl = strchr (odata, '\n');
	if (nl) {
		r_cons_break_push (NULL, NULL);
		do {
			if (r_cons_is_breaked ()) {
				free (odata);
				r_cons_break_pop ();
				return ret;
			}
			*nl = '\0';
			r = r_core_cmd (core, data, 0);
			if (r < 0) { //== -1) {
				data = nl + 1;
				ret = -1; //r; //false;
				break;
			}
			r_cons_flush ();
			if (data[0] == 'q') {
				if (data[1] == '!') {
					ret = -1;
				} else {
					eprintf ("'q': quit ignored. Use 'q!'\n");
				}
				data = nl + 1;
				break;
			}
			data = nl + 1;
			r_core_task_yield (&core->tasks);
		} while ((nl = strchr (data, '\n')));
		r_cons_break_pop ();
	}
	if (ret >= 0 && data && *data) {
		r_core_cmd (core, data, 0);
		r_cons_flush ();
		r_core_task_yield (&core->tasks);
	}
	free (odata);
	return ret;
}

R_API int r_core_cmd_file(RCore *core, const char *file) {
	char *data = r_file_abspath (file);
	if (!data) {
		return false;
	}
	char *odata = r_file_slurp (data, NULL);
	free (data);
	if (!odata) {
		return false;
	}
	if (!r_core_cmd_lines (core, odata)) {
		eprintf ("Failed to run script '%s'\n", file);
		free (odata);
		return false;
	}
	free (odata);
	return true;
}

R_API int r_core_cmd_command(RCore *core, const char *command) {
	int ret, len;
	char *buf, *rcmd, *ptr;
	char *cmd = r_core_sysenv_begin (core, command);
	rcmd = ptr = buf = r_sys_cmd_str (cmd, 0, &len);
	if (!buf) {
		free (cmd);
		return -1;
	}
	ret = r_core_cmd (core, rcmd, 0);
	r_core_sysenv_end (core, command);
	free (buf);
	return ret;
}

//TODO: Fix disasm loop is mandatory
R_API char *r_core_disassemble_instr(RCore *core, ut64 addr, int l) {
	char *cmd, *ret = NULL;
	cmd = r_str_newf ("pd %i @ 0x%08"PFMT64x, l, addr);
	if (cmd) {
		ret = r_core_cmd_str (core, cmd);
		free (cmd);
	}
	return ret;
}

R_API char *r_core_disassemble_bytes(RCore *core, ut64 addr, int b) {
	char *cmd, *ret = NULL;
	cmd = r_str_newf ("pD %i @ 0x%08"PFMT64x, b, addr);
	if (cmd) {
		ret = r_core_cmd_str (core, cmd);
		free (cmd);
	}
	return ret;
}

R_API int r_core_cmd_buffer(RCore *core, const char *buf) {
	char *ptr, *optr, *str = strdup (buf);
	if (!str) {
		return false;
	}
	optr = str;
	ptr = strchr (str, '\n');
	while (ptr) {
		*ptr = '\0';
		r_core_cmd (core, optr, 0);
		optr = ptr + 1;
		ptr = strchr (str, '\n');
	}
	r_core_cmd (core, optr, 0);
	free (str);
	return true;
}

R_API int r_core_cmdf(RCore *core, const char *fmt, ...) {
	char string[4096];
	int ret;
	va_list ap;
	va_start (ap, fmt);
	vsnprintf (string, sizeof (string), fmt, ap);
	ret = r_core_cmd (core, string, 0);
	va_end (ap);
	return ret;
}

R_API int r_core_cmd0(RCore *core, const char *cmd) {
	return r_core_cmd (core, cmd, 0);
}

R_API int r_core_flush(RCore *core, const char *cmd) {
	int ret = r_core_cmd (core, cmd, 0);
	r_cons_flush ();
	return ret;
}

R_API char *r_core_cmd_str_pipe(RCore *core, const char *cmd) {
	char *tmp = NULL;
	char *p = (*cmd != '"')? strchr (cmd, '|'): NULL;
	if (!p && *cmd != '!' && *cmd != '.') {
		return r_core_cmd_str (core, cmd);
	}
#if 0
	char *p = (*cmd != '"')? strchr (cmd, '|'): NULL;
	if (p) {
		// This code works but its pretty ugly as its a workaround to
		// make the webserver work as expected, this was broken some
		// weeks. let's use this hackaround for now
		char *c = strdup (cmd);
		c[p - cmd] = 0;
		if (!strcmp (p + 1, "H")) {
			char *res = r_core_cmd_str (core, c);
			free (c);
			char *hres = r_cons_html_filter (res, NULL);
			free (res);
			return hres;
		} else {
			int sh = r_config_get_i (core->config, "scr.color");
			r_config_set_i (core->config, "scr.color", 0);
			char *ret = r_core_cmd_str (core, c);
			r_config_set_i (core->config, "scr.color", sh);
			free (c);
			return ret;
		}
	}
	return r_core_cmd_str (core, cmd);
#else
	r_cons_reset ();
	r_sandbox_disable (true);
	if (r_file_mkstemp ("cmd", &tmp) != -1) {
		int pipefd = r_cons_pipe_open (tmp, 1, 0);
		if (pipefd == -1) {
			r_file_rm (tmp);
			r_sandbox_disable (false);
			free (tmp);
			return r_core_cmd_str (core, cmd);
		}
		char *_cmd = strdup (cmd);
		r_core_cmd_subst (core, _cmd);
		r_cons_flush ();
		r_cons_pipe_close (pipefd);
		if (r_file_exists (tmp)) {
			char *s = r_file_slurp (tmp, NULL);
			r_file_rm (tmp);
			r_sandbox_disable (false);
			free (tmp);
			free (_cmd);
			return s? s: strdup ("");
		}
		eprintf ("slurp %s fails\n", tmp);
		r_file_rm (tmp);
		free (tmp);
		free (_cmd);
		r_sandbox_disable (false);
		return r_core_cmd_str (core, cmd);
	}
	r_sandbox_disable (0);
	return NULL;
#endif
}

R_API char *r_core_cmd_strf(RCore *core, const char *fmt, ...) {
	char string[4096];
	char *ret;
	va_list ap;
	va_start (ap, fmt);
	vsnprintf (string, sizeof (string), fmt, ap);
	ret = r_core_cmd_str (core, string);
	va_end (ap);
	return ret;
}

/* return: pointer to a buffer with the output of the command */
R_API char *r_core_cmd_str(RCore *core, const char *cmd) {
	const char *static_str;
	char *retstr = NULL;
	r_cons_push ();
	if (r_core_cmd (core, cmd, 0) == -1) {
		//eprintf ("Invalid command: %s\n", cmd);
		return NULL;
	}
	r_cons_filter ();
	static_str = r_cons_get_buffer ();
	retstr = strdup (static_str? static_str: "");
	r_cons_pop ();
	r_cons_echo (NULL);
	return retstr;
}

/* run cmd in the main task synchronously */
R_API int r_core_cmd_task_sync(RCore *core, const char *cmd, bool log) {
	RCoreTask *task = core->tasks.main_task;
	char *s = strdup (cmd);
	if (!s) {
		return 0;
	}
	task->cmd = s;
	task->cmd_log = log;
	task->state = R_CORE_TASK_STATE_BEFORE_START;
	int res = r_core_task_run_sync (&core->tasks, task);
	free (s);
	return res;
}

static int cmd_ox(void *data, const char *input) {
	return r_core_cmdf ((RCore*)data, "s 0%s", input);
}

static int compare_cmd_descriptor_name(const void *a, const void *b) {
	return strcmp (((RCmdDescriptor *)a)->cmd, ((RCmdDescriptor *)b)->cmd);
}

static void cmd_descriptor_init(RCore *core) {
	const ut8 *p;
	RListIter *iter;
	RCmdDescriptor *x, *y;
	int n = core->cmd_descriptors->length;
	r_list_sort (core->cmd_descriptors, compare_cmd_descriptor_name);
	r_list_foreach (core->cmd_descriptors, iter, y) {
		if (--n < 0) {
			break;
		}
		x = &core->root_cmd_descriptor;
		for (p = (const ut8 *)y->cmd; *p; p++) {
			if (!x->sub[*p]) {
				if (p[1]) {
					RCmdDescriptor *d = R_NEW0 (RCmdDescriptor);
					r_list_append (core->cmd_descriptors, d);
					x->sub[*p] = d;
				} else {
					x->sub[*p] = y;
				}
			} else if (!p[1]) {
				eprintf ("Command '%s' is duplicated, please check\n", y->cmd);
			}
			x = x->sub[*p];
		}
	}
}

static int core_cmd0_wrapper(void *core, const char *cmd) {
	return r_core_cmd0 ((RCore *)core, cmd);
}

R_API void r_core_cmd_init(RCore *core) {
	struct {
		const char *cmd;
		const char *description;
		r_cmd_callback (cb);
		void (*descriptor_init)(RCore *core);
	} cmds[] = {
		{"!",        "run system command", cmd_system},
		{"_",        "print last output", cmd_last},
		{"#",        "calculate hash", cmd_hash},
		{"$",        "alias", cmd_alias},
		{"%",        "short version of 'env' command", cmd_env},
		{"&",        "tasks", cmd_tasks},
		{"(",        "macro", cmd_macro, cmd_macro_init},
		{"*",        "pointer read/write", cmd_pointer},
		{"-",        "open cfg.editor and run script", cmd_stdin},
		{".",        "interpret", cmd_interpret},
		{"/",        "search kw, pattern aes", cmd_search, cmd_search_init},
		{"=",        "io pipe", cmd_rap},
		{"?",        "help message", cmd_help, cmd_help_init},
		{"\\",       "alias for =!", cmd_rap_run},
		{"'",        "alias for =!", cmd_rap_run},
		{"0x",       "alias for s 0x", cmd_ox},
		{"analysis", "analysis", cmd_anal, cmd_anal_init},
		{"bsize",    "change block size", cmd_bsize},
		{"cmp",      "compare memory", cmd_cmp, cmd_cmp_init},
		{"Code",     "code metadata", cmd_meta, cmd_meta_init},
		{"debug",    "debugger operations", cmd_debug, cmd_debug_init},
		{"eval",     "evaluate configuration variable", cmd_eval, cmd_eval_init},
		{"flag",     "get/set flags", cmd_flag, cmd_flag_init},
		{"g",        "egg manipulation", cmd_egg, cmd_egg_init},
		{"info",     "get file info", cmd_info, cmd_info_init},
		{"kuery",    "perform sdb query", cmd_kuery},
		{"l",       "list files and directories", cmd_ls},
		{"join",    "join the contents of the two files", cmd_join},
		{"head",    "show the top n number of line in file", cmd_head},
		{"L",        "manage dynamically loaded plugins", cmd_plugins},
		{"mount",    "mount filesystem", cmd_mount, cmd_mount_init},
		{"open",     "open or map file", cmd_open, cmd_open_init},
		{"print",    "print current block", cmd_print, cmd_print_init},
		{"Project",  "project", cmd_project, cmd_project_init},
		{"quit",     "exit program session", cmd_quit, cmd_quit_init},
		{"Q",        "alias for q!", cmd_Quit},
		{":",        "long commands starting with :", cmd_colon},
		{"resize",   "change file size", cmd_resize},
		{"seek",     "seek to an offset", cmd_seek, cmd_seek_init},
		{"t",        "type information (cparse)", cmd_type, cmd_type_init},
		{"Text",     "Text log utility", cmd_log, cmd_log_init},
		{"u",        "uname/undo", cmd_undo},
		{"<",        "pipe into RCons.readChar", cmd_pipein},
		{"Visual",   "enter visual mode", cmd_visual},
		{"visualPanels",   "enter visual mode", cmd_panels},
		{"write",    "write bytes", cmd_write, cmd_write_init},
		{"x",        "alias for px", cmd_hexdump},
		{"yank",     "yank bytes", cmd_yank},
		{"zign",     "zignatures", cmd_zign, cmd_zign_init},
	};

	core->rcmd = r_cmd_new ();
	core->rcmd->macro.user = core;
	core->rcmd->macro.num = core->num;
	core->rcmd->macro.cmd = core_cmd0_wrapper;
	core->rcmd->nullcallback = r_core_cmd_nullcallback;
	core->rcmd->macro.cb_printf = (PrintfCallback)r_cons_printf;
	r_cmd_set_data (core->rcmd, core);
	core->cmd_descriptors = r_list_newf (free);
	int i;
	for (i = 0; i < R_ARRAY_SIZE (cmds); i++) {
		r_cmd_add (core->rcmd, cmds[i].cmd, cmds[i].description, cmds[i].cb);
		if (cmds[i].descriptor_init) {
			cmds[i].descriptor_init (core);
		}
	}
	DEFINE_CMD_DESCRIPTOR_SPECIAL (core, $, dollar);
	DEFINE_CMD_DESCRIPTOR_SPECIAL (core, %, percent);
	DEFINE_CMD_DESCRIPTOR_SPECIAL (core, *, star);
	DEFINE_CMD_DESCRIPTOR_SPECIAL (core, ., dot);
	DEFINE_CMD_DESCRIPTOR_SPECIAL (core, =, equal);
	DEFINE_CMD_DESCRIPTOR (core, b);
	DEFINE_CMD_DESCRIPTOR (core, k);
	DEFINE_CMD_DESCRIPTOR (core, r);
	DEFINE_CMD_DESCRIPTOR (core, u);
	DEFINE_CMD_DESCRIPTOR (core, y);
	cmd_descriptor_init (core);
}
