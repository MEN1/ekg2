/* $Id$ */

/*
 *  (C) Copyright 2001-2005 Wojtek Kaniewski <wojtekka@irc.pl>
 *                          Robert J. Wo�ny <speedy@ziew.org>
 *                          Pawe� Maziarz <drg@infomex.pl>
 *                          Wojciech Bojdo� <wojboj@htc.net.pl>
 *                          Piotr Wysocki <wysek@linux.bydg.org>
 *                          Dawid Jarosz <dawjar@poczta.onet.pl>
 *                          Piotr Domagalski <szalik@szalik.net>
 *                          Kuba Kowalski <qbq@kofeina.net>
 *			    Piotr Kupisiewicz <deli@rzepaknet.us>
 *			    Leszek Krupi�ski <leafnode@wafel.com>
 *			    Adam Mikuta <adamm@ekg2.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License Version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "ekg2-config.h"
#include "win32.h"

#ifndef __FreeBSD__
#define _XOPEN_SOURCE 600
#define __EXTENSIONS__
#endif

#define _BSD_SOURCE

#include <sys/types.h>

#ifndef NO_POSIX_SYSTEM
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

#include <sys/stat.h>
#include <sys/time.h>

#ifndef NO_POSIX_SYSTEM
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef NO_POSIX_SYSTEM
#include <sys/utsname.h>
#endif

#ifdef __sun
#include <procfs.h>
#endif

#ifdef __FreeBSD__
# include <kvm.h>		/* kvm_ funcs */
# include <limits.h>		/* _POSIX2_LINE_MAX */
# include <sys/param.h>
# include <sys/sysctl.h>	/* KERN_PROC_PID */
# include <sys/user.h>
#endif

#include "char.h"
#include "commands.h"
#include "debug.h"
#include "events.h"
#include "configfile.h"
#include "dynstuff.h"
#include "log.h"
#include "metacontacts.h"
#include "msgqueue.h"
#include "protocol.h"
#include "sessions.h"
#ifndef HAVE_STRLCAT
#  include "compat/strlcat.h"
#endif
#ifndef HAVE_STRLCPY
#  include "compat/strlcpy.h"
#endif
#include "stuff.h"
#include "themes.h"
#include "vars.h"
#include "userlist.h"
#include "scripts.h"
#include "windows.h"
#include "xmalloc.h"

#define unpar(x) normal_to_wcs(x)

char *send_nicks[SEND_NICKS_MAX] = { NULL };
int send_nicks_count = 0, send_nicks_index = 0;
static int quit_command = 0;

list_t commands = NULL;

/*
 * match_arg()
 *
 * sprawdza, czy dany argument funkcji pasuje do podanego.
 */
int match_arg(const CHAR_T *arg, char shortopt, const CHAR_T *longopt, int longoptlen)
{
	if (!arg || *arg != '-')
		return 0;

	arg++;
	if (*arg == '-') {
		int len = xwcslen(++arg);

		if (longoptlen > len)
			len = longoptlen;

		return !xwcsncmp(arg, longopt, len);
	}
	return (*arg == shortopt) && (*(arg + 1) == 0);
}

int nmatch_arg(const char *arg, char shortopt, const CHAR_T *longopt, int longoptlen) {
	CHAR_T *uarg = normal_to_wcs(arg);
	int ret = match_arg(uarg, shortopt, longopt, longoptlen);
	free_utf(uarg);
	return ret;
}

/*
 * tabnick_add()
 *
 * dodaje do listy nick�w dope�nianych automagicznie tabem.
 */
void tabnick_add(const char *nick)
{
	int i;

	for (i = 0; i < send_nicks_count; i++)
		if (send_nicks[i] && !xstrcmp(nick, send_nicks[i])) {
			tabnick_remove(nick);
			break;
		}

	if (send_nicks_count == SEND_NICKS_MAX) {
		xfree(send_nicks[SEND_NICKS_MAX - 1]);
		send_nicks_count--;
	}

	for (i = send_nicks_count; i > 0; i--)
		send_nicks[i] = send_nicks[i - 1];

	if (send_nicks_count != SEND_NICKS_MAX)
		send_nicks_count++;
	
	send_nicks[0] = xstrdup(nick);
}

/*
 * tabnick_remove()
 *
 * usuwa z listy dope�nianych automagicznie tabem.
 */
void tabnick_remove(const char *nick)
{
	int i, j;

	for (i = 0; i < send_nicks_count; i++) {
		if (send_nicks[i] && !xstrcmp(send_nicks[i], nick)) {
			xfree(send_nicks[i]);

			for (j = i + 1; j < send_nicks_count; j++)
				send_nicks[j - 1] = send_nicks[j];

			send_nicks_count--;
			send_nicks[send_nicks_count] = NULL;

			break;
		}
	}
}

/*
 * tabnick_flush()
 *
 * czy�ci list� nick�w dope�nianych tabem.
 */
void tabnick_flush()
{
	int i;

	for (i = 0; i < send_nicks_count; i++) {
		xfree(send_nicks[i]);
		send_nicks[i] = NULL;
	}

	send_nicks_count = 0;
	send_nicks_index = 0;
}

/*
 * command_find()
 *
 * szuka podanej komendy.
 */
command_t *command_find(const CHAR_T *name)
{
        list_t l;

        if (!name)
                return NULL;
        for (l = commands; l; l = l->next) {
                command_t *c = l->data;

                if (!xwcscasecmp(c->name, name)) {
                        return c;
		}
        }
        return NULL;
}


COMMAND(cmd_tabclear)
{
	PARASC
	int i;

	if (!params[0]) {
		tabnick_flush();
		return 0;
	}

	if (nmatch_arg(params[0], 'o', TEXT("offline"), 2)) {
		for (i = 0; i < send_nicks_count; i++) {
			userlist_t *u = NULL;

			if (send_nicks[i])
				u = userlist_find(session, send_nicks[i]);

			if (!u || xstrcasecmp(u->status, EKG_STATUS_NA))
				continue;

			tabnick_remove(send_nicks[i]);
		}

		return 0;
	}

	wcs_printq("invalid_params", name);

	return -1;
}

COMMAND(cmd_add)
{
	PARASC
	int params_free = 0;	/* zmienili�my params[] i trzeba zwolni� */
	int result = 0;
	userlist_t *u = NULL;
	plugin_t *plug = NULL;
	
	if (!session_current) {
		return -1;
	}
	
	if (params[0] && !params[1] && params[0][0] != '-' && window_current->target) {
		const char *name = params[0], *s1 = params[1], *s2 = params[2];
		params_free = 1;
		params = xmalloc(4 * sizeof(char *));
		params[0] = window_current->target;
		params[1] = name;
		params[2] = (s1) ? saprintf("%s %s", s1, ((s2) ? s2 : "")) : NULL;
		params[3] = NULL;
	}

	if (params[0] && nmatch_arg(params[0], 'f', TEXT("find"), 2)) {
		int nonick = 0;
		char *nickname, *tmp;

		if (!last_search_uid || !last_search_nickname) {
			wcs_printq("search_no_last");
			return -1;
		}

		tmp = strip_spaces(last_search_nickname);

		if ((nonick = !xstrcmp(tmp, "")) && !params[1]) {
			wcs_printq("search_no_last_nickname");
			return -1;
		}

		if (nonick || params[1])
			nickname = (char *) params[1];
		else
			nickname = tmp;

		params_free = 1;

		params = xmalloc(4 * sizeof(char *));
		params[0] = last_search_uid;
		params[1] = nickname;
		params[2] = saprintf("-f \"%s\" -l \"%s\"", ((last_search_first_name) ? last_search_first_name : ""), ((last_search_last_name) ? last_search_last_name : ""));
		params[3] = NULL;
	}

	if (!params[0] || !params[1]) {
		wcs_printq("not_enough_params", name);
		result = -1;
		goto cleanup;
	}

	if (!(plug=plugin_find_uid(params[0]))) {
		wcs_printq("invalid_uid");
		result = -1;
		goto cleanup;
	}

	if (plug != plugin_find_uid(session_current->uid)) {
		wcs_printq("invalid_uid");
		result = -1;
		goto cleanup;
	}

	if (!valid_nick(params[1])) {
		wcs_printq("invalid_nick");
		result = -1;
		goto cleanup;
	}

	if (((u = userlist_find(session_current, params[0])) && u->nickname) || ((u = userlist_find(session_current, params[1])) && u->nickname)) {
		if (!xstrcasecmp(params[1], u->nickname) && !xstrcasecmp(params[0], u->uid))
			printq("user_exists", params[1], session_name(session_current));
		else
			printq("user_exists_other", params[1], format_user(session_current, u->uid), session_name(session_current));

		result = -1;
		goto cleanup;
	}

	/* kto� by� tylko ignorowany/blokowany, nadajmy mu nazw� */
	if (u) {
		xfree(u->nickname);
		u->nickname = xstrdup(params[1]);
	}

	if (u || userlist_add(session_current, params[0], params[1])) {
		char *uid = xstrdup(params[0]);

		query_emit(NULL, "userlist-added", &uid, &params[1], &quiet);
                query_emit(NULL, "add-notify", &session_current->uid, &uid);
                xfree(uid);

		printq("user_added", params[1], session_name(session_current));

		tabnick_remove(params[0]);
		config_changed = 1;

	}

cleanup:
	if (params_free) {
		xfree((char*) params[2]);
		xfree(params);
	}

	return result;
}

