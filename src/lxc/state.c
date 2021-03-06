/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <daniel.lezcano at free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cgroup.h"
#include "commands.h"
#include "config.h"
#include "log.h"
#include "lxc.h"
#include "monitor.h"
#include "start.h"

lxc_log_define(lxc_state, lxc);

static const char * const strstate[] = {
	"STOPPED", "STARTING", "RUNNING", "STOPPING",
	"ABORTING", "FREEZING", "FROZEN", "THAWED",
};

const char *lxc_state2str(lxc_state_t state)
{
	if (state < STOPPED || state > MAX_STATE - 1)
		return NULL;
	return strstate[state];
}

lxc_state_t lxc_str2state(const char *state)
{
	size_t len;
	lxc_state_t i;
	len = sizeof(strstate)/sizeof(strstate[0]);
	for (i = 0; i < len; i++)
		if (!strcmp(strstate[i], state))
			return i;

	ERROR("invalid state '%s'", state);
	return -1;
}

lxc_state_t lxc_getstate(const char *name, const char *lxcpath)
{
	extern lxc_state_t freezer_state(const char *name, const char *lxcpath);

	lxc_state_t state = freezer_state(name, lxcpath);
	if (state != FROZEN && state != FREEZING)
		state = lxc_cmd_get_state(name, lxcpath);
	return state;
}

static int fillwaitedstates(const char *strstates, int *states)
{
	char *token, *saveptr = NULL;
	char *strstates_dup = strdup(strstates);
	int state;

	if (!strstates_dup)
		return -1;

	token = strtok_r(strstates_dup, "|", &saveptr);
	while (token) {

		state = lxc_str2state(token);
		if (state < 0) {
			free(strstates_dup);
			return -1;
		}

		states[state] = 1;

		token = strtok_r(NULL, "|", &saveptr);
	}
	free(strstates_dup);
	return 0;
}

extern int lxc_wait(const char *lxcname, const char *states, int timeout,
		    const char *lxcpath)
{
	struct lxc_msg msg;
	int state, ret;
	int s[MAX_STATE] = {0}, fd;

	if (fillwaitedstates(states, s))
		return -1;

	if (lxc_monitord_spawn(lxcpath))
		return -1;

	fd = lxc_monitor_open(lxcpath);
	if (fd < 0)
		return -1;

	/*
	 * if container present,
	 * then check if already in requested state
	 */
	ret = -1;
	state = lxc_getstate(lxcname, lxcpath);
	if (state < 0) {
		goto out_close;
	} else if ((state >= 0) && (s[state])) {
		ret = 0;
		goto out_close;
	}

	for (;;) {
		int64_t elapsed_time, curtime = 0;
		struct timespec tspec;
		int stop = 0;
		int retval;

		if (timeout != -1) {
			retval = clock_gettime(CLOCK_REALTIME, &tspec);
			if (retval)
				goto out_close;
			curtime = tspec.tv_sec;
		}
		if (lxc_monitor_read_timeout(fd, &msg, timeout) < 0) {
			/* try again if select interrupted by signal */
			if (errno != EINTR)
				goto out_close;
		}

		if (timeout != -1) {
			retval = clock_gettime(CLOCK_REALTIME, &tspec);
			if (retval)
				goto out_close;
			elapsed_time = tspec.tv_sec - curtime;
			if (timeout - elapsed_time <= 0)
				stop = 1;
			timeout -= elapsed_time;
		}

		if (strcmp(lxcname, msg.name)) {
			if (stop) {
				ret = -2;
				goto out_close;
			}
			continue;
		}

		switch (msg.type) {
		case lxc_msg_state:
			if (msg.value < 0 || msg.value >= MAX_STATE)
				goto out_close;

			if (s[msg.value]) {
				ret = 0;
				goto out_close;
			}
			break;
		default:
			if (stop) {
				ret = -2;
				goto out_close;
			}
			/* just ignore garbage */
			break;
		}
	}

out_close:
	lxc_monitor_close(fd);
	return ret;
}
