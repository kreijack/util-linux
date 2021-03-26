/*
 * lsfd(1) - list file descriptors
 *
 * Copyright (C) 2021 Red Hat, Inc. All rights reserved.
 * Written by Masatake YAMATO <yamato@redhat.com>
 *
 * Very generally based on lsof(8) by Victor A. Abell <abe@purdue.edu>
 * It supports multiple OSes. lsfd specializes to Linux.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <sys/types.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>

#include "c.h"
#include "nls.h"
#include "xalloc.h"
#include "list.h"
#include "closestream.h"
#include "strutils.h"
#include "procutils.h"
#include "fileutils.h"
#include "idcache.h"

#include "libsmartcols.h"

#include "lsfd.h"

/*
 * Multi-threading related stuffs
 */
#define NUM_COLLECTORS 2
static pthread_cond_t  procs_ready = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t procs_ready_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t procs_consumer_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list_head *current_proc;

static void *fill_procs(void *arg);

/*
 * idcaches
 */
struct idcache *username_cache;

/*
 * Column related stuffs
 */

/* column names */
struct colinfo {
	const char *name;
	double whint;
	int flags;
	const char *help;
};

/* columns descriptions */
static struct colinfo infos[] = {
	[COL_PID]     = { "PID",      0, SCOLS_FL_RIGHT, N_("PID of the process opening the file") },
	[COL_FD]      = { "FD",       0, SCOLS_FL_RIGHT, N_("file descriptor for the file") },
	[COL_NAME]    = { "NAME",     0, 0,              N_("name of the file") },
	[COL_COMMAND] = { "COMMAND",  0, 0,              N_("command of the process opening the file") },
	[COL_TYPE]    = { "TYPE",     0, SCOLS_FL_RIGHT, N_("file type") },
	[COL_USER]    = { "USER",     0, SCOLS_FL_RIGHT, N_("user of the process") },
	[COL_UID]     = { "UID",      0, SCOLS_FL_RIGHT, N_("user ID number") },
	/* DEVICE */
	/* SIZE/OFF */
	/* NODE */
};

static int columns[ARRAY_SIZE(infos) * 2] = {-1};
static size_t ncolumns;

struct lsfd_control {
	struct libscols_table *tb;		/* output */

	unsigned int noheadings : 1,
		raw : 1,
		json : 1;
};

static int column_name_to_id(const char *name, size_t namesz)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(infos); i++) {
		const char *cn = infos[i].name;

		if (!strncasecmp(name, cn, namesz) && !*(cn + namesz))
			return i;
	}
	warnx(_("unknown column: %s"), name);

	return -1;
}

static int get_column_id(int num)
{
	assert(num >= 0);
	assert((size_t) num < ncolumns);
	assert(columns[num] < (int) ARRAY_SIZE(infos));

	return columns[num];
}

static const struct colinfo *get_column_info(int num)
{
	return &infos[ get_column_id(num) ];
}

static struct proc *make_proc(pid_t pid)
{
	struct proc *proc = xcalloc(1, sizeof(*proc));

	proc->pid = pid;
	proc->command = NULL;

	return proc;
}

static void free_file(struct file *file)
{
	const struct file_class *class = file->class;

	while (class) {
		if (class->free_content)
			class->free_content(file);
		class = class->super;
	}
	free(file);
}

static void free_proc(struct proc *proc)
{
	list_free(&proc->files, struct file, files, free_file);

	free(proc->command);
	free(proc);
}

static void enqueue_proc(struct list_head *procs, struct proc * proc)
{
	INIT_LIST_HEAD(&proc->procs);
	list_add_tail(&proc->procs, procs);
}

static void collect_procs(DIR *dirp, struct list_head *procs)
{
	struct dirent *dp;
	long num;

	while ((dp = readdir(dirp))) {
		struct proc *proc;

		/* care only for numerical entries.
		 * For a non-numerical entry, strtol returns 0.
		 * We can skip it because there is no task having 0 as pid. */
		if (!(num = strtol(dp->d_name, (char **) NULL, 10)))
			continue;

		proc = make_proc((pid_t)num);
		enqueue_proc(procs, proc);
	}
}

static void run_collectors(struct list_head *procs)
{
	pthread_t collectors[NUM_COLLECTORS];

	for (int i = 0; i < NUM_COLLECTORS; i++) {
		errno = pthread_create(collectors + i, NULL, fill_procs, procs);
		if (errno)
			err(EXIT_FAILURE, _("failed to create a thread"));
	}

	errno = pthread_mutex_lock(&procs_ready_lock);
	if (errno != 0)
		err(EXIT_FAILURE, _("failed to lock a mutex"));

	current_proc = procs->next;

	errno = pthread_mutex_unlock(&procs_ready_lock);
	if (errno != 0)
		err(EXIT_FAILURE, _("failed to unlock a mutex"));

	errno = pthread_cond_broadcast(&procs_ready);
	if (errno != 0)
		err(EXIT_FAILURE, _("failed to broadcast a condvar"));

	for (int i = 0; i < NUM_COLLECTORS; i++)
		pthread_join(collectors[i], NULL);
}