COMMAND(cmd_alias)
{
	PARASC
	CHAR_T *sname;
	if (nmatch_arg(params[0], 'a', TEXT("add"), 2)) {
		if (!params[1] || !xstrchr(params[1], ' ')) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (!alias_add((sname = normal_to_wcs(params[1])), quiet, 0)) {
			config_changed = 1;
			free_utf(sname);
			return 0;
		}
		free_utf(sname);

		return -1;
	}

	if (nmatch_arg(params[0], 'A', TEXT("append"), 2)) {
		if (!params[1] || !xstrchr(params[1], ' ')) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (!alias_add((sname = normal_to_wcs(params[1])), quiet, 1)) {
			config_changed = 1;
			free_utf(sname);
			return 0;
		}
		free_utf(sname);

		return -1;
	}

	if (nmatch_arg(params[0], 'd', TEXT("del"), 2)) {
		int ret;

		if (!params[1]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (!xstrcmp(params[1], "*"))
			ret = alias_remove(NULL, quiet);
		else {
			ret = alias_remove((sname = normal_to_wcs(params[1])), quiet);
			free_utf(sname);
		}

		if (!ret) {
			config_changed = 1;
			return 0;
		}

		return -1;
	}
	
	if (!params[0] || nmatch_arg(params[0], 'l', TEXT("list"), 2) || params[0][0] != '-') {
		list_t l;
		int count = 0;
		CHAR_T *aname = NULL;

		if (params[0] && nmatch_arg(params[0], 'l', TEXT("list"), 2))
			aname = normal_to_wcs(params[1]);
		else if (params[0])
			aname = normal_to_wcs(params[0]);

		for (l = aliases; l; l = l->next) {
			struct alias *a = l->data;
			list_t m;
			int first = 1, i;
			CHAR_T *tmp;
			
			if (aname && xwcscasecmp(aname, a->name))
				continue;

			tmp = xcalloc(xwcslen(a->name) + 1, sizeof(CHAR_T));

			for (i = 0; i < xwcslen(a->name); i++)
				xwcscat(tmp, TEXT(" "));

			for (m = a->commands; m; m = m->next) {
				wcs_printq((first) ? "aliases_list" : "aliases_list_next", a->name, (CHAR_T *) m->data, tmp);
				first = 0;
				count++;
			}

			xfree(tmp);
		}

		if (!count) {
			if (aname) {
				wcs_printq("aliases_noexist", aname);
				return -1;
			}

			wcs_printq("aliases_list_empty");
		}
		free_utf(aname);

		return 0;
	}

	wcs_printq("invalid_params", name);

	return -1;
}

COMMAND(cmd_status)
{
	PARASC
        struct tm *t;
        time_t n;
        int now_days;
	char buf1[100];
	const char *format;
	session_t *s = NULL;

        wcs_printq("show_status_header");

	if (!params[0] && session)
		query_emit(plugin_find_uid(session->uid), "status-show", &session->uid);
	else if (!(s = session_find(params[0]))) {
		printq("invalid_uid", params[0]);
		return -1;
	} else
		query_emit(plugin_find_uid(s->uid), "status-show", &s->uid);

        n = time(NULL);
        t = localtime(&n);
        now_days = t->tm_yday;

        t = localtime(&ekg_started);
	format = format_find((t->tm_yday == now_days) ? "show_status_ekg_started_today" : "show_status_ekg_started");
        if (!strftime(buf1, sizeof(buf1), format, t) && xstrlen(format)>0)
		xstrcpy(buf1, "TOOLONG");

        printq("show_status_ekg_started_since", buf1);
        wcs_printq("show_status_footer");

	return 0;
}

COMMAND(cmd_del)
{
	PARASC
	userlist_t *u;
	char *tmp;
	int del_all = ((params[0] && !xstrcmp(params[0], "*")) ? 1 : 0);

	if (!session)
		return -1;
	
	if (!params[0]) {
		wcs_printq("not_enough_params", name);
		return -1;
	}

	if (del_all) {
		list_t l;
		for (l = session->userlist; l; ) {
			userlist_t *u = l->data;
			char *tmp;
	
			l = l->next;

			tmp = xstrdup(u->uid);
			query_emit(NULL, "userlist-removed", &tmp);
			xfree(tmp);

			userlist_remove(session, u);
		}

		printq("user_cleared_list", session_name(session));
		tabnick_flush();
		config_changed = 1;

		return 0;
	}

	if (!(u = userlist_find(session, params[0])) || !u->nickname) {
		printq("user_not_found", params[0]);
		return -1;
	}

	tmp = xstrdup(u->uid);
	query_emit(NULL, "userlist-removed", &params[0], &tmp);
	query_emit(NULL, "remove-notify", &session->uid, &tmp);

        printq("user_deleted", params[0], session_name(session));
	xfree(tmp);

	tabnick_remove(u->uid);
	tabnick_remove(u->nickname);

	config_changed = 1;

	userlist_remove(session, u);
	
	return 0;
}

typedef struct {
	char *target;		/* je�li wysy�amy wynik, to do kogo? */
	char *session;		/* jaka sesja? */
	int quiet;		/* czy mamy by� cicho? */
	wcs_string_t buf;	/* je�li buforujemy, to tutaj */
} cmd_exec_info_t;

WATCHER(cmd_exec_watch_handler)	/* sta�y */
{
	cmd_exec_info_t *i = data;
	int quiet = (i) ? i->quiet : 0;

	if (!i)
		return -1;

	if (type == 1) {
		if (i->buf) {
			string_insert(i->buf, 0, TEXT("/ ")); /* UUU */
			command_exec(i->target, session_find(i->session), i->buf->str, quiet);
			wcs_string_free(i->buf, 1);
		}
		xfree(i->target);
		xfree(i->session);
		xfree(i);
		return 0;
	}

	if (!i->target) {
		printq("exec", watch);
		return 0;
	}

	if (i->buf) {
		CHAR_T *watch_wcs = normal_to_wcs(watch);
		wcs_string_append(i->buf, watch_wcs);
		wcs_string_append(i->buf, TEXT("\r\n"));
		free_utf(watch_wcs);
	} else {
		command_exec_format(i->target, session_find(i->session), quiet, TEXT("/ %s"), watch);
	}
	return 0;
}

void cmd_exec_child_handler(child_t *c, int pid, const char *name, int status, void *priv)
{
	int quiet = (name && name[0] == '^');

	wcs_printq("process_exit", wcs_itoa(pid), name, wcs_itoa(status));
}

COMMAND(cmd_exec)
{
	PARASC
	list_t l;
	int pid;

	if (params[0]) {
		int fd[2] = { 0, 0 }, buf = 0, add_commandline = 0;
		const char *command = params[0], *target = NULL;
		char **args = NULL;
		cmd_exec_info_t *i;
		watch_t *w;

		if (params[0][0] == '-') {
			int big_match = 0;
			args = (char **) params;

			if (nmatch_arg(args[0], 'M', TEXT("MSG"), 2) || (buf = nmatch_arg(args[0], 'B', TEXT("BMSG"), 2)))
				big_match = add_commandline = 1;

			if (big_match || nmatch_arg(args[0], 'm', TEXT("msg"), 2) || (buf = nmatch_arg(args[0], 'b', TEXT("bmsg"), 2))) {
				const char *uid;

				if (!args[1] || !args[2]) {
					wcs_printq("not_enough_params", name);
					return -1;
				}

				if (!(uid = get_uid(session, args[1]))) {
					printq("user_not_found", args[1]);
					return -1;
				}

				target = uid;
				command = args[2];
			} else {
				wcs_printq("invalid_params", name);
				return -1;
			}
		} 

		if (pipe(fd) == -1) {
			printq("exec_error", strerror(errno));
			return -1;
		}

#ifndef NO_POSIX_SYSTEM
		if (!(pid = fork())) {
			dup2(open("/dev/null", O_RDONLY), 0);
			dup2(fd[1], 1);
			dup2(fd[1], 2);

			close(fd[0]);
			close(fd[1]);

			execl("/bin/sh", "sh", "-c", (command[0] == '^') ? command + 1 : command, (void *) NULL);

			exit(1);
		}
#else
/* XXX, fork and execute cmd /C $command */
		pid = -1;
#endif

		if (pid < 0) {
			printq("exec_error", strerror(errno));
			close(fd[0]);
			close(fd[1]);
			return -1;
		}
	
		i = xmalloc(sizeof(cmd_exec_info_t));
		
		i->quiet = quiet;
		i->target = xstrdup(target);
		i->session = xstrdup(session_uid_get(session));

		if (buf)
			i->buf = wcs_string_init(NULL);

		w = watch_add(NULL, fd[0], WATCH_READ_LINE, cmd_exec_watch_handler, i);

		if (add_commandline) {
			char *tmp = format_string(format_find("exec_prompt"), ((command[0] == '^') ? command + 1 : command));
			string_append(w->buf, tmp);
			xfree(tmp);
		}

		fcntl(fd[0], F_SETFL, O_NONBLOCK);

		close(fd[1]);
		
		child_add(NULL, pid, command, cmd_exec_child_handler, NULL);
			
	} else {
		for (l = children; l; l = l->next) {
			child_t *c = l->data;
			
			wcs_printq("process", wcs_itoa(c->pid), ((c->name) ? (c->name) : TEXT("?")));
		}

		if (!children) {
			wcs_printq("no_processes");
			return -1;
		}
	}

	return 0;
}

COMMAND(cmd_eval)
{
	PARUNI
	int i;
	CHAR_T **argv;

	argv = wcs_array_make(params[0], TEXT(" "), 0, 1, 1);
	
	for (i = 0; argv[i]; i++) {
		command_exec(NULL, session, argv[i], 0);
	}

	wcs_array_free(argv);

	return 0;
}

COMMAND(cmd_for)
{
	PARASC
	char *nname = wcs_to_normal(name);
	int for_all = 0, ret = 0;

	if (!xstrcmp(params[1], "*")) 
		for_all = 1; 

	if (nmatch_arg(params[0], 's', TEXT("sessions"), 2)) {
		char *param = (char *) params[2];
		int next_is_for = 0;

		if (param[0] == '/')
			param++;

		if (!xstrncasecmp(param, nname, xstrlen(nname)))
			next_is_for = 1;

		if (for_all) {
			list_t l;
			
			if (!sessions) {
				wcs_printq("session_list_empty");
				ret = -2;
				goto for_end;
			}
				
			for (l = sessions; l; l = l->next) {
				session_t *s = l->data;
				CHAR_T *ffor_command;
				char *for_command;
				
				if (!s || !s->uid)
					continue;
				
				if (!next_is_for)
					for_command = format_string(params[2], session_alias_uid(s), s->uid);
				else
					for_command = xstrdup(params[2]);

				ffor_command = normal_to_wcs(for_command);
				command_exec(NULL, s, ffor_command, 0);
				xfree(for_command);
				free_utf(ffor_command);
			}
		} else {
			char **tmp = array_make(params[1], ",", 0, 0, 0);
			int i;
			session_t **s;
	
			s = xmalloc(sizeof(session_t *) * array_count(tmp));
			
			/* first we are checking all of the parametrs */
			for (i = 0; tmp[i]; i++) {
				if (!(s[i] = session_find(tmp[i]))) {
					printq("session_doesnt_exist", tmp[i]);
					ret = -1;
					xfree(s);
					goto for_end;
				}
			}		
			
			for (i = 0; tmp[i]; i++) {
				char *for_command;
				
				if (!s[i] || !s[i]->uid)
					continue;
				
				if (!next_is_for)
					for_command = format_string(params[2], session_alias_uid(s[i]), s[i]->uid);
				else
					for_command = xstrdup(params[2]);

				command_exec(NULL, s[i], for_command, 0);
				xfree(for_command);
			}
	
			xfree(s);
		}
	} else if (nmatch_arg(params[0], 'u', TEXT("users"), 2)) {
		char *param = (char *) params[2];
                int next_is_for = 0;

                if (param[0] == '/')
                        param++;

                if (!xstrncasecmp(param, nname, xstrlen(nname)))
                        next_is_for = 1;
				
		if (!session) {
			ret = -1;
			goto for_end;
		}

		if (!session->userlist) {
			wcs_printq("list_empty");
			ret = -1;
			goto for_end;
		}

		if (for_all) {
			list_t l;

			for (l = session->userlist; l; l = l->next) {
				userlist_t *u = l->data;
				char *for_command;

				if (!u || !u->uid)
					continue;
				
				if (!next_is_for)
					for_command = format_string(params[2], (u->nickname) ? u->nickname : u->uid, u->uid);
				else
					for_command = xstrdup(params[2]);

				command_exec(NULL, session, for_command, 0);
				xfree(for_command);
			}
				
		} else {
                        char **tmp = array_make(params[1], ",", 0, 0, 0);
                        int i;
                        userlist_t **u;

                        u = xmalloc(sizeof(userlist_t *) * array_count(tmp));

                        /* first we are checking all of the parametrs */
                        for (i = 0; tmp[i]; i++) {
                                if (!(u[i] = userlist_find(session, tmp[i]))) {
                                        printq("user_not_found", tmp[i]);
                                        ret = -1;
                                        xfree(u);
                                        goto for_end;
                                }
                        }

                        for (i = 0; tmp[i]; i++) {
                                char *for_command;

                                if (!u[i] || !u[i]->uid)
                                        continue;

				if (!next_is_for)
	                                for_command = format_string(params[2], (u[i]->nickname) ? u[i]->nickname : u[i]->uid, u[i]->uid);
				else
					for_command = xstrdup(params[2]);

                                command_exec(NULL, session, for_command, 0);
                                xfree(for_command);
                        }

                        xfree(u);
                }
	} else if (nmatch_arg(params[0], 'w', TEXT("windows"), 2)) {
                char *param = (char *) params[2];
                int next_is_for = 0;

                if (param[0] == '/')
                        param++;

                if (!xstrncasecmp(param, nname, xstrlen(nname)))
                        next_is_for = 1;

                if (!windows) {
                        ret = -1;
                        goto for_end;
                }

                if (for_all) {
                        list_t l;

                        for (l = windows; l; l = l->next) {
                        	window_t *w = l->data;
				CHAR_T *ffor_command;
                                char *for_command;

                                if (!w || !w->target || !w->session)
                                        continue;

                                if (!next_is_for)
                                        for_command = format_string(params[2], get_nickname(w->session, w->target), get_uid(w->session, w->target));
                                else
                                        for_command = xstrdup(params[2]);

				ffor_command = normal_to_wcs(for_command);
                                command_exec(NULL, session, ffor_command, 0);
                                xfree(for_command);
				free_utf(ffor_command);
			}
                 } else {
                       	char **tmp = array_make(params[1], ",", 0, 0, 0);
	                int i;
	                window_t **w;

                        w = xmalloc(sizeof(window_t *) * array_count(tmp));

                        /* first we are checking all of the parametrs */
                        for (i = 0; tmp[i]; i++) {
				list_t l;
				int found = 0;

				for (l = windows; l; l = l->next) {
					window_t *wp = l->data;

					if (!wp)
						continue;
					
					if (atoi(tmp[i]) == wp->id) {
						found = 1;
						w[i] = wp;
						break;
					}
				}
			
				if (!found) {
		                        printq("window_doesnt_exist", tmp[i]);
		                        ret = -1;	
                                        xfree(w);
       	                                goto for_end;
				}
                        }

                        for (i = 0; tmp[i]; i++) {
                                char *for_command;

                                if (!w[i] || !w[i]->target || !w[i]->session)
                                        continue;

                                if (!next_is_for)
                                        for_command = format_string(params[2], get_nickname(w[i]->session, w[i]->target), get_uid(w[i]->session, w[i]->target));
                                else
                                        for_command = xstrdup(params[2]);

                                command_exec(NULL, session, for_command, 0);
                                xfree(for_command);
                        }

                        xfree(w);
                }
	} else {
	        wcs_printq("invalid_params", name);
	        ret = -1;
	}
	
for_end:
	free_utf(nname);
	return ret;
}

COMMAND(cmd_help)
{
	PARUNI
	list_t l;
	if (params[0]) {
		const CHAR_T *p = (params[0][0] == '/' && xwcslen(params[0]) > 1) ? params[0] + 1 : params[0];
		int plen;

		if (!xwcscasecmp(p, TEXT("set")) && params[1]) {
			if (!quiet)
				variable_help(params[1]);
			return 0;
		}

                if (!xwcscasecmp(p, TEXT("session")) && params[1]) {
                        if (!quiet)
                                session_help(session, params[1]);
                        return 0;
                }

        	if (session && session->uid) {
	                plen = (int)(xstrchr(session->uid, ':') - session->uid) + 1;
		} else
			plen = 0;
	
		for (l = commands; l; l = l->next) {
			command_t *c = l->data;
			
			if (!xwcscasecmp(c->name, p) && (c->flags & COMMAND_ISALIAS)) {
				printq("help_alias", p);
				return -1;
			}
			if (!xwcscasecmp(c->name, p) && (c->flags & COMMAND_ISSCRIPT)) {
				printq("help_script", p);
				return -1;
			}

			if ((!xwcscasecmp(c->name, p) || !xwcscasecmp(c->name + plen, p)) && !(c->flags & COMMAND_ISALIAS) ) {
				FILE *f; 
				CHAR_T *line, *params_help = NULL, *params_help_s, *brief = NULL, *tmp = NULL;
				const CHAR_T *seeking_name;
				wcs_string_t s;
				int found = 0;

				if (c->plugin && c->plugin->name) {
                                        char *tmp = help_path("commands", c->plugin->name);
					CHAR_T *tmp2;
					f = fopen(tmp, "r");
				        xfree(tmp);

			                if (!f) {
                                                wcs_print("help_command_file_not_found_plugin", c->plugin->name);
						return -1;
			                }
					tmp2 = xwcschr(c->name, ':');
					if (!tmp2)
						seeking_name = c->name;
					else
				                seeking_name = tmp2 + 1;
			        } else {
					char *tmp = help_path("commands", NULL);
					f = fopen(tmp, "r");
					xfree(tmp);

			                if (!f) {
		                	        wcs_print("help_command_file_not_found");
						return -1;
			                }

	                		seeking_name = c->name;
        			}
			        while ((line = wcs_read_file(f))) {
			                if (!xwcscasecmp(line, seeking_name)) {
			                        found = 1;
			                        xfree(line);
			                        break;
			                }

			                xfree(line);
			        }

			        if (!found) {
			                fclose(f);
			                wcs_print("help_command_not_found", c->name);
			                return -1;
			        }

			        line = wcs_read_file(f);

			        if ((tmp = xwcsstr(line, TEXT(": "))))
			                params_help = xwcsdup(tmp + 2);
			        else
			                params_help = xwcsdup(TEXT(""));

				params_help_s = wcs_strip_spaces(params_help);

			        xfree(line);

				line = wcs_read_file(f);

                                if ((tmp = xwcsstr(line, TEXT(": "))))
                                        brief = xwcsdup(tmp + 2);
                                else
                                        brief = xwcsdup(TEXT("?"));

                                xfree(line);

				tmp = NULL;

			        if (xwcsstr(brief, TEXT("%")))
			  		tmp = wcs_format_string(brief);

	                        if (!xwcscmp(brief, TEXT(""))) {
	                                xfree(brief);
	                                brief = xwcsdup(TEXT("?"));
	                        }

	                        if (xwcscmp(params_help_s, TEXT("")))
        	                        wcs_printq("help", (c->name) ? (c->name) : TEXT(""), params_help_s, tmp ? tmp : brief, "");
	                        else
	                                wcs_printq("help_no_params", (c->name) ? (c->name) : TEXT(""), tmp ? tmp : brief, TEXT(""));

				xfree(brief);
				xfree(params_help);
				xfree(tmp);

				s = wcs_string_init(NULL);
			        while ((line = wcs_read_file(f))) {
			                if (line[0] != TEXT('\t')) {
			                        xfree(line);
			                        break;
			                }
					
			                if (!xwcsncmp(line, TEXT("\t- "), 3) && xwcscmp(s->str, TEXT(""))) {
			                        wcs_print("help_command_body", line);
			                        wcs_string_clear(s);
			                }
					
					if (!xwcsncmp(line, TEXT("\t"), 1) && xwcslen(line) == 1) {
						wcs_string_append(s, TEXT("\n\r"));
						continue;
					}
					
			                wcs_string_append(s, line + 1);

			                if (line[xwcslen(line) - 1] != TEXT(' '))
			                        wcs_string_append_c(s, TEXT(' '));
			                xfree(line);
			        }
				
				if (xwcscmp(s->str, TEXT(""))) {
					CHAR_T *tmp = wcs_format_string(s->str);
                                        wcs_printq("help_command_body", tmp);
					xfree(tmp);
				}
				fclose(f);
			        wcs_string_free(s, 1);
				return 0;
			}
		}
	}

	for (l = commands; l; l = l->next) {
		command_t *c = l->data;

		if (xisalnum(*c->name) && !(c->flags & COMMAND_ISALIAS)) {
		    	CHAR_T *blah = NULL;
                        FILE *f;
                        CHAR_T *line, *params_help, *params_help_s, *brief, *tmp = NULL;
                        const CHAR_T *seeking_name;
                        int found = 0;

		        if (c->plugin && c->plugin->name) {
	                        char *tmp = help_path("commands", c->plugin->name);
				CHAR_T *tmp2;
                                f = fopen(tmp, "r");
                                xfree(tmp);

                                if (!f) {
					continue;
                                }
				tmp2 = xwcschr(c->name, TEXT(':'));
				if (!tmp2)
					seeking_name = c->name;
				else 
	                                seeking_name = tmp2 + 1;
                        } else {
                                char *tmp = help_path("commands", NULL);
                                f = fopen(tmp, "r");
				xfree(tmp);

				if (!f) {
					continue;
                                }

                                seeking_name = c->name;
                        }

                        while ((line = wcs_read_file(f))) {
                        	if (!xwcscasecmp(line, seeking_name)) {
                                	found = 1;
                                        xfree(line);
                                        break;
                                }

                                xfree(line);
                        }

                        if (!found) {
		                fclose(f);
				continue;
                        }

			line = wcs_read_file(f);
		
			if ((tmp = xwcsstr(line, TEXT(": "))))
                               params_help = xwcsdup(tmp + 2);
                        else
                               params_help = xwcsdup(TEXT(""));

			params_help_s = wcs_strip_spaces(params_help);	

                        xfree(line);

                        line = wcs_read_file(f);

                        if ((tmp = xwcsstr(line, TEXT(": "))))
 	                       brief = xwcsdup(tmp + 2);
                        else
                               brief = xwcsdup(TEXT("?"));

                        xfree(line);

			if (xwcsstr(brief, TEXT("%"))) {
			    	blah = wcs_format_string(brief);
			}

			if (!xwcscmp(brief, TEXT(""))) {
				xfree(brief);
				brief = xwcsdup(TEXT("?"));
			}

			if (xwcscmp(params_help_s, TEXT("")))
				wcs_printq("help", c->name ? (c->name) : TEXT(""), params_help_s, blah ? blah : brief, TEXT(""));
			else
				wcs_printq("help_no_params", (c->name) ? (c->name) : TEXT(""), blah ? blah : brief, TEXT(""));
			xfree(blah);
			xfree(brief);
			xfree(params_help);

			fclose(f);
		}
	}

	wcs_printq("help_footer");
	wcs_printq("help_quick");
	return 0;
}

COMMAND(cmd_ignore)
{
	PARASC
	const char *uid;

	if (*name == 'i' || *name == 'I') {
		int flags, modified = 0;

		if (!params[0]) {
			list_t l;
			int i = 0;
			for (l = session->userlist; l; l = l->next) {
				userlist_t *u = l->data;
				int level;

				if (!(level = ignored_check(session, u->uid)))
					continue;

				i = 1;

				printq("ignored_list", format_user(session, u->uid), ignore_format(level));
			}

			if (!i)
				wcs_printq("ignored_list_empty");

			return 0;
		}

		if (params[0][0] == '#') {
			return command_exec_format(NULL, NULL, quiet, TEXT("/conference --ignore %s"), params[0]);
		}

                if ((flags = ignored_check(session, get_uid(session, params[0]))))
                        modified = 1;

		if (params[1]) {
			int __flags = ignore_flags(params[1]);

			if (!__flags) {
				wcs_printq("invalid_params", name);
				return -1;
			}

                        flags |= __flags;
		} else
			flags = IGNORE_ALL;

		if (!(uid = get_uid(session, params[0]))) {
			printq("user_not_found", params[0]);
			return -1;
		}

		if (modified)
			ignored_remove(session, uid);

                if (!ignored_add(session, uid, flags)) {
                        if (modified)
                                printq("ignored_modified", format_user(session, uid));
                        else
                                printq("ignored_added", format_user(session, uid));
                        config_changed = 1;
                }

	} else {
		int unignore_all = ((params[0] && !xstrcmp(params[0], "*")) ? 1 : 0);
		int level;

		if (!params[0]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (params[0][0] == '#') {
			return command_exec_format(NULL, NULL, quiet, TEXT("/conference --unignore %s"), params[0]);
		}
		
		if (!unignore_all && !(uid = get_uid(session, params[0]))) {
			printq("user_not_found", params[0]);
			return -1;
		}

		if (unignore_all) {
			list_t l;
			int x = 0;
			
			for (l = session->userlist; l; ) {
				userlist_t *u = l->data;

				l = l->next;

				if (!ignored_remove(session, u->uid))
					x = 1;

				level = ignored_check(session, u->uid);
			}

			if (x) {
				wcs_printq("ignored_deleted_all");
				config_changed = 1;
			} else {
				wcs_printq("ignored_list_empty");
				return -1;
			}
			
			return 0;
		}

		level = ignored_check(session, uid);
		
		if (!ignored_remove(session, uid)) {
			printq("ignored_deleted", format_user(session, uid));
			config_changed = 1;
		} else {
			printq("error_not_ignored", format_user(session, uid));
			return -1;
		}
	
	}
	return 0;
}

COMMAND(cmd_list)
{
	PARASC
	list_t l;
	int count = 0, show_all = 1, show_away = 0, show_active = 0, show_inactive = 0, show_invisible = 0, show_descr = 0, show_blocked = 0, show_offline = 0, j;
	char **argv = NULL, *show_group = NULL;
	const char *tmp;
	metacontact_t *m = NULL;
	char *params0	= xstrdup(params[0]);		/* XXX, target */

        if (!params0 && window_current->target) { 
                params0 = xstrdup(window_current->target);
	}
	strip_quotes(params0);

	if (params0 && (*params0 != '-' || userlist_find(session, params0))) {
		char *status, *last_status;
		const char *group = params0;
		userlist_t *u;
		int invert = 0;
		
		/* list !@grupa */
		if (group[0] == '!' && group[1] == '@') {
			group++;
			invert = 1;
		}

		/* list @grupa */
		if (group[0] == '@' && xstrlen(group) > 1) {
			string_t members = string_init(NULL);
			char *__group;
			int count = 0;
			
			for (l = session->userlist; l; l = l->next) {
				u = l->data;

				if (u->groups || invert) {
					if ((!invert && ekg_group_member(u, group + 1)) || (invert && !ekg_group_member(u, group + 1))) {
						if (count++)
							string_append(members, ", ");
						string_append(members, u->nickname);
					}
				}
			}
			
			__group = saprintf("%s%s", ((invert) ? "!" : ""), group + 1);

			if (count)
				printq("group_members", __group, members->str);
			else
				printq("group_empty", __group);

			xfree(__group);

			string_free(members, 1);
			
			xfree(params0);
			return 0;
		}

	        if (params0 && (tmp = xstrrchr(params0, '/'))) {
        	        char *session_name = xstrndup(params0, xstrlen(params0) - xstrlen(tmp));
	
	                if (!session_find(session_name))
	                        goto next;
	
	                tmp++;
	                session = session_find(session_name);
	        	if (!(u = userlist_find(session, tmp)) || !u->nickname) {
	                        printq("user_not_found", tmp);
				xfree(session_name);
				xfree(params0);
	                	return -1;
			}
	
	                xfree(session_name);
	                goto list_user;
	        }

next:
		/* list _metacontact */
		if (params0 && (m = metacontact_find(params0))) {
        	        metacontact_item_t *i;
	
	                i = metacontact_find_prio(m);

	                if (!i) {
	                        wcs_printq("metacontact_item_list_empty");
				xfree(params0);
				return -1;
	               	} 
		
			u = userlist_find_n(i->s_uid, i->name);

	                status = format_string(format_find(ekg_status_label(u->status, u->descr, "metacontact_info_")), (u->first_name) ? u->first_name : u->nickname, u->descr);

	                printq("metacontact_info_header", params0);
			printq("metacontact_info_status", status);
	                printq("metacontact_info_footer", params0);

			xfree(status);
			xfree(params0);
			return 0;
		}
	
		if (!(u = userlist_find(session, params0)) || !u->nickname) {
			printq("user_not_found", params0);
			xfree(params0);
			return -1;
		}

list_user:
		status = format_string(format_find(ekg_status_label(u->status, u->descr, "user_info_")), 
				(u->first_name) ? u->first_name : u->nickname, u->descr);

                last_status = format_string(format_find(ekg_status_label(u->last_status, u->last_descr, "user_info_")), 
				(u->first_name) ? u->first_name : u->nickname, u->last_descr);

		printq("user_info_header", u->nickname, u->uid);
		if (u->nickname && xstrcmp(u->nickname, u->nickname)) 
			printq("user_info_nickname", u->nickname);

		if (u->first_name && xstrcmp(u->first_name, "") && u->last_name && u->last_name && xstrcmp(u->last_name, ""))
			printq("user_info_name", u->first_name, u->last_name);
		if (u->first_name && xstrcmp(u->first_name, "") && (!u->last_name || !xstrcmp(u->last_name, "")))
			printq("user_info_name", u->first_name, "");
		if ((!u->first_name || !xstrcmp(u->first_name, "")) && u->last_name && xstrcmp(u->last_name, ""))
			printq("user_info_name", u->last_name, "");

		printq("user_info_status", status);
                if (u->status_time && xstrcasecmp(u->status, EKG_STATUS_NA)) {
		        struct tm *status_time;
			char buf[100];		

			status_time = localtime(&(u->status_time));
	        	if (!strftime(buf, sizeof(buf), format_find("user_info_status_time_format") ,status_time) && xstrlen(format_find("user_info_status_time_format"))>0)
				xstrcpy(buf, "TOOLONG");


			printq("user_info_status_time", buf);
		}

		if (u->last_status)
			printq("user_info_last_status", last_status);

		if (u->authtype)
			printq("user_info_auth_type", u->authtype);

		if (ekg_group_member(u, "__blocked"))
			printq("user_info_block", ((u->first_name) ? u->first_name : u->nickname));
		if (ekg_group_member(u, "__offline"))
			printq("user_info_offline", ((u->first_name) ? u->first_name : u->nickname));

		query_emit(NULL, "userlist-info", &u, &quiet);

		if (u->ip) {
			char *ip_str = saprintf("%s:%s", inet_ntoa(*((struct in_addr*) &u->ip)), itoa(u->port));
			printq("user_info_ip", ip_str);
			xfree(ip_str);
		} else if (u->last_ip) {
			char *last_ip_str = saprintf("%s:%s", inet_ntoa(*((struct in_addr*) &u->last_ip)), itoa(u->last_port));
                        printq("user_info_last_ip", last_ip_str);
			xfree(last_ip_str);
		}

		if (u->mobile && xstrcmp(u->mobile, ""))
			printq("user_info_mobile", u->mobile);
		if (u->groups) {
			char *groups = group_to_string(u->groups, 0, 1);
			printq("user_info_groups", groups);
			xfree(groups);
		}
		if (!xstrcasecmp(u->status, EKG_STATUS_NA) || !xstrcasecmp(u->status, EKG_STATUS_INVISIBLE) || !xstrcasecmp(u->status, EKG_STATUS_ERROR)) {
			char buf[100];
			struct tm *last_seen_time;
			
			if (u->last_seen) {
				last_seen_time = localtime(&(u->last_seen));
				if (!strftime(buf, sizeof(buf), format_find("user_info_last_seen_time"), last_seen_time) && xstrlen(format_find("user_info_last_seen_time"))>0)
					xstrcpy(buf, "TOOLONG");
				printq("user_info_last_seen", buf);
			} else
				wcs_printq("user_info_never_seen");
		}
			
		printq("user_info_footer", u->nickname, u->uid);
		
		xfree(status);
		xfree(last_status);
		xfree(params0);
		return 0;
	}
	
	/* list --active | --away | --inactive | --invisible | --description | --member | --blocked | --offline */
	for (j = 0; params[j]; j++) {
		int i;

		argv = array_make(params[j], " \t", 0, 1, 1);

	 	for (i = 0; argv[i]; i++) {
			if (nmatch_arg(argv[i], 'a', TEXT("active"), 2)) {
				show_all = 0;
				show_active = 1;
			}
				
			if (nmatch_arg(argv[i], 'i', TEXT("inactive"), 2) || nmatch_arg(argv[i], 'n', TEXT("notavail"), 2)) {
				show_all = 0;
				show_inactive = 1;
			}
			
			if (nmatch_arg(argv[i], 'A', TEXT("away"), 2)) {
				show_all = 0;
				show_away = 1;
			}
			
			if (nmatch_arg(argv[i], 'I', TEXT("invisible"), 2)) {
				show_all = 0;
				show_invisible = 1;
			}

			if (nmatch_arg(argv[i], 'B', TEXT("blocked"), 2)) {
				show_all = 0;
				show_blocked = 1;
			}

			if (nmatch_arg(argv[i], 'o', TEXT("offline"), 2)) {
				show_all = 0;
				show_offline = 1;
			}

			if (nmatch_arg(argv[i], 'm', TEXT("member"), 2)) {
				if (j && argv[i+1]) {
					int off = (argv[i+1][0] == '@' && xstrlen(argv[i+1]) > 1) ? 1 : 0;

					show_group = xstrdup(argv[i+1] + off);
				} else
					if (params[i+1]) {
						char **tmp = array_make(params[i+1], " \t", 0, 1, 1);
						int off = (params[i+1][0] == '@' && xstrlen(params[i+1]) > 1) ? 1 : 0;

 						show_group = xstrdup(tmp[0] + off);
						array_free(tmp);
					}
			}

			if (nmatch_arg(argv[i], 'd', TEXT("description"), 2))
				show_descr = 1;
		}
		array_free(argv);
	}

	for (l = session->userlist; l; l = l->next) {
		userlist_t *u = l->data;
		int show;

		if (!u->nickname)
			continue;

		tmp = ekg_status_label(u->status, u->descr, "list_");

		show = show_all;

		if (show_away && !xstrcasecmp(u->status, EKG_STATUS_AWAY))
			show = 1;

		if (show_active && !xstrcasecmp(u->status, EKG_STATUS_AVAIL))
			show = 1;

		if (show_inactive && !xstrcasecmp(u->status, EKG_STATUS_NA))
			show = 1;

		if (show_invisible && !xstrcasecmp(u->status, EKG_STATUS_INVISIBLE))
			show = 1;

		if (show_blocked && !xstrcasecmp(u->status, EKG_STATUS_BLOCKED))
			show = 1;
		
		/* nie chcialo mi sie zmiennej robic */
		if (!xstrcasecmp(u->status, EKG_STATUS_ERROR))
			show = 1;

		if (show_descr && !u->descr)
			show = 0;

		if (show_group && !ekg_group_member(u, show_group))
			show = 0;

		if (show_offline && ekg_group_member(u, "__offline"))
			show = 1;

		if (show) {
			printq(tmp, format_user(session, u->uid), (u->first_name) ? u->first_name : u->nickname, inet_ntoa(*((struct in_addr*) &u->ip)), itoa(u->port), u->descr);
			count++;
		}
	}

	if (!count && !(show_descr || show_group) && show_all)
		wcs_printq("list_empty");
	xfree(show_group);
	xfree(params0);
	return 0;
}

COMMAND(cmd_save)
{
	PARASC
	last_save = time(NULL);
	
	if (!config_write(params[0]) && !session_write() && !metacontact_write() && !script_variables_write() ) {
		wcs_printq("saved");
		config_changed = 0;
		reason_changed = 0;
	} else {
		wcs_printq("error_saving");
		return -1;
	}

	return 0;
}

COMMAND(cmd_set)
{
	PARUNI
	const CHAR_T *arg = NULL, *val = NULL;
	int unset = 0, show_all = 0, res = 0;
	CHAR_T *value = NULL;
	list_t l;

	if (match_arg(params[0], 'a', TEXT("all"), 1)) {
		show_all = 1;
		arg = params[1];
		if (arg)
			val = params[2];
	} else {
		arg = params[0];
		if (arg)
			val = params[1];
	}
	
	if (arg && arg[0] == '-') {
		unset = 1;
		arg++;
	}

	if (arg && val) {
		CHAR_T **tmp = wcs_array_make(val, TEXT(""), 0, 0, 1);

		value = tmp[0];
		tmp[0] = NULL;
		wcs_array_free(tmp);
	}

	if ((!arg || !val) && !unset) {
		int displayed = 0;

		for (l = variables; l; l = l->next) {
			variable_t *v = l->data;
			
			if ((!arg || !xwcscasecmp(arg, v->name)) && (v->display != 2 || xwcscmp(name, TEXT("set")))) {
				char *string = *(char**)(v->ptr);
				int value = *(int*)(v->ptr);

				if (!show_all && !arg && v->dyndisplay && !((v->dyndisplay)(v->name)))
					continue;

				if (!v->display) {
					wcs_printq("variable", v->name, TEXT("(...)"));
					displayed = 1;
					continue;
				}

				if (v->type == VAR_STR || v->type == VAR_FILE || v->type == VAR_DIR || v->type == VAR_THEME) {
					CHAR_T *tmp = (string) ? wcsprintf(TEXT("\"%s\""), string) : TEXT("(none)");

					wcs_printq("variable", v->name, tmp);
					
					if (string)
						xfree(tmp);
				}

				if (v->type == VAR_BOOL)
					wcs_printq("variable", v->name, (value) ? TEXT("1 (on)") : TEXT("0 (off)"));
				
				if ((v->type == VAR_INT || v->type == VAR_MAP) && !v->map)
					wcs_printq("variable", v->name, wcs_itoa(value));

				if (v->type == VAR_INT && v->map) {
					CHAR_T *tmp = NULL;
					int i;

					for (i = 0; v->map[i].label; i++)
						if (v->map[i].value == value) {
							CHAR_T *vmaplabel = normal_to_wcs(v->map[i].label);
							tmp = wcsprintf(TEXT("%d (%s)"), value, vmaplabel);
							free_utf(vmaplabel);
							break;
						}

					if (!tmp)
						tmp = wcsprintf(TEXT("%d"), value);

					wcs_printq("variable", v->name, tmp);

					xfree(tmp);
				}

				if (v->type == VAR_MAP && v->map) {
					wcs_string_t s = wcs_string_init(wcs_itoa(value));
					int i, first = 1;

					for (i = 0; v->map[i].label; i++) {
						if ((value & v->map[i].value) || (!value && !v->map[i].value)) {
							CHAR_T *vmaplabel = normal_to_wcs(v->map[i].label);
							wcs_string_append(s, (first) ? TEXT(" (") : TEXT(","));
							first = 0;
							wcs_string_append(s, vmaplabel);
							free_utf(vmaplabel);
						}
					}

					if (!first)
						wcs_string_append_c(s, TEXT(')'));

					wcs_printq("variable", v->name, s->str);

					wcs_string_free(s, 1);
				}

				displayed = 1;
			}
		}

		if (!displayed && params[0]) {
			printq("variable_not_found", params[0]);
			return -1;
		}
	} else {
		variable_t *v = variable_find(arg);
		theme_cache_reset();

		if (!unset && !xwcscasecmp(value, TEXT("t"))) {
			if (v->type == VAR_BOOL) {
				int t_value = *(int*)(v->ptr);
			
				xfree(value);
				value = (t_value) ? xwcsdup(TEXT("0")) : xwcsdup(TEXT("1"));
			}
		} 

		switch (variable_set(arg, (unset) ? NULL : wcs_to_normal(value), 0)) {
			case 0:
			{
				const CHAR_T *my_params[2] = { (!unset) ? params[0] : params[0] + 1, NULL };

				cmd_set(TEXT("set-show"), (const CHAR_T **) my_params, NULL, NULL, quiet);
				config_changed = 1;
				last_save = time(NULL);
				break;
			}
			case -1:
				printq("variable_not_found", arg);
				res = -1;
				break;
			case -2:
				printq("variable_invalid", arg);
				res = -1;
				break;
		}
	}
	
	xfree(value);

	return res;
}

COMMAND(cmd_quit)
{
	PARASC
    	char *reason;
	list_t l;
	
	reason = xstrdup(params[0]);
	query_emit(NULL, "quitting", &reason);
	xfree(reason);

	for (l = sessions; l; l = l->next) {
		session_t *s = l->data;
		
		command_exec_format(NULL, s, 3, TEXT("/disconnect \"%s\""), (params[0]) ? params[0] : (s->descr) ? s->descr : "");
	}

	/* nie wychodzimy tutaj, �eby command_exec() mia�o szans� zwolni�
	 * u�ywan� przez siebie pami��. */
	quit_command = 1;

	return 0;
}

COMMAND(cmd_version) 
{
    	printq("ekg_version", VERSION, compile_time());
	query_emit(NULL, "plugin-print-version");

	return 0;
}

COMMAND(cmd_test_segv)
{
	char *foo = (char*) 0x41414141;

	*foo = 0x41;

	return 0;
}

COMMAND(cmd_test_send)
{
	PARASC
	const char *sender, *rcpts[2] = { NULL, NULL };

	if (!params[0] || !params[1] || !window_current || !window_current->session)
		return -1;

	sender = params[0];

	if (sender[0] == '>') {
		rcpts[0] = sender + 1;
		sender = window_current->session->uid;
	}

	message_print((session) ? session->uid : NULL, sender, (rcpts[0]) ? rcpts : NULL, params[1], NULL, time(NULL), EKG_MSGCLASS_CHAT, "1234", EKG_TRY_BEEP, 0);

	return 0;
}

COMMAND(cmd_test_addtab)
{
	PARASC
	tabnick_add(params[0]);
	return 0;
}

COMMAND(cmd_test_deltab)
{
	PARASC
	tabnick_remove(params[0]);
	return 0;
}

COMMAND(cmd_test_debug)
{
	PARUNI
	debug(CHARF "\n", params[0]);
	return 0;
}

COMMAND(cmd_test_debug_dump)
{
	CHAR_T *tmp = wcsprintf(TEXT("Zapisalem debug do pliku debug.%d"), (int) getpid());

	debug_write_crash();
	wcs_printq("generic", tmp);
	xfree(tmp);

	return 0;
}

COMMAND(cmd_test_event_test)
{
	PARASC
	char *tmp = xstrdup(params[0]);
	event_target_check(tmp);
	xfree(tmp);
	return 0;
}

COMMAND(cmd_debug_watches)
{
	list_t l;
	char buf[256];
	
	wcs_printq("generic_bold", TEXT("fd     wa   plugin  pers tout  started     rm"));
	
	for (l = watches; l; l = l->next) {
		watch_t *w = l->data;
		char wa[4];
		CHAR_T *plugin;

		xstrcpy(wa, "");

		if ((w->type & WATCH_READ))
			xstrcat(wa, "R");
		if ((w->type & WATCH_WRITE))
			xstrcat(wa, "W");
		if (w->buf)
			xstrcat(wa, "L");
		if (w->plugin)
			plugin = w->plugin->name;
		else
			plugin = TEXT("-");
#if USE_UNICODE
		snprintf(buf, sizeof(buf), "%-5d  %-3s  %-8ls  %-2d  %-4ld  %-10ld  %-2d", w->fd, wa, plugin, 1, w->timeout, w->started, w->removed);
#else
		snprintf(buf, sizeof(buf), "%-5d  %-3s  %-8s  %-2d  %-4ld  %-10ld  %-2d", w->fd, wa, plugin, 1, w->timeout, w->started, w->removed);
#endif
		printq("generic", buf);
	}

	return 0;
}

COMMAND(cmd_debug_queries)
{
	list_t l;
	
	wcs_printq("generic", TEXT("name                             | plugin   | count"));
	wcs_printq("generic", TEXT("---------------------------------|----------|------"));
	
	for (l = queries; l; l = l->next) {
		query_t *q = l->data;
		char buf[256];
		CHAR_T *plugin;

		plugin = (q->plugin) ? q->plugin->name : TEXT("-");
#if USE_UNICODE
		snprintf(buf, sizeof(buf), "%-32s | %-8ls | %d", q->name, plugin, q->count);
#else
		snprintf(buf, sizeof(buf), "%-32s | %-8s | %d", q->name, plugin, q->count);
#endif
		printq("generic", buf);
	}

	return 0;
}

COMMAND(cmd_debug_query)
{
	PARASC
	char *p[10];
	int i;

	memset(p, 0, sizeof(p));

	for (i = 0; params[i] && i < 10; i++)
		p[i] = xstrdup(params[i]);

	query_emit(NULL, p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &p[7], &p[8], &p[9]);

	for (i = 0; i < 10; i++)
		xfree(p[i]);

	return 0;
}

/* 
 * on Solaris files under /proc filesystem are in 
 * binary format, so this is not portable...
 *
 * it now supports linux, solaris, freebsd.
 */
COMMAND(cmd_test_mem) 
{
#ifndef NO_POSIX_SYSTEM
	char *temp, *p = NULL;
	CHAR_T *txt;
	FILE *file = NULL;
	int rd = 0, rozmiar = 0, unmres;
	struct utsname sys;

	unmres = uname(&sys);

	temp = saprintf("/proc/%d/status", getpid());

	if ( (unmres != -1 && !xstrcmp(sys.sysname, "FreeBSD")) || (file  = fopen(temp,"rb")) ) {
		xfree(temp);
#ifdef __linux__
			char buf[1024];

			rd = fread(buf, 1, 1024, file);
			fclose(file);
			if (rd == 0)
			{
				wcs_printq("generic_error", TEXT("Internal error, GiM's fault"));
				return -1;
			} 
			p = xstrstr(buf, "VmSize");
			if (p) {
				sscanf(p, "VmSize:     %d kB", &rozmiar);
			} else {
				wcs_printq("generic_error", TEXT("VmSize line not found!"));
				return -1;
			}
#elif __sun
			pstatus_t proc_stat;
			rd = fread(&proc_stat, sizeof(proc_stat), 1, file);
			fclose(file);
			if (rd == 0)
			{
				printq("generic_error", "Internal error, GiM's fault");
				return -1;
			}
			rozmiar = proc_stat.pr_brksize + proc_stat.pr_stksize;
#elif __FreeBSD__ /* link with -lkvm */
		        char errbuf[_POSIX2_LINE_MAX];
    			int nentries = -1;
    			struct kinfo_proc *kp;
			static kvm_t      *kd;

			if (!(kd = kvm_openfiles(NULL /* "/dev/null" */, "/dev/null", NULL, /* O_RDONLY */0, errbuf))) {
				wcs_printq("generic_error", TEXT("Internal error! (kvm_openfiles)"));
				return -1;
			}
			kp = kvm_getprocs(kd, KERN_PROC_PID, getpid(), &nentries);
    			if (!kp || nentries != 1) {
				wcs_printq("generic_error", TEXT("Internal error! (kvm_getprocs)"));
            			return -1; 
    			}
#ifdef HAVE_STRUCT_KINFO_PROC_KI_SIZE
			rozmiar = (u_long) kp->ki_size/1024; /* freebsd5 */
#else
			rozmiar = kp->kp_eproc.e_vm.vm_map.size/1024; /* freebsd4 */
#endif /* HAVE_STRUCT_KINFO_PROC_KI_SIZE */
#else
			if (unmres != -1)
				p = saprintf(" You seem to have /proc mounted, but I don't know how to deal with it. Authors would be very thankful, if you'd contac wit them, quoting this error message. [%s %s %s %s]", sys.sysname, sys.release, sys.version, sys.machine);
			else
				p = saprintf(" You seem to have /proc mounted, but I don't know how to deal with it. Authors would be very thankful, if you'd contac wit them, quoting this error message. uname() failed!");

			printq("generic_error", p);
			xfree(p);
			return -1;
#endif
		txt = wcsprintf(TEXT("Memory used by ekg2: %d kB"), rozmiar);
		wcs_printq("generic", txt);
		xfree(txt);
	} else {
		wcs_printq("generic_error", TEXT("/proc not mounted, no permissions, or no proc filesystem support"));
		xfree(temp);
		return -1;
	}
	return rozmiar;
#else
	return -1;
#endif
}

COMMAND(cmd_test_fds)
{
#ifndef NO_POSIX_SYSTEM
	struct stat st;
	char buf[1000];
	int i;
	
	for (i = 0; i < 2048; i++) {
		if (fstat(i, &st) == -1)
			continue;

		sprintf(buf, "%d: ", i);

		if (S_ISREG(st.st_mode))
			sprintf(buf + xstrlen(buf), "file, inode %lu, size %lu", st.st_ino, st.st_size);

		if (S_ISSOCK(st.st_mode)) {
			/* GiM: ten sock_n, bo sockaddr_in6 > sockaddr_in */
			char sock_n[256], bufek[256];
			struct sockaddr *sa = (struct sockaddr*)sock_n;
//			struct sockaddr_un *sun = (struct sockaddr_un*) &sa;
			struct sockaddr_in *sin = (struct sockaddr_in*) sa;
#ifdef HAVE_GETADDRINFO
			struct sockaddr_in6 *sin6 = (struct sockaddr_in6*) sa;
#endif
			int sa_len = 256;

			if (getpeername(i, sa, &sa_len) == -1) {
				getsockname(i, sa, &sa_len);

				/* GiM->dj: I'm not changing this stuff
				 * couse this was just beautifying stuff
				 * and by '*' I don't mean 0.0.0.0
				 */
				if (sa->sa_family == AF_INET) {
					xstrcat(buf, "socket, inet, *:");
					xstrcat(buf, itoa(ntohs(sin->sin_port)));
				} else if (sa->sa_family == AF_INET6) {
					xstrcat(buf, "socket, inet6, *:");
					xstrcat(buf, itoa(ntohs(sin->sin_port)));
				} else
					xstrcat(buf, "socket");
			} else {
				switch (sa->sa_family) {
					case AF_UNIX:
						xstrcat(buf, "socket, unix");
						break;
					case AF_INET:
						xstrcat(buf, "socket, inet, ");
						xstrcat(buf, inet_ntoa(sin->sin_addr));
						xstrcat(buf, ":");
						xstrcat(buf, itoa(ntohs(sin->sin_port)));
						break;
#ifdef HAVE_GETADDRINFO
					case AF_INET6:
						xstrcat(buf, "socket, inet6, ");
#ifdef HAVE_INET_NTOP
						inet_ntop(sa->sa_family, &(sin6->sin6_addr), bufek, sizeof(bufek));
						xstrcat(buf, bufek);
#else
						xstrcat(buf, "strange?");
#endif /* HAVE_INET_NTOP */
						xstrcat(buf, ":");
						xstrcat(buf, itoa(ntohs(sin6->sin6_port)));
						break;
#endif /* HAVE_GETADDRINFO */
					default:
						xstrcat(buf, "socket, ");
						xstrcat(buf, itoa(sa->sa_family));
				}
			}
		}
		
		if (S_ISDIR(st.st_mode))
			xstrcat(buf, "directory");
		
		if (S_ISCHR(st.st_mode))
			xstrcat(buf, "char");

		if (S_ISBLK(st.st_mode))
			xstrcat(buf, "block");

		if (S_ISFIFO(st.st_mode))
			xstrcat(buf, "fifo");

		if (S_ISLNK(st.st_mode))
			xstrcat(buf, "symlink");

		printq("generic", buf);
	}

	return 0;
#else
	return -1;
#endif
}

COMMAND(cmd_beep)
{
	query_emit(NULL, "ui-beep", NULL);

	return 0;
}

COMMAND(cmd_play)
{
	PARASC
	if (!config_sound_app) {
		wcs_printq("not_enough_params", name);
		return -1;
	}

	return play_sound(params[0]);
}

COMMAND(cmd_say)
{
	PARUNI
	if (!config_speech_app) {
		wcs_printq("not_enough_params", name);
		return -1;
	}

	if (match_arg(params[0], 'c', TEXT("clear"), 2)) {
		xfree(buffer_flush(BUFFER_SPEECH, NULL));
		return 0;
	}

	return say_it(params[0]);
}

COMMAND(cmd_reload)
{
	int res;

	if ((res = config_read_plugins())) printq("error_reading_config", strerror(errno));
	if (res == -1) return -1;

	if ((res = config_read(NULL))) printq("error_reading_config", strerror(errno));
	if (res == -1) return -1;

	if ((res = session_read())) printq("error_reading_config", strerror(errno));
	if (res == -1) return -1;

	wcs_printq("config_read_success");
	config_changed = 0;

	return res;
}

COMMAND(cmd_query)
{
	PARASC
	char **p = xcalloc(3, sizeof(char*));
	int i, res = 0;
	metacontact_t *m;
	char *tmp;

	/* skopiuj argumenty dla wywo�ania /chat */
	for (i = 0; params[i]; i++)
		p[i] = xstrdup(params[i]);

	p[i] = NULL;

	if (params[0] && (params[0][0] == '@' || xstrchr(params[0], ','))) {
		struct conference *c = conference_create(session, params[0]);
		
		if (!c) {
			res = -1;
			goto cleanup;
		}

		xfree(p[0]);
		p[0] = xstrdup(c->name);

		goto query;
	}

	if (params[0] && params[0][0] == '#') {
		struct conference *c = conference_find(params[0]);

		if (!c) {
			printq("conferences_noexist", params[0]);
			res = -1;
			goto cleanup;
		}
		
		xfree(p[0]);
		p[0] = xstrdup(c->name);

		goto query;
	}

	if (params[0] && (tmp = xstrrchr(params[0], '/'))) {
		char *session_name = xstrndup(params[0], xstrlen(params[0]) - xstrlen(tmp));
		session_t *sess_tmp;

		if (!(sess_tmp = session_find(session_name))) {
			xfree(session_name);
			goto next;
		} 

		session = sess_tmp;
		
		if (!get_uid(session, ++tmp)) {
			printq("user_not_found", tmp);
			xfree(session_name);
		        res = -1;
	                goto cleanup;
		}

		xfree(session_name);
                xfree(p[0]);
		p[0] = xstrdup(tmp);
		goto query;
	}

next:
	if (params[0] && (m = metacontact_find(params[0]))) {
		metacontact_item_t *i = metacontact_find_prio(m);

		if (!i) {
			wcs_printq("metacontact_item_list_empty");
			res = -1;
			goto cleanup;
		}

		xfree(p[0]);
		p[0] = xstrdup(i->name);
		session = session_find(i->s_uid);
		goto query;
	}	

	if (params[0] && !get_uid(session, params[0])) {
		printq("user_not_found", params[0]);
		res = -1;
		goto cleanup;
	}

query:
	if (!p[0] && !window_current->target)
		goto chat;

	if (p[0]) {
		window_t *w;

		if ((w = window_find_s(session, p[0]))) {
			window_switch(w->id);
			goto chat;
		}

		if (config_make_window == 1) {
			list_t l;

			for (l = windows; l; l = l->next) {
				window_t *v = l->data;

				if (v->id < 2 || v->floating || v->target)
					continue;

				w = v;
				break;
			}

			if (!w)
				w = window_new(p[0], session, 0);

			window_switch(w->id);
		}

		if (config_make_window == 2) {
			w = window_new(p[0], session, 0);
			window_switch(w->id);
		}

		xfree(window_current->target);
		window_current->target = xstrdup(p[0]);
		query_emit(NULL, "ui-window-target-changed", &window_current);

		if (!quiet) {
			print_window(p[0], session, 0, "query_started", p[0], session_name(session));
			print_window(p[0], session, 0, "query_started_window", p[0], session_name(session));
		}
	} else {
		query_emit(NULL, "ui-window-target-changed", &window_current);
	}

chat:
	if (params[0] && params[1]) {
		command_exec_format((p[0]) ? p[0] : params[0], session, quiet, TEXT("/ %s"), params[1]);
	}

cleanup:
	for (i = 0; p[i]; i++)
		xfree(p[i]);

	xfree(p);

	return res;
}

COMMAND(cmd_echo)
{
	PARUNI
	wcs_printq("generic", (params && params[0]) ? params[0] : TEXT(""));

	return 0;
}

COMMAND(cmd_bind)
{
	PARASC
	window_lock_dec_n(target); /* this is interactive command */
	query_emit(NULL, "binding-command", (params) ? params[0] : NULL, (params && params[0]) ? params[1] : NULL, (params && params[1]) ? params[2] : NULL, quiet);

	return 0;
}

/*
 * command_exec()
 *
 * wykonuje polecenie zawarte w linii tekstu. 
 *
 *  - target - w kt�rym oknie nast�pi�o (NULL je�li to nie query)
 *  - session - sesja, dla kt�rej dzia�amy
 *  - xline - linia tekstu
 *  - quiet - =1 ukrywamy wynik, =2 ukrywamy nieistnienie polecenia
 *
 * 0/-1.
 */
int command_exec(const char *target, session_t *session, const CHAR_T *xline, int quiet)
{
	CHAR_T short_cmd[2] = TEXT(".");
	CHAR_T *p = NULL;
	CHAR_T *line_save = NULL, *line = NULL;
	CHAR_T *cmd = NULL, *tmp;

	command_t *last_command = NULL;
	command_t *last_command_plugin = NULL; /* niepotrzebne, ale ktos to napisal tak ze moze kiedys mialobyc potrzebne.. wiec zostaje. */
	int abbrs = 0;
        int abbrs_plugins = 0;

	int exact = 0;

	list_t l;

	if (!xline)
		return 0;

	/* wysy�amy do kogo� i nie ma na pocz�tku slasha */
	if (target && *xline != '/') {
		int correct_command = 0;
	
		/* wykrywanie przypadkowo wpisanych polece� */
		if (config_query_commands) {
			for (l = commands; l; l = l->next) {
				command_t *c = l->data;
				int l = xwcslen(c->name);

				if (l < 3 || xwcsncasecmp(xline, c->name, l))
					continue;
				
				if (!xline[l] || xisspace(xline[l])) {
					correct_command = 1;
					break;
				}
			}		
		}

		if (!correct_command) {
			command_exec_format(target, session, quiet, TEXT("/ " CHARF), xline);
			return 0;
		}
	}
	
	send_nicks_index = 0;
	line = line_save = xwcsdup(xline);

	if (*line == '/')
		line++;

	if (*line == '^') {
		quiet = 1;
		line++;
	}

	for (l = commands; l; l = l->next) {
		command_t *c = l->data;

		if (!isalpha_pl_PL(c->name[0]) && xwcslen(c->name) == 1 && line[0] == c->name[0]) {
			short_cmd[0] = c->name[0];
			cmd = short_cmd;
			p = line + 1;
		}
	}

	if (!cmd) {
		tmp = cmd = line;
		while (*tmp && !xisspace(*tmp))
			tmp++;
		p = (*tmp) ? tmp + 1 : tmp;
		*tmp = 0;
		p = wcs_strip_spaces(p);
	}

	/* poszukaj najpierw komendy dla danej sesji */
	if (!session && session_current)
		session = session_current;
	if (session && session->uid) {
		CHAR_T *suid = normal_to_wcs(session->uid);
		int plen = (int)(xwcschr(suid, ':') - suid) + 1;
		
		for (l = commands; l; l = l->next) {
			command_t *c = l->data;

			if (xwcsncasecmp(c->name, suid, plen))
				continue;
		
			if (!xwcscasecmp(c->name + plen, cmd)) {
				last_command = c;
				abbrs = 1;
				exact = 1;
				break;
			}

			if (!xwcsncasecmp(c->name + plen, cmd, xwcslen(cmd))) {
				last_command_plugin = c;
				abbrs_plugins++;
			} else {
				if (last_command_plugin && abbrs_plugins == 1)
					break;
			} 
		}
		free_utf(suid);
	}
	if (!exact) {
		for (l = commands; l; l = l->next) {
			command_t *c = l->data;

			if (!xwcscasecmp(c->name, cmd)) {
				last_command = c;
				abbrs = 1;
				exact = 1;
				/* if this is exact_match we should zero those below, they won't be used */
			    	abbrs_plugins = 0; 
				last_command_plugin = NULL;
				break;
			}
			if (!xwcsncasecmp(c->name, cmd, xwcslen(cmd))) {
				last_command = c;
		    		abbrs++;
			} else {
				if (last_command && abbrs == 1)
					break;
			}
		} 
	}
//	debug("%x %x\n", last_command, last_command_plugin);

	if ((last_command && abbrs == 1 && !abbrs_plugins) || ( (last_command = last_command_plugin) && abbrs_plugins == 1 && !abbrs)) {
		session_t *s = session ? session : window_current->session;
		CHAR_T *last_name    = last_command->name;
		CHAR_T *tmp;
		int last_alias	   = 0;
		int res		   = 0;

		if (exact) 
			last_alias = (last_command->flags & COMMAND_ISALIAS || last_command->flags & COMMAND_ISSCRIPT) ? 1 : 0;
		
		if (!last_alias && (tmp = xwcschr(last_name, ':')))
			last_name = tmp + 1;
		
        	/* sprawdzamy czy session istnieje - je�li nie to nie mamy po co robi� czego� dalej ... */
		/* nie obsolete ? nie robi tego samego co `session_t *s = session ? session : window_current->session;` (?) */
		if (last_command->flags & SESSION_MUSTHAS) {
			if (!s && !(s = session_current))
				res = -1;
		}

		if (!res && last_command->flags & SESSION_MUSTHASPRIVATE) {
			if (!s || !(s->priv)) {
				wcs_printq("invalid_session");
				res = -1;
/*				debug("[command_exec] res = -1; SESSION_MUSTHASPRIVATE"); */
			}
		}
		if (!res && (last_command->flags & SESSION_MUSTBECONNECTED)) {
			if (!session_connected_get(s)) {
				printq("not_connected", session_name(s));	
				res = -1;
			}
		}
		if (!res && (last_command->flags & SESSION_MUSTBELONG)) {
			/* ok, it works but i still thinks that valid_plugin_uid sucks (too slow) */
			if (valid_plugin_uid(last_command->plugin, session_uid_get(s)) != 1) {
				wcs_printq("invalid_session");
				res = -1;
/*				debug("[command_exec] res = -1; SESSION_MUSTBELONG\n"); */
			}
		}

		if (!res) {
			CHAR_T **last_params = (last_command->flags & COMMAND_ISALIAS) ? wcs_array_make(TEXT("?"), TEXT(" "), 0, 1, 1) : last_command->params;
			int parcount = wcs_array_count(last_params);
			CHAR_T **par = wcs_array_make(p, TEXT(" \t"), parcount, 1, 1);
			char *ntarget;

			if ((last_command->flags & COMMAND_PARAMASTARGET) && par[0]) {
/*				debug("[command_exec] oldtarget = %s newtarget = %s\n", target, par[0]); */
				ntarget = xstrdup(wcs_to_normal(par[0]));
			} else	ntarget = xstrdup(target);

			target = strip_quotes(ntarget);

			if (/* !res && */ last_command->flags & COMMAND_ENABLEREQPARAMS) {
				int i;
				for (i=0; i < parcount; i++) {
					CHAR_T *p;
					if (!(p = last_params[i])) break; /* rather impossible */
					if (p[0] == '!' && !par[i]) {
						if (i == 0 && (last_command->flags & COMMAND_PARAMASTARGET) && target) /* if params[0] already in target */
							continue;	/* skip it */
						debug("[command_exec,%s] res = -1; req params[%d] = NIL\n", last_name, i);
						wcs_printq("not_enough_params", last_name);
						res = -1;
						break;
					} else if (p[0] != '!') break;
				}
			}

			if (!res) {
				window_t *w = window_find(target);

				window_lock_inc(w);
				res = (last_command->function)(last_name, (const CHAR_T **) par, s, target, (quiet & 1));
				if ((w == window_find(target)))
					window_lock_dec(w);
				else {
					list_t l;
					debug("[WINDOW LOCKING] INTERNAL ERROR SETTING ALL WINDOW LOCKS TO 0 [wtarget=%s command=%s]\n", target, last_name);
					/* may be faultly */
					for (l=windows; l; l = l->next) {
						window_t *w = l->data;
						if ((!w) || !(w->lock)) continue;
						w->lock = 0;
					}
				}
				query_emit(NULL, "ui-window-refresh");
			}
			if (last_command->flags & COMMAND_ISALIAS) wcs_array_free(last_params);
			wcs_array_free(par);
			xfree(ntarget);
		}
		xfree(line_save);

		if (quit_command)
			ekg_exit();

		return res;
	}

	if (xwcscmp(cmd, TEXT(""))) {
		quiet = quiet & 2;
		wcs_printq("unknown_command", cmd);
	}

	xfree(line_save);

	return -1;
}

int command_exec_format(const char *target, session_t *session, int quiet, const CHAR_T *format, ...)
{
	CHAR_T *command;
	va_list ap;
	int res;
	
	va_start(ap, format);
	command = vwcssaprintf(format, ap);
	va_end(ap);
	
	if (!command) 
		return 0;
/*	debug("[command_exec_format] %s\n", command); */
	res = command_exec(target, session, command, quiet); /* segvuje na tym ? wtf ?! */
	xfree(command);
	return res;
}

int binding_help(int a, int b)  
{
	wcs_print("help_quick");  

	return 0;  
}

/*
 * binding_quick_list()
 *
 * wy�wietla kr�tk� i zwi�z�a list� dost�pnych, zaj�tych i niewidocznych
 * ludzi z listy kontakt�w.
 */
int binding_quick_list(int a, int b)
{
	string_t list = string_init(NULL);
	list_t l, sl;
	for (sl = sessions; sl; sl = sl->next) {
		session_t *s = sl->data;
		for (l = s->userlist; l; l = l->next) {
			userlist_t *u = l->data;
			const char *format = NULL;
			char *tmp;

			if (!u->nickname)
				continue;
		
			format = format_find(ekg_status_label(u->status, NULL, "quick_list_"));

			if (!format)
				continue;

			if (!(tmp = format_string(format, u->nickname)))
				continue;

			string_append(list, tmp);

			xfree(tmp);
		}
	}

	if (xstrlen(list->str) > 0)
		print("quick_list", list->str);

	string_free(list, 1);

	return 0;
}

COMMAND(cmd_alias_exec)
{	
	PARUNI
	list_t l, tmp = NULL, m = NULL;
	int need_args = 0;

	for (l = aliases; l; l = l->next) {
		struct alias *a = l->data;

		if (!xwcscasecmp(name, a->name)) {
			tmp = a->commands;
			break;
		}
	}

	for (; tmp; tmp = tmp->next) {
		CHAR_T *p;
		int __need = 0;

		for (p = tmp->data; *p; p++) {

			if (*p == '\\' && p[1] == TEXT('%')) {
				p += 2;
				continue;
			}

			if (*p != '%')
				continue;

			p++;

			if (!*p)
				break;

			if (*p >= '1' && *p <= '9' && (*p - '0') > __need)
				__need = *p - '0';

			if (need_args < __need)
				need_args = __need;
		}

		list_add(&m, tmp->data, (xwcslen(tmp->data) + 1)*sizeof(CHAR_T));
	}
	
	for (tmp = m; tmp; tmp = tmp->next) {
		wcs_string_t str;

		if (*((CHAR_T *) tmp->data) == '/')
			str = wcs_string_init(NULL);
		else
			str = wcs_string_init(TEXT("/"));

		if (need_args) {
			CHAR_T *args[9], **arr, *s; 
			int i;

			if (!params[0]) {
				wcs_printq("aliases_not_enough_params", name);
				wcs_string_free(str, 1);
				list_destroy(m, 1);
				return -1;
			}

			arr = wcs_array_make(params[0], TEXT("\t "), need_args, 1, 1);

			if (wcs_array_count(arr) < need_args) {
				wcs_printq("aliases_not_enough_params", name);
				wcs_string_free(str, 1);
				wcs_array_free(arr);
				list_destroy(m, 1);
				return -1;
			}

			for (i = 0; i < 9; i++) {
				if (i < need_args)
					args[i] = arr[i];
				else
					args[i] = NULL;
			}

			s = wcs_format_string((CHAR_T *) tmp->data, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
			wcs_string_append(str, s);
			xfree(s);

			wcs_array_free(arr);

		} else {
			wcs_string_append(str, tmp->data);
			
			if (params[0]) {
				wcs_string_append(str, TEXT(" "));
				wcs_string_append(str, params[0]);
			}
		}

		command_exec(target, session, str->str, quiet);
		wcs_string_free(str, 1);
	}
	
	list_destroy(m, 1);
		
	return 0;
}

COMMAND(cmd_at)
{
	PARASC
	list_t l;

	if (nmatch_arg(params[0], 'a', TEXT("add"), 2)) {
		const char *p, *a_name = NULL;
		char *a_command = NULL;
		time_t period = 0, freq = 0;
		struct timer *t;

		if (!params[1] || !params[2]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (!strncmp(params[2], "*/", 2) || xisdigit(params[2][0])) {
			a_name = params[1];

			if (!xstrcmp(a_name, "(null)")) {
				wcs_printq("invalid_params", name);
				return -1;
			}

			for (l = timers; l; l = l->next) {
				t = l->data;

				if (!xstrcasecmp(t->name, a_name)) {
					printq("at_exist", a_name);
					return -1;
				}
			}

			p = params[2];
		} else
			p = params[1];

		{
			struct tm *lt;
			time_t now = time(NULL);
			char *tmp, *freq_str = NULL, *foo = xstrdup(p);
			int wrong = 0;

			lt = localtime(&now);
			lt->tm_isdst = -1;

			/* cz�stotliwo�� */
			if ((tmp = xstrchr(foo, '/'))) {
				*tmp = 0;
				freq_str = ++tmp;
			}

			/* wyci�gamy sekundy, je�li s� i obcinamy */
			if ((tmp = xstrchr(foo, '.')) && !(wrong = (xstrlen(tmp) != 3))) {
				sscanf(tmp + 1, "%2d", &lt->tm_sec);
				tmp[0] = 0;
			} else
				lt->tm_sec = 0;

			/* pozb�d�my si� dwukropka */
			if ((tmp = xstrchr(foo, ':')) && !(wrong = (xstrlen(tmp) != 3))) {
				tmp[0] = tmp[1];
				tmp[1] = tmp[2];
				tmp[2] = 0;
			}

			/* jedziemy ... */
			if (!wrong) {
				switch (xstrlen(foo)) {
					int ret;

					case 12:
						ret = sscanf(foo, "%4d%2d%2d%2d%2d", &lt->tm_year, &lt->tm_mon, &lt->tm_mday, &lt->tm_hour, &lt->tm_min);
						if (ret != 5)
							wrong = 1;
						lt->tm_year -= 1900;
						lt->tm_mon -= 1;
						break;
					case 10:
						ret = sscanf(foo, "%2d%2d%2d%2d%2d", &lt->tm_year, &lt->tm_mon, &lt->tm_mday, &lt->tm_hour, &lt->tm_min);
						if (ret != 5)
							wrong = 1;
						lt->tm_year += 100;
						lt->tm_mon -= 1;
						break;
					case 8:
						ret = sscanf(foo, "%2d%2d%2d%2d", &lt->tm_mon, &lt->tm_mday, &lt->tm_hour, &lt->tm_min);
						if (ret != 4)
							wrong = 1;
						lt->tm_mon -= 1;
						break;
					case 6:
						ret = sscanf(foo, "%2d%2d%2d", &lt->tm_mday, &lt->tm_hour, &lt->tm_min);
						if (ret != 3)
							wrong = 1;
						break;	
					case 4:
						ret = sscanf(foo, "%2d%2d", &lt->tm_hour, &lt->tm_min);
						if (ret != 2)
							wrong = 1;
						break;
					default:
						wrong = 1;
				}
			}

			/* nie ma b��d�w ? */
			if (wrong || lt->tm_hour > 23 || lt->tm_min > 59 || lt->tm_sec > 59 || lt->tm_mday > 31 || !lt->tm_mday || lt->tm_mon > 11) {
				wcs_printq("invalid_params", name);
				xfree(foo);
				return -1;
			}

			if (freq_str) {
				for (;;) {
					time_t _period = 0;

					if (xisdigit(*freq_str))
						_period = atoi(freq_str);
					else {
						wcs_printq("invalid_params", name);
						xfree(foo);
						return -1;
					}

					freq_str += xstrlen(itoa(_period));

					if (xstrlen(freq_str)) {
						switch (xtolower(*freq_str++)) {
							case 'd':
								_period *= 86400;
								break;
							case 'h':
								_period *= 3600;
								break;
							case 'm':
								_period *= 60;
								break;
							case 's':
								break;
							default:
								wcs_printq("invalid_params", name);
								xfree(foo);
								return -1;
						}
					}

					freq += _period;
					
					if (!*freq_str)
						break;
				}
			}

			xfree(foo);

			/* plany na przesz�o��? */
			if ((period = mktime(lt) - now) <= 0) {
				if (freq) {
					while (period <= 0)
						period += freq;
				} else {
					wcs_printq("at_back_to_past");
					return -1;
				}
			}
		}

		if (a_name)
			a_command = xstrdup(params[3]);
		else
			a_command = array_join((char **) params + 2, " ");

		if (a_command) {
			char *tmp = a_command;

			a_command = strip_spaces(a_command);
			
			if (!xstrcmp(a_command, "")) {
				wcs_printq("not_enough_params", name);
				xfree(tmp);
				return -1;
			}

			a_command = tmp;
		} else {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if ((t = timer_add(NULL, a_name, period, ((freq) ? 1 : 0), timer_handle_command, xstrdup(a_command)))) {
			t->at = 1;
			printq("at_added", t->name);
			if (freq)
				t->period = freq;
			if (!in_autoexec)
				config_changed = 1;
		}

		xfree(a_command);
		return 0;
	}

	if (nmatch_arg(params[0], 'd', TEXT("del"), 2)) {
		int del_all = 0;
		int ret = 1;

		if (!params[1]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (!xstrcmp(params[1], "*")) {
			del_all = 1;
			ret = timer_remove_user(1);
		} else
			ret = timer_remove(NULL, params[1]);
		
		if (!ret) {
			if (del_all)
				wcs_printq("at_deleted_all");
			else
				printq("at_deleted", params[1]);
			
			config_changed = 1;
		} else {
			if (del_all)
				wcs_printq("at_empty");
			else {
				printq("at_noexist", params[1]);
				return -1;
			}
		}

		return 0;
	}

	if (!params[0] || nmatch_arg(params[0], 'l', TEXT("list"), 2) || params[0][0] != '-') {
		const char *a_name = NULL;
		int count = 0;

		if (params[0] && nmatch_arg(params[0], 'l', TEXT("list"), 2))
			a_name = params[1];
		else if (params[0])
			a_name = params[0];

		for (l = timers; l; l = l->next) {
			struct timer *t = l->data;
			struct timeval tv;
			struct tm *at_time;
			char tmp[100], tmp2[150];
			time_t sec, minutes = 0, hours = 0, days = 0;

			if (!t->at || (a_name && xstrcasecmp(t->name, a_name)))
				continue;

			if (t->function != timer_handle_command)
				continue;

			count++;

			gettimeofday(&tv, NULL);

			at_time = localtime((time_t *) &t->ends.tv_sec);
			if (!strftime(tmp, sizeof(tmp), format_find("at_timestamp"), at_time)
					&& xstrlen(format_string("at_timestamp"))>0)
				xstrcpy(tmp, "TOOLONG");

			if (t->persist) {
				sec = t->period;

				if (sec > 86400) {
					days = sec / 86400;
					sec -= days * 86400;
				}

				if (sec > 3600) {
					hours = sec / 3600;
					sec -= hours * 3600;
				}
			
				if (sec > 60) {
					minutes = sec / 60;
					sec -= minutes * 60;
				}

				strlcpy(tmp2, "every ", sizeof(tmp2));

				if (days) {
					strlcat(tmp2, itoa(days), sizeof(tmp2));
					strlcat(tmp2, "d ", sizeof(tmp2));
				}

				if (hours) {
					strlcat(tmp2, itoa(hours), sizeof(tmp2));
					strlcat(tmp2, "h ", sizeof(tmp2));
				}

				if (minutes) {
					strlcat(tmp2, itoa(minutes), sizeof(tmp2));
					strlcat(tmp2, "m ", sizeof(tmp2));
				}

				if (sec) {
					strlcat(tmp2, itoa(sec), sizeof(tmp2));
					strlcat(tmp2, "s", sizeof(tmp2));
				}
			}

			printq("at_list", t->name, tmp, (char*)(t->data), "", ((t->persist) ? tmp2 : ""));
		}

		if (!count) {
			if (a_name) {
				printq("at_noexist", a_name);
				return -1;
			} else
				wcs_printq("at_empty");
		}

		return 0;
	}

	wcs_printq("invalid_params", name);

	return -1;
}

COMMAND(cmd_timer)
{
	PARASC
	list_t l;

	if (nmatch_arg(params[0], 'a', TEXT("add"), 2)) {
		const char *t_name = NULL, *p;
		char *t_command = NULL;
		time_t period = 0;
		struct timer *t;
		int persistent = 0;

		if (!params[1] || !params[2]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (xisdigit(params[2][0]) || !strncmp(params[2], "*/", 2)) {
			t_name = params[1];

			if (!xstrcmp(t_name, "(null)")) {
				wcs_printq("invalid_params", name);
				return -1;
			}

			for (l = timers; l; l = l->next) {
				t = l->data;

				if (!t->at && !xstrcasecmp(t->name, t_name)) {
					printq("timer_exist", t_name);
					return -1;
				}
			}

			p = params[2];
			t_command = xstrdup(params[3]);
		} else {
			p = params[1];
			t_command = array_join((char **) params + 2, " ");
		}

		if ((persistent = !strncmp(p, "*/", 2)))
			p += 2;

		for (;;) {
			time_t _period = 0;

			if (xisdigit(*p))
				_period = atoi(p);
			else {
				wcs_printq("invalid_params", name);
				xfree(t_command);
				return -1;
			}

			p += xstrlen(itoa(_period));

			if (xstrlen(p)) {
				switch (xtolower(*p++)) {
					case 'd':
						_period *= 86400;
						break;
					case 'h':
						_period *= 3600;
						break;
					case 'm':
						_period *= 60;
						break;
					case 's':
						break;
					default:
						wcs_printq("invalid_params", name);
						xfree(t_command);
						return -1;
				}
			}

			period += _period;
			
			if (!*p)
				break;
		}

		if (t_command) {
			char *tmp = t_command;

			t_command = strip_spaces(t_command);

			if (!xstrcmp(t_command, "")) {
				wcs_printq("not_enough_params", name);
				xfree(tmp);
				return -1;
			}

			t_command = tmp;
		} else {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if ((t = timer_add(NULL, t_name, period, persistent, timer_handle_command, xstrdup(t_command)))) {
			printq("timer_added", t->name);
			if (!in_autoexec)
				config_changed = 1;
		}

		xfree(t_command);
		return 0;
	}

	if (nmatch_arg(params[0], 'd', TEXT("del"), 2)) {
		int del_all = 0, ret;

		if (!params[1]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (!xstrcmp(params[1], "*")) {
			del_all = 1;
			ret = timer_remove_user(0);
		} else
			ret = timer_remove(NULL, params[1]);
		
		if (!ret) {
			if (del_all)
				printq("timer_deleted_all");
			else
				printq("timer_deleted", params[1]);

			config_changed = 1;
		} else {
			if (del_all)
				printq("timer_empty");
			else {
				printq("timer_noexist", params[1]);
				return -1;	
			}
		}

		return 0;
	}

	if (!params[0] || nmatch_arg(params[0], 'l', TEXT("list"), 2) || params[0][0] != '-') {
		const char *t_name = NULL;
		int count = 0;

		if (params[0] && nmatch_arg(params[0], 'l', TEXT("list"), 2))
			t_name = params[1];
		else if (params[0])
			t_name = params[0];

		for (l = timers; l; l = l->next) {
			struct timer *t = l->data;
			struct timeval tv;
			char *tmp;
			long usec, sec, minutes = 0, hours = 0, days = 0;

			if (t->function != timer_handle_command)
				continue;

			if (t->at || (t_name && xstrcasecmp(t->name, t_name)))
				continue;

			count++;

			gettimeofday(&tv, NULL);

			if (t->ends.tv_usec < tv.tv_usec) {
				sec = t->ends.tv_sec - tv.tv_sec - 1;
				usec = (t->ends.tv_usec - tv.tv_usec + 1000000) / 1000;
			} else {
				sec = t->ends.tv_sec - tv.tv_sec;
				usec = (t->ends.tv_usec - tv.tv_usec) / 1000;
			}

			if (sec > 86400) {
				days = sec / 86400;
				sec -= days * 86400;
			}

			if (sec > 3600) {
				hours = sec / 3600;
				sec -= hours * 3600;
			}
		
			if (sec > 60) {
				minutes = sec / 60;
				sec -= minutes * 60;
			}

			if (days)
				tmp = saprintf("%ldd %ldh %ldm %ld.%.3ld", days, hours, minutes, sec, usec);
			else
				if (hours)
					tmp = saprintf("%ldh %ldm %ld.%.3ld", hours, minutes, sec, usec);
				else
					if (minutes)
						tmp = saprintf("%ldm %ld.%.3ld", minutes, sec, usec);
					else
						tmp = saprintf("%ld.%.3ld", sec, usec);

			printq("timer_list", t->name, tmp, (char*)(t->data), "", (t->persist) ? "*" : "");
			xfree(tmp);
		}

		if (!count) {
			if (t_name) {
				printq("timer_noexist", t_name);
				return -1;
			} else
				wcs_printq("timer_empty");
		}

		return 0;
	}	

	wcs_printq("invalid_params", name);

	return -1;
}

COMMAND(cmd_conference) 
{
	PARASC
	if (!params[0] || nmatch_arg(params[0], 'l', TEXT("list"), 2) || params[0][0] == '#') {
		list_t l, r;
		int count = 0;
		const char *cname = NULL;
	
		if (params[0] && nmatch_arg(params[0], 'l', TEXT("list"), 2))
			cname = params[1];
		else if (params[0])
			cname = params[0];

		for (l = conferences; l; l = l->next) {
			struct conference *c = l->data;
			string_t recipients;
			const char *recipient;
			int first = 0;

			recipients = string_init(NULL);

			if (cname && xstrcasecmp(cname, c->name))
				continue;
			
			for (r = c->recipients; r; r = r->next) {
				char *uid = r->data;
				userlist_t *u = userlist_find(session, uid);

				if (u && u->nickname)
					recipient = u->nickname;
				else
					recipient = uid;

				if (first++)
					string_append_c(recipients, ',');
				
				string_append(recipients, recipient);

				count++;
			}

		        print(c->ignore ? "conferences_list_ignored" : "conferences_list", c->name, recipients->str);

			string_free(recipients, 1);
		}

		if (!count) {
			if (params[0] && params[0][0] == '#') {
				printq("conferences_noexist", params[0]);
				return -1;
			} else
				wcs_printq("conferences_list_empty");
		}

		return 0;
	}

	if (nmatch_arg(params[0], 'j', TEXT("join"), 2)) {
		struct conference *c;
		const char *uid;

		if (!params[1] || !params[2]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (params[1][0] != '#') {
			printq("conferences_name_error");
			return -1;
		}

		if (!(c = conference_find(params[1]))) {
			printq("conferences_noexist");
			return -1;
		}

		if (!(uid = get_uid(session, params[2]))) {
			printq("unknown_user", params[2]);
			return -1;
		}

		if (conference_participant(c, uid)) {
			printq("conferences_already_joined", format_user(session, uid), params[1]);
			return -1;
		}

		list_add(&c->recipients, (void*) uid, xstrlen(uid) + 1);

		printq("conferences_joined", format_user(session, uid), params[1]);

		return 0;
	}

	if (nmatch_arg(params[0], 'a', TEXT("add"), 2)) {
		if (!params[1]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (params[2]) {
			if (params[1][0] != '#') {
				wcs_printq("conferences_name_error");
				return -1;
			} else
				conference_add(session, params[1], params[2], quiet);
		} else
			conference_create(session, params[1]);

		return 0;
	}

	if (nmatch_arg(params[0], 'd', TEXT("del"), 2)) {
		if (!params[1]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (!xstrcmp(params[1], "*"))
			conference_remove(NULL, quiet);
		else {
			if (params[1][0] != '#') {
				wcs_printq("conferences_name_error");
				return -1;
			}

			conference_remove(params[1], quiet);
		}

		return 0;
	}

	if (nmatch_arg(params[0], 'r', TEXT("rename"), 2)) {
		if (!params[1] || !params[2]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (params[1][0] != '#' || params[2][0] != '#') {
			wcs_printq("conferences_name_error");
			return -1;
		}

		conference_rename(params[1], params[2], quiet);

		return 0;
	}
	
	if (nmatch_arg(params[0], 'i', TEXT("ignore"), 2)) {
		if (!params[1]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (params[1][0] != '#') {
			wcs_printq("conferences_name_error");
			return -1;
		}

		conference_set_ignore(params[1], 1, quiet);

		return 0;
	}

	if (nmatch_arg(params[0], 'u', TEXT("unignore"), 2)) {
		if (!params[1]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (params[1][0] != '#') {
			wcs_printq("conferences_name_error");
			return -1;
		}

		conference_set_ignore(params[1], 0, quiet);

		return 0;
	}

	if (nmatch_arg(params[0], 'f', TEXT("find"), 2)) {
		struct conference *c;
		list_t l;

		if (!params[1]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		if (params[1][0] != '#') {
			wcs_printq("conferences_name_error");
			return -1;
		}

		c = conference_find(params[1]);

		if (c) {
			for (l = c->recipients; l; l = l->next) {
				command_exec_format(target, session, quiet, TEXT("/find %s\n"), (char*) (l->data));
			}
		} else {
			printq("conferences_noexist", params[1]);
			return -1;
		}


		return 0;
	}

	wcs_printq("invalid_params", name);

	return -1;
}

COMMAND(cmd_last)
{
	PARASC
        list_t l;
	const char *uid = NULL;
	int show_sent = 0, last_n = 0, count = 0, i = 0, show_all = 0;
	char **arr = NULL;
	const char *nick = NULL;
	time_t n;
	struct tm *now;

	if (nmatch_arg(params[0], 'c', TEXT("clear"), 2)) {
		if (params[1] && !(uid = get_uid(session, params[1]))) {
			printq("user_not_found", params[1]);
			return -1;
		}

		if ((uid && !last_count(uid)) || !list_count(lasts)) {
			if (uid)
				printq("last_list_empty_nick", format_user(session, uid));
			else
				wcs_printq("last_list_empty");

			return -1;
		}

		if (uid) {
			last_del(uid);
			printq("last_clear_uin", format_user(session, uid));
		} else {
			last_free();
			wcs_printq("last_clear");
		}

		return 0;
	}		

	if (params[0]) {
		show_sent = nmatch_arg(params[0], 's', TEXT("stime"), 2);

		if (!show_sent)
			nick = params[0];

		if (params[1]) {
			arr = array_make(params[1], " \t", 0, 1, 0);

			nick = arr[0];
			
			if (nmatch_arg(params[0], 'n', TEXT("number"), 2)) {
				last_n = strtol(arr[0], NULL, 0);
				nick = arr[1];
				
				if (arr[1] && (show_sent = nmatch_arg(arr[1], 's', TEXT("stime"), 2)))
					nick = arr[2];
			}

			if (arr[1] && show_sent && nmatch_arg(arr[0], 'n', TEXT("number"), 2)) {
				last_n = atoi(arr[1]);
				nick = arr[2];
			}

		}
	}

	show_all = (nick && !xstrcasecmp(nick, "*")) ? 1 : 0;

	if (!show_all && nick && !(uid = get_uid(session, nick))) {
		printq("user_not_found", nick);
		array_free(arr);
		return -1;
	}

	array_free(arr);
		
	if (!((uid) ? (count = last_count(uid)) : (count = list_count(lasts)))) {
		if (uid) {
			printq("last_list_empty_nick", format_user(session, uid));
			return -1;
		}

		wcs_printq("last_list_empty");
		return 0;
	}

	n = time(NULL);
	now = localtime(&n);

        printq("last_begin");

        for (l = lasts; l; l = l->next) {
                struct last *ll = l->data;
		struct tm *tm, *st;
		char buf[100], buf2[100], *time_str = NULL;
		const char *form;

		if (show_all || !uid || !xstrcasecmp(uid, ll->uid)) {

			if (last_n && i++ < (count - last_n))
				continue;

			tm = localtime(&ll->time);
			if (!strftime(buf, sizeof(buf), format_find("last_list_timestamp"), tm)
					&& xstrlen(format_find("last_list_timestamp"))>0)
				xstrcpy(buf, "TOOLONG");

			if (show_sent && ll->type == 0 && !(ll->sent_time - config_time_deviation <= ll->time && ll->time <= ll->sent_time + config_time_deviation)) {
				st = localtime(&ll->sent_time);
				form = format_find((tm->tm_yday == now->tm_yday) ? "last_list_timestamp_today" : "last_list_timestamp");
				if (!strftime(buf2, sizeof(buf2), form, st) && xstrlen(form)>0)
					xstrcpy(buf2, "TOOLONG");
				time_str = saprintf("%s/%s", buf, buf2);
			} else
				time_str = xstrdup(buf);

			if (config_last & 4 && ll->type == 1)
				printq("last_list_out", time_str, format_user(session, ll->uid), ll->message);
			else
				printq("last_list_in", time_str, format_user(session, ll->uid), ll->message);

			xfree(time_str);
		}
        }

        wcs_printq("last_end");

	return 0;
}

COMMAND(cmd_queue)
{
	PARASC
	list_t l;
	
	if (nmatch_arg(params[0], 'c', TEXT("clear"), 2)) {
		if ((params[1] && !msg_queue_count_uid(params[1])) || !msg_queue_count()) {
			if (params[1])
				printq("queue_empty_uid", format_user(session, params[1]));
			else
				wcs_printq("queue_empty");

			return 0;
		}

		if (params[1]) {
			msg_queue_remove_uid(params[1]);
			printq("queue_clear_uid", format_user(session, params[1]));
		} else {
			msg_queue_free();
			wcs_printq("queue_clear");
		}

		return 0;
	}		

	if ((params[0] && !msg_queue_count_uid(params[0])) || !msg_queue_count()) {
		if (params[0])
			printq("queue_empty_uid", format_user(session, params[0]));
		else
			wcs_printq("queue_empty");

		return 0;
	}

        for (l = msg_queue; l; l = l->next) {
                msg_queue_t *m = l->data;
		struct tm *tm;
		char buf[100];

		if (!params[0] || !xstrcasecmp(m->rcpts, params[0])) {
			tm = localtime(&m->time);
			if (!strftime(buf, sizeof(buf), format_find("queue_list_timestamp"), tm)
					&& xstrlen(format_find("queue_list_timestamp"))>0)
				xstrcpy(buf, "TOOLONG");

			printq("queue_list_message", buf, m->rcpts, m->message);
		}
	}

	return 0;
}

COMMAND(cmd_dcc)
{
	PARASC
	list_t l;

	if (!params[0] || !xstrncasecmp(params[0], "li", 2)) {	/* list */
		int empty = 1, passed = 0;

		for (l = dccs; l; l = l->next) {
			dcc_t *d = l->data;

			if (d->active)
				continue;
			
			empty = 0;
			
			if (!passed++)
				wcs_printq("dcc_show_pending_header");

			switch (d->type) {
				case DCC_SEND:
					printq("dcc_show_pending_send", itoa(d->id), format_user(session, d->uid), d->filename);
					break;
				case DCC_GET:
					printq("dcc_show_pending_get", itoa(d->id), format_user(session, d->uid), d->filename);
					break;
				case DCC_VOICE:
					printq("dcc_show_pending_voice", itoa(d->id), format_user(session, d->uid));
					break;
				default:
					break;
			}
		}

		passed = 0;

		for (l = dccs; l; l = l->next) {
			dcc_t *d = l->data;

			if (!d->active)
				continue;

			empty = 0;

			if (!passed++)
				wcs_printq("dcc_show_active_header");

			switch (d->type) {
				case DCC_SEND:
					printq("dcc_show_active_send", itoa(d->id), format_user(session, d->uid), d->filename, itoa(d->offset), itoa(d->size), 
							(d->size) ? itoa(100 * (d->offset / d->size)) : "?");
					break;
				case DCC_GET:
					printq("dcc_show_active_get", itoa(d->id), format_user(session, d->uid), d->filename, itoa(d->offset), itoa(d->size), 
							(d->size) ? itoa(100 * (d->offset / d->size)) : "?");
					break;
				case DCC_VOICE:
					printq("dcc_show_active_voice", itoa(d->id), format_user(session, d->uid));
					break;
				default:
					break;
			}
		}

		if (empty)
			wcs_printq("dcc_show_empty");
		
		return 0;
	}

	if (!xstrncasecmp(params[0], "c", 1)) {		/* close */
		dcc_t *d = NULL;
		const char *uid;

		if (!params[1]) {
			wcs_printq("not_enough_params", name);
			return -1;
		}

		uid = get_uid(session, params[1]);		

		for (l = dccs; l; l = l->next) {
			dcc_t *D = l->data;

			if (params[1][0] == '#' && atoi(params[1] + 1) == D->id) {
				d = D;
				uid = dcc_uid_get(d);
				break;
			}

			if (uid && !xstrcasecmp(D->uid, uid)) {
				d = D;
				break;
			}
		}

		if (!d) {
			printq("dcc_not_found", params[1]);
			return -1;
		}

		printq("dcc_close", format_user(session, uid));
		dcc_close(d);

		return 0;
	}

	wcs_printq("invalid_params", name);

	return -1;
}

COMMAND(cmd_plugin)
{
	PARUNI
	int ret;
	plugin_t *pl;

	if (!params[0]) {
		list_t l;
		
		for (l = plugins; l; l = l->next) {
			plugin_t *p = l->data;

			wcs_printq("plugin_list", (p && p->name) ? p->name : TEXT("?"), wcs_itoa(p->prio));
		}

		return 0;
	}

        if (match_arg(params[0], 'd', TEXT("default"), 2)) {
                list_t l;

                for (l = plugins; l;) {
                        plugin_t *p = l->data;

                        l = l->next;

                        list_remove(&plugins, p, 0);
                        plugin_register(p, -254);
                }

		config_changed = 1;
                wcs_printq("plugin_default");
        }

	if (params[0][0] == '+') {
		ret = plugin_load(params[0] + 1, -254, 0);
		if (!ret) /* if plugin cannot be founded || loaded don't reload theme. */
			changed_theme(NULL); 
		return ret;
	}

	if (params[0][0] == '-')
		return plugin_unload(plugin_find(params[0] + 1));

	if (params[1] && (pl = plugin_find(params[0]))) {
		list_remove(&plugins, pl, 0);
		plugin_register(pl, wcs_atoi(params[1])); 

		config_changed = 1;
		wcs_printq("plugin_prio_set", pl->name, params[1]);

		return 0;
	}

	wcs_printq("invalid_params", name);

	return -1;
}

/*
 * changes reason without changing status
 */

COMMAND(cmd_desc)
{
	PARASC
	const char *status, *cmd;
	
	if (!session)
		return -1;
	
	status = session_status_get(session);

	if (!xstrcmp(status, EKG_STATUS_AVAIL)) {
		cmd = "back";
	} else if (!xstrcmp(status, EKG_STATUS_AWAY)) {
		cmd = "away";
	} else if (!xstrcmp(status, EKG_STATUS_AUTOAWAY)) {
		cmd = "away";
	} else if (!xstrcmp(status, EKG_STATUS_INVISIBLE)) {
		cmd = "invisible";
	} else if (!xstrcmp(status, EKG_STATUS_XA)) {
		cmd = "xa";
	} else if (!xstrcmp(status, EKG_STATUS_DND)) {
		cmd = "dnd";
	} else if (!xstrcmp(status, EKG_STATUS_FREE_FOR_CHAT)) {
		cmd = "ffc";
	} else {
		/* invalid self status? */
		return -2;
	};
	
	return command_exec_format(NULL, session, 0, TEXT("/%s %s"), cmd, (params[0] ? params[0] : ""));
}

/*
 * command_add_compare()
 *
 * funkcja por�wnuj�ca nazwy komend, by wyst�powa�y alfabetycznie w li�cie.
 *
 *  - data1, data2 - dwa wpisy komend do por�wnania.
 *
 * zwraca wynik xstrcasecmp() na nazwach komend.
 */
static int command_add_compare(void *data1, void *data2)
{
	command_t *a = data1, *b = data2;

	if (!a || !a->name || !b || !b->name)
		return 0;

	return xwcscasecmp((CHAR_T *) a->name, (CHAR_T *) b->name);
}

/*
 * command_add()
 *
 * dodaje komend�. 
 *
 *  - plugin - plugin obs�uguj�cy komend�,
 *  - name - nazwa komendy,
 *  - params - definicja parametr�w (szczeg�y poni�ej),
 *  - function - funkcja obs�uguj�ca komend�,
 *  - flags - bitmask. look @ commands.h
 *  - possibilities - mo�liwo�ci tj ewentualne parametry - przy dope�nianiu przydatne 
 *
 * 0 je�li si� nie uda�o, w przeciwnym razie adres do strukturki.
 */
command_t *command_add(plugin_t *plugin, const CHAR_T *name, CHAR_T *params, command_func_t function, int flags, char *possibilities)
{
	command_t *c = xmalloc(sizeof(command_t));

	c->name = xwcsdup(name);
	c->params = params ? wcs_array_make(params, TEXT(" "), 0, 1, 1) : NULL;
	c->function = function;
	c->flags = flags;
	c->plugin = plugin;
	c->possibilities = possibilities ? array_make(possibilities, " ", 0, 1, 1) : NULL;

	return list_add_sorted(&commands, c, 0, command_add_compare);
}

/*
 * command_remove()
 *
 * usuwa komend� z listy.
 *
 *  - plugin - plugin obs�uguj�cy,
 *  - name - nazwa komendy.
 */

void command_freeone(command_t *c)
{
	if (!c)
		return;
	xfree(c->name);
	wcs_array_free(c->params);
	array_free(c->possibilities);
	list_remove(&commands, c, 1);
}

int command_remove(plugin_t *plugin, const CHAR_T *name)
{
	list_t l;

	for (l = commands; l;) {
		command_t *c = l->data;
		l = l->next;

		if (!xwcscasecmp(name, c->name) && plugin == c->plugin) {
			command_freeone(c);
			return 0;
		}
	}

	return -1;
}

/*
 * rodzaje parametr�w komend:
 *
 * '?' - it means nothing 
 * 'U' - uid typed by hand, sender of message
 * 'u' - name or uid from the contact list
 * 'C' - name of conference
 * 'c' - name of command
 * 'i' - nicks of ignored
 * 'b' - nicks of blocked
 * 'v' - variable name
 * 'p' - params typed in possibilities
 * 'P' - plugin's name
 * 'f' - file
 * 'e' - event name
 * 'I' - ignored level
 * 's' - session name
 * 'S' - session variable name
 * 'r' - session description 
 * 'o' - directory
 * 'm' - metacontact
 * 'w' - window name
 * 
 * je�eli parametr == 'p' to 6 argument funkcji command_add() przyjmuje jako argument
 * tablic� z mo�liwymi uzupe�nieniami 
 * 
 * parametry te� s� tablic�, dlatego u�ywamy makr possibilities() i params() - najlepiej
 * przeanalizowa� przyk�ady 
 */

/*
 * command_init()
 *
 * inicjuje list� domy�lnych komend.
 */
void command_init()
{
#if USE_UNICODE
#define LNULL NULL
#define command_add(x, y, par, a, b, c) command_add(x, y, L##par, a, b, c)
#endif

	command_add(NULL, TEXT("add"), "U ? p", cmd_add, 0, "-f --find");

	command_add(NULL, TEXT("alias"), "p ?", cmd_alias, 0,
	 "-a --add -A --append -d --del -l --list");

	command_add(NULL, TEXT("at"), "p ? ? c", cmd_at, 0, 
	 "-a --add -d --del -l --list");

	command_add(NULL, TEXT("beep"), NULL, cmd_beep, 0, NULL);

	command_add(NULL, TEXT("bind"), "p ? ?", cmd_bind, 0,
	 "-a --add -d --del -l --list -L --list-default");

	command_add(NULL, TEXT("clear"), NULL, cmd_window, 0,	NULL);
 
        command_add(NULL, TEXT("conference"), "p C uU", cmd_conference, SESSION_MUSTHAS,
          "-a --add -j --join -d --del -i --ignore -u --unignore -r --rename -f --find -l --list");
 
	command_add(NULL, TEXT("dcc"), "p u f ?", cmd_dcc, 0,
	  "send rsend get resumce rvoice voice close list");

	command_add(NULL, TEXT("del"), "u ?", cmd_del, 0, NULL);
	
	command_add(NULL, TEXT("echo"), "?", cmd_echo, 0, NULL);
	  
	command_add(NULL, TEXT("exec"), "p UuC ?", cmd_exec, 0,
	  "-m --msg -b --bmsg");
	
	command_add(NULL, TEXT("eval"), "!", cmd_eval, COMMAND_ENABLEREQPARAMS, NULL);
 
	command_add(NULL, TEXT("for"), "!p !? !c", cmd_for, COMMAND_ENABLEREQPARAMS,
          "-s --sessions -u --users -w --windows");
 
	command_add(NULL, TEXT("!"), "?", cmd_exec, 0, NULL);

	command_add(NULL, TEXT("help"), "c vS", cmd_help, 0, NULL);
	  
	command_add(NULL, TEXT("?"), "c vS", cmd_help, 0, NULL);
	 
	command_add(NULL, TEXT("ignore"), "uUC I", cmd_ignore, 0,
	  "status descr notify msg dcc events *");
	  
	command_add(NULL, TEXT("last"), "CpuU CuU", cmd_last, SESSION_MUSTHAS,
	  "-c --clear -s --stime -n --number");

        command_add(NULL, TEXT("metacontact"), "mp m s uU ?", cmd_metacontact, 0,
          "-a --add -d --del -i --add-item -r --del-item -l --list");

	command_add(NULL, TEXT("list"), "CpuUsm", cmd_list, SESSION_MUSTHAS,
	  "-a --active -A --away -i --inactive -B --blocked -d --description -m --member -o --offline -f --first -l --last -n --nick -d --display -u --uin -g --group -p --phone -o --offline -O --online");
	  
	command_add(NULL, TEXT("on"), "p e ? UuC c", cmd_on, SESSION_MUSTHAS,
	  "-a --add -d --del -l --list" );
	
	command_add(NULL, TEXT("play"), "!f", cmd_play, COMMAND_ENABLEREQPARAMS, NULL);

	command_add(NULL, TEXT("plugin"), "P ?", cmd_plugin, 0, NULL);

	command_add(NULL, TEXT("query"), "uUCms ?", cmd_query, SESSION_MUSTHAS,
	  "-c --clear");

	command_add(NULL, TEXT("queue"), "puUC uUC", cmd_queue, 0, 
	  "-c --clear");

 
	command_add(NULL, TEXT("quit"), "r", cmd_quit, 0, NULL);
	  
	command_add(NULL, TEXT("reload"), NULL, cmd_reload, 0, NULL);
	  
	command_add(NULL, TEXT("save"), NULL, cmd_save, SESSION_MUSTHAS, NULL);

	command_add(NULL, TEXT("say"), "!", cmd_say, COMMAND_ENABLEREQPARAMS,
	  "-c --clear");
	  
        command_add(NULL, TEXT("session"), "psS psS sS ?", session_command, 0,
          "-a --add -d --del -l --list -g --get -s --set -w --sw");

	command_add(NULL, TEXT("set"), "v ?s", cmd_set, 0, NULL);

        command_add(NULL, TEXT("status"), "s", cmd_status, 0, NULL);

	command_add(NULL, TEXT("tabclear"), "p", cmd_tabclear, 0,
	  "-o --offline");

	command_add(NULL, TEXT("timer"), "p ? ? c", cmd_timer, 0,
	  "-a --add -d --del -l --list");

	command_add(NULL, TEXT("unignore"), "i ?", cmd_ignore, 0, NULL);
	  
	command_add(NULL, TEXT("version"), NULL, cmd_version, 0, NULL);
	  
	command_add(NULL, TEXT("window"), "p ? p", cmd_window, 0,
	  "active clear kill last list move new next prev switch refresh left right");

	command_add(NULL, TEXT("_watches"), NULL, cmd_debug_watches, 0,NULL);
 
	command_add(NULL, TEXT("_queries"), NULL, cmd_debug_queries, 0, NULL);

	command_add(NULL, TEXT("_query"), "? ? ? ? ? ? ? ? ? ?", cmd_debug_query, 0,NULL); 

	command_add(NULL, TEXT("_addtab"), "!", cmd_test_addtab, COMMAND_ENABLEREQPARAMS, NULL);
 
	command_add(NULL, TEXT("_deltab"), "!", cmd_test_deltab, COMMAND_ENABLEREQPARAMS, NULL);

	command_add(NULL, TEXT("_fds"), NULL, cmd_test_fds, 0, NULL);

	command_add(NULL, TEXT("_streams"), "p ? ? ? ?", cmd_streams, 0, "-c --create -l --list");

	command_add(NULL, TEXT("_mem"), NULL, cmd_test_mem, 0, NULL);

	command_add(NULL, TEXT("_msg"), "uUC ?", cmd_test_send, 0, NULL);

	command_add(NULL, TEXT("_segv"), NULL, cmd_test_segv, 0, NULL);
 
	command_add(NULL, TEXT("_debug"), "!", cmd_test_debug, COMMAND_ENABLEREQPARAMS, NULL);
 
	command_add(NULL, TEXT("_debug_dump"), NULL, cmd_test_debug_dump, 0, NULL);
 
	command_add(NULL, TEXT("_event_test"), "!", cmd_test_event_test, COMMAND_ENABLEREQPARAMS, NULL);

	command_add(NULL, TEXT("_desc"), "r", cmd_desc, 0, NULL);
}

/*
 * command_free()
 *
 * usuwa list� komend z pami�ci.
 */
void command_free()
{
	list_t l;

	for (l = commands; l; l = l->next) {
		command_t *c = l->data;

		xfree(c->name);
		wcs_array_free(c->params);
		array_free(c->possibilities);
	}

	list_destroy(commands, 1);
	commands = NULL;
}

/*
 * Local Variables:
 * mode: c
 * c-file-style: "k&r"
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 * vim: sts=8 sw=8
 */