static void collect(struct list_head *procs)
{
	DIR *dirp;

	dirp = opendir("/proc");
	if (!dirp)
		err(EXIT_FAILURE, _("failed to open /proc"));
	collect_procs(dirp, procs);
	closedir(dirp);

	run_collectors(procs);
}

static struct file *collect_file(int dd, struct dirent *dp)
{
	long num;
	char *endptr = NULL;
	struct stat sb;
	ssize_t len;
	char sym[PATH_MAX];

	/* care only for numerical descriptors */
	num = strtol(dp->d_name, &endptr, 10);
	if (num == 0 && endptr == dp->d_name)
		return NULL;

	if (fstatat(dd, dp->d_name, &sb, 0) < 0)
		return NULL;

	memset(sym, 0, sizeof(sym));
	if ((len = readlinkat(dd, dp->d_name, sym, sizeof(sym) - 1)) < 0)
		return NULL;

	switch (sb.st_mode & S_IFMT) {
	case S_IFREG:
		return make_regular_fd_file(NULL, &sb, sym, (int)num);
	}

	return make_fd_file(NULL, &sb, sym, (int)num);
}

static void enqueue_file(struct proc *proc, struct file * file)
{
	INIT_LIST_HEAD(&file->files);
	list_add_tail(&file->files, &proc->files);
}

static void collect_files(struct proc *proc)
{
	DIR *dirp;
	int dd;
	struct dirent *dp;

	dirp = opendirf("/proc/%d/fd/", proc->pid);
	if (!dirp)
		return;

	if ((dd = dirfd(dirp)) < 0 )
		return;

	while ((dp = xreaddir(dirp))) {
		struct file *file;

		if ((file = collect_file(dd, dp)) == NULL)
			continue;

		enqueue_file(proc, file);
	}
	closedir(dirp);
}

static void fill_proc(struct proc *proc)
{
	INIT_LIST_HEAD(&proc->files);

	proc->command = proc_get_command_name(proc->pid);
	if (!proc->command)
		err(EXIT_FAILURE, _("failed to get command name"));

	collect_files(proc);
}


static void *fill_procs(void *arg)
{
	struct list_head *procs = arg;
	struct list_head *target_proc;

	errno = pthread_mutex_lock(&procs_ready_lock);
	if (errno != 0)
		err(EXIT_FAILURE, _("failed to lock a mutex"));

	if (current_proc == NULL) {
		errno = pthread_cond_wait(&procs_ready, &procs_ready_lock);
		if (errno != 0)
			err(EXIT_FAILURE, _("failed to wait a condvar"));
	}

	errno = pthread_mutex_unlock(&procs_ready_lock);
	if (errno != 0)
		err(EXIT_FAILURE, _("failed to unlock a mutex"));

	while (1) {
		errno = pthread_mutex_lock(&procs_consumer_lock);
		if (errno != 0)
			err(EXIT_FAILURE, _("failed to lock a mutex"));

		target_proc = current_proc;

		if (current_proc != procs)
			current_proc = current_proc->next;

		errno = pthread_mutex_unlock(&procs_consumer_lock);
		if (errno != 0)
			err(EXIT_FAILURE, _("failed to lock a mutex"));

		if (target_proc == procs) {
			/* All pids are processed. */
			break;
		}

		fill_proc(list_entry(target_proc, struct proc, procs));
	}

	return NULL;
}

static void fill_column(struct proc *proc,
			struct file *file,
			struct libscols_line *ln,
			int column_id,
			size_t column_index)
{
	const struct file_class *class = file->class;

	while (class) {
		if (class->fill_column
		    && class->fill_column(proc, file, ln,
					  column_id, column_index))
			break;
		class = class->super;
	}
}

static void convert1(struct proc *proc,
		     struct file *file,
		     struct libscols_line *ln)

{
	for (size_t i = 0; i < ncolumns; i++)
		fill_column(proc, file, ln, get_column_id(i), i);
}

static void convert(struct list_head *procs, struct lsfd_control *ctl)
{
	struct list_head *p;

	list_for_each (p, procs) {
		struct proc *proc = list_entry(p, struct proc, procs);
		struct list_head *f;

		list_for_each (f, &proc->files) {
			struct file *file = list_entry(f, struct file, files);
			struct libscols_line *ln = scols_table_new_line(ctl->tb, NULL);
			if (!ln)
				err(EXIT_FAILURE, _("failed to allocate output line"));
			convert1(proc, file, ln);
		}
	}
}

static void delete(struct list_head *procs, struct lsfd_control *ctl)
{
	list_free(procs, struct proc, procs, free_proc);

	scols_unref_table(ctl->tb);
}

static void emit(struct lsfd_control *ctl)
{
	scols_print_table(ctl->tb);
}

static void __attribute__((__noreturn__)) usage(void)
{
	FILE *out = stdout;
	size_t i;

	fputs(USAGE_HEADER, out);
	fprintf(out, _(" %s [options]\n"), program_invocation_short_name);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -J, --json            use JSON output format\n"), out);
	fputs(_(" -n, --noheadings      don't print headings\n"), out);
	fputs(_(" -o, --output <list>   output columns\n"), out);
	fputs(_(" -r, --raw             use raw output format\n"), out);

	fputs(USAGE_SEPARATOR, out);
	printf(USAGE_HELP_OPTIONS(23));

	fprintf(out, USAGE_COLUMNS);

	for (i = 0; i < ARRAY_SIZE(infos); i++)
		fprintf(out, " %11s  %s\n", infos[i].name, _(infos[i].help));

	printf(USAGE_MAN_TAIL("lsfd(1)"));

	exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
	int c;
	char *outarg = NULL;

	struct list_head procs;

	struct lsfd_control ctl = {};

	static const struct option longopts[] = {
		{ "noheadings", no_argument, NULL, 'n' },
		{ "output",     required_argument, NULL, 'o' },
		{ "version",    no_argument, NULL, 'V' },
		{ "help",	no_argument, NULL, 'h' },
		{ "json",       no_argument, NULL, 'J' },
		{ "raw",        no_argument, NULL, 'r' },
		{ NULL, 0, NULL, 0 },
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	close_stdout_atexit();

	while ((c = getopt_long(argc, argv, "no:JrVh", longopts, NULL)) != -1) {
		switch (c) {
		case 'n':
			ctl.noheadings = 1;
			break;
		case 'o':
			outarg = optarg;
			break;
		case 'J':
			ctl.json = 1;
			break;
		case 'r':
			ctl.raw = 1;
			break;
		case 'V':
			print_version(EXIT_SUCCESS);
		case 'h':
			usage();
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	if (!ncolumns) {
		columns[ncolumns++] = COL_COMMAND;
		columns[ncolumns++] = COL_PID;
		columns[ncolumns++] = COL_USER;
		columns[ncolumns++] = COL_FD;
		columns[ncolumns++] = COL_TYPE;
		columns[ncolumns++] = COL_NAME;
	}

	if (outarg && string_add_to_idarray(outarg, columns, ARRAY_SIZE(columns),
					    &ncolumns, column_name_to_id) < 0)
		return EXIT_FAILURE;

	username_cache = new_idcache();
	if (!username_cache)
		err(EXIT_FAILURE, _("failed to allocate UID cache"));

	scols_init_debug(0);
	ctl.tb = scols_new_table();

	if (!ctl.tb)
		err(EXIT_FAILURE, _("failed to allocate output table"));

	scols_table_enable_noheadings(ctl.tb, ctl.noheadings);
	scols_table_enable_raw(ctl.tb, ctl.raw);
	scols_table_enable_json(ctl.tb, ctl.json);
	if (ctl.json)
		scols_table_set_name(ctl.tb, "lsfd");

	for (size_t i = 0; i < ncolumns; i++) {
		const struct colinfo *col = get_column_info(i);
		struct libscols_column *cl;

		cl = scols_table_new_column(ctl.tb, col->name, col->whint, col->flags);
		if (!cl)
			err(EXIT_FAILURE, _("failed to allocate output column"));

		if (ctl.json) {
			int id = get_column_id(i);

			switch (id) {
			case COL_NAME:
			case COL_COMMAND:
			case COL_TYPE:
			case COL_USER:
				scols_column_set_json_type(cl, SCOLS_JSON_STRING);
				break;
			case COL_PID:
			case COL_FD:
			case COL_UID:
				/* fallthrough */
			default:
				scols_column_set_json_type(cl, SCOLS_JSON_NUMBER);
				break;
			}
		}
	}

	INIT_LIST_HEAD(&procs);
	collect(&procs);

	convert(&procs, &ctl);
	emit(&ctl);
	delete(&procs, &ctl);

	free_idcache(username_cache);

	return 0;
}

DIR *opendirf(const char *format, ...)
{
	va_list ap;
	char path[PATH_MAX];

	memset(path, 0, sizeof(path));

	va_start(ap, format);
	vsprintf(path, format, ap);
	va_end(ap);

	return opendir(path);
}