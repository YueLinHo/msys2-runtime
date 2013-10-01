/*-
 * Copyright (c) 2008 Ed Schouten <ed@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
FUNCTION
<<posix_spawn>>, <<posix_spawnp>>---spawn a process

INDEX
	posix_spawn
INDEX
	posix_spawnp

ANSI_SYNOPSIS
	#include <spawn.h>

	int posix_spawn(pid_t *<[pid]>, const char *<[path]>,
			const posix_spawn_file_actions_t *<[file_actions]>,
			const posix_spawnattr_t *<[attrp]>,
			char *const <[argv]>, char *const <[envp]>);
	int posix_spawnp(pid_t *<[pid]>, const char *<[file]>,
			const posix_spawn_file_actions_t *<[file_actions]>,
			const posix_spawnattr_t *<[attrp]>,
			char *const <[argv]>, char *const <[envp]>);

DESCRIPTION
Use <<posix_spawn>> and <<posix_spawnp>> to create a new child process
from the specified process image file. <<argc>> is the argument count
and <<argv>> is an array of argument strings passed to the new program.
<<envp>> is an array of stings, which are passed as environment to the
new program.

The <<path>> argument to <<posix_spawn>> identifies the new process
image file to execute. The <<file>> argument to <<posix_spawnp>> is
used to construct a pathname that identifies the new process image
file by duplicating the actions of the shell in searching for an
executable file if the specified filename does not contain a `<</>>'
character. The <<file>> is sought in the colon-separated list of
directory pathnames specified in the <<PATH>> environment variable.

The file descriptors remain open across <<posix_spawn>> and
<<posix_spawnp>> except for those marked as close-on-exec. The open
file descriptors in the child process can be modified by the spawn file
actions object pointed to by <<file_actions>>.

The spawn attributes object type pointed to by <<attrp>> argument
may contain any of the attributes defined in <<spawn.h>>.

RETURNS
<<posix_spawn>> and <<posix_spawnp>> return the process ID of the newly
spawned child process in the variable pointed by a non-NULL <<*<[pid]>>>
argument and zero as the function return value upon successful
completion. Otherwise, <<posix_spawn>> and <<posix_spawnp>> return an
error number as the function return value to indicate the error; the
value stored into the variable pointed to by a non-NULL <<*<[pid]>>>
argument is unspecified.

PORTABILITY
POSIX.1-2008 requires <<posix_spawn>> and <<posix_spawnp>>.

Supporting OS subroutines required: <<_close>>, <<_dup2>>, <<_fcntl>>,
<<_execve>>, <<_execvpe>>, <<_exit>>, <<_open>>, <<_sigaction>>,
<<_sigprocmask>>, <<_waitpid>>, <<sched_setscheduler>>,
<<sched_setparam>>, <<setegid>>, <<seteuid>>, <<setpgid>>, <<vfork>>.
*/

#ifndef _NO_POSIX_SPAWN

#include <sys/cdefs.h>

#include "namespace.h"
#include <sys/signal.h>
#include <sys/queue.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <spawn.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "un-namespace.h"

/* Only deal with a pointer to environ, to work around subtle bugs with shared
   libraries and/or small data systems where the user declares his own
   'environ'.  */
static char ***p_environ = &environ;

struct __posix_spawnattr {
	short			sa_flags;
	pid_t			sa_pgroup;
	struct sched_param	sa_schedparam;
	int			sa_schedpolicy;
	sigset_t		sa_sigdefault;
	sigset_t		sa_sigmask;
};

struct __posix_spawn_file_actions {
	STAILQ_HEAD(, __posix_spawn_file_actions_entry) fa_list;
};

typedef struct __posix_spawn_file_actions_entry {
	STAILQ_ENTRY(__posix_spawn_file_actions_entry) fae_list;
	enum { FAE_OPEN, FAE_DUP2, FAE_CLOSE } fae_action;

	int fae_fildes;
	union {
		struct {
			char *path;
#define fae_path	fae_data.open.path
			int oflag;
#define fae_oflag	fae_data.open.oflag
			mode_t mode;
#define fae_mode	fae_data.open.mode
		} open;
		struct {
			int newfildes;
#define fae_newfildes	fae_data.dup2.newfildes
		} dup2;
	} fae_data;
} posix_spawn_file_actions_entry_t;

/*
 * Spawn routines
 */

static int
process_spawnattr(_CONST posix_spawnattr_t sa)
{
	struct sigaction sigact = { .sa_flags = 0, .sa_handler = SIG_DFL };
	int i;

	/*
	 * POSIX doesn't really describe in which order everything
	 * should be set. We'll just set them in the order in which they
	 * are mentioned.
	 */

	/* Set process group */
	if (sa->sa_flags & POSIX_SPAWN_SETPGROUP) {
		if (setpgid(0, sa->sa_pgroup) != 0)
			return (errno);
	}

	/* Set scheduler policy */
	if (sa->sa_flags & POSIX_SPAWN_SETSCHEDULER) {
		if (sched_setscheduler(0, sa->sa_schedpolicy,
		    &sa->sa_schedparam) != 0)
			return (errno);
	} else if (sa->sa_flags & POSIX_SPAWN_SETSCHEDPARAM) {
		if (sched_setparam(0, &sa->sa_schedparam) != 0)
			return (errno);
	}

	/* Reset user ID's */
	if (sa->sa_flags & POSIX_SPAWN_RESETIDS) {
		if (setegid(getgid()) != 0)
			return (errno);
		if (seteuid(getuid()) != 0)
			return (errno);
	}

	/* Set signal masks/defaults */
	if (sa->sa_flags & POSIX_SPAWN_SETSIGMASK) {
		_sigprocmask(SIG_SETMASK, &sa->sa_sigmask, NULL);
	}

	if (sa->sa_flags & POSIX_SPAWN_SETSIGDEF) {
		for (i = 1; i < NSIG; i++) {
			if (sigismember(&sa->sa_sigdefault, i))
				if (_sigaction(i, &sigact, NULL) != 0)
					return (errno);
		}
	}

	return (0);
}

static int
process_file_actions_entry(posix_spawn_file_actions_entry_t *fae)
{
	int fd;

	switch (fae->fae_action) {
	case FAE_OPEN:
		/* Perform an open(), make it use the right fd */
		fd = _open(fae->fae_path, fae->fae_oflag, fae->fae_mode);
		if (fd < 0)
			return (errno);
		if (fd != fae->fae_fildes) {
			if (_dup2(fd, fae->fae_fildes) == -1)
				return (errno);
			if (_close(fd) != 0) {
				if (errno == EBADF)
					return (EBADF);
			}
		}
#ifdef HAVE_FCNTL
		if (_fcntl(fae->fae_fildes, F_SETFD, 0) == -1)
			return (errno);
#endif /* HAVE_FCNTL */
		break;
	case FAE_DUP2:
		/* Perform a dup2() */
		if (_dup2(fae->fae_fildes, fae->fae_newfildes) == -1)
			return (errno);
#ifdef HAVE_FCNTL
		if (_fcntl(fae->fae_newfildes, F_SETFD, 0) == -1)
			return (errno);
#endif /* HAVE_FCNTL */
		break;
	case FAE_CLOSE:
		/* Perform a close(), do not fail if already closed */
		(void)_close(fae->fae_fildes);
		break;
	}
	return (0);
}

static int
process_file_actions(_CONST posix_spawn_file_actions_t fa)
{
	posix_spawn_file_actions_entry_t *fae;
	int error;

	/* Replay all file descriptor modifications */
	STAILQ_FOREACH(fae, &fa->fa_list, fae_list) {
		error = process_file_actions_entry(fae);
		if (error)
			return (error);
	}
	return (0);
}

static int
do_posix_spawn(pid_t *pid, _CONST char *path,
	_CONST posix_spawn_file_actions_t *fa,
	_CONST posix_spawnattr_t *sa,
	char * _CONST argv[], char * _CONST envp[], int use_env_path)
{
	pid_t p;
	volatile int error = 0;

	p = vfork();
	switch (p) {
	case -1:
		return (errno);
	case 0:
		if (sa != NULL) {
			error = process_spawnattr(*sa);
			if (error)
				_exit(127);
		}
		if (fa != NULL) {
			error = process_file_actions(*fa);
			if (error)
				_exit(127);
		}
		if (use_env_path)
			_execvpe(path, argv, envp != NULL ? envp : *p_environ);
		else
			_execve(path, argv, envp != NULL ? envp : *p_environ);
		error = errno;
		_exit(127);
	default:
		if (error != 0)
			_waitpid(p, NULL, WNOHANG);
		else if (pid != NULL)
			*pid = p;
		return (error);
	}
}

int
_DEFUN(posix_spawn, (pid, path, fa, sa, argv, envp),
	pid_t *pid _AND
	_CONST char *path _AND
	_CONST posix_spawn_file_actions_t *fa _AND
	_CONST posix_spawnattr_t *sa _AND
	char * _CONST argv[] _AND
	char * _CONST envp[])
{
	return do_posix_spawn(pid, path, fa, sa, argv, envp, 0);
}

int
_DEFUN(posix_spawnp, (pid, path, fa, sa, argv, envp),
	pid_t *pid _AND
	_CONST char *path _AND
	_CONST posix_spawn_file_actions_t *fa _AND
	_CONST posix_spawnattr_t *sa _AND
	char * _CONST argv[] _AND
	char * _CONST envp[])
{
	return do_posix_spawn(pid, path, fa, sa, argv, envp, 1);
}

/*
 * File descriptor actions
 */

int
_DEFUN(posix_spawn_file_actions_init, (ret),
	posix_spawn_file_actions_t *ret)
{
	posix_spawn_file_actions_t fa;

	fa = malloc(sizeof(struct __posix_spawn_file_actions));
	if (fa == NULL)
		return (-1);

	STAILQ_INIT(&fa->fa_list);
	*ret = fa;
	return (0);
}

int
_DEFUN(posix_spawn_file_action_destroy, (fa),
	posix_spawn_file_actions_t *fa)
{
	posix_spawn_file_actions_entry_t *fae;

	while ((fae = STAILQ_FIRST(&(*fa)->fa_list)) != NULL) {
		/* Remove file action entry from the queue */
		STAILQ_REMOVE_HEAD(&(*fa)->fa_list, fae_list);

		/* Deallocate file action entry */
		if (fae->fae_action == FAE_OPEN)
			free(fae->fae_path);
		free(fae);
	}

	free(*fa);
	return (0);
}

int
_DEFUN(posix_spawn_file_actions_addopen, (fa, fildes, path, oflag, mode),
	posix_spawn_file_actions_t * __restrict fa _AND
	int fildes _AND
	_CONST char * __restrict path _AND
	int oflag _AND
	mode_t mode)
{
	posix_spawn_file_actions_entry_t *fae;
	int error;

	if (fildes < 0)
		return (EBADF);

	/* Allocate object */
	fae = malloc(sizeof(posix_spawn_file_actions_entry_t));
	if (fae == NULL)
		return (errno);

	/* Set values and store in queue */
	fae->fae_action = FAE_OPEN;
	fae->fae_path = strdup(path);
	if (fae->fae_path == NULL) {
		error = errno;
		free(fae);
		return (error);
	}
	fae->fae_fildes = fildes;
	fae->fae_oflag = oflag;
	fae->fae_mode = mode;

	STAILQ_INSERT_TAIL(&(*fa)->fa_list, fae, fae_list);
	return (0);
}

int
_DEFUN(posix_spawn_file_actions_adddup2, (fa, fildes, newfildes),
	posix_spawn_file_actions_t *fa _AND
	int fildes _AND
	int newfildes)
{
	posix_spawn_file_actions_entry_t *fae;

	if (fildes < 0 || newfildes < 0)
		return (EBADF);

	/* Allocate object */
	fae = malloc(sizeof(posix_spawn_file_actions_entry_t));
	if (fae == NULL)
		return (errno);

	/* Set values and store in queue */
	fae->fae_action = FAE_DUP2;
	fae->fae_fildes = fildes;
	fae->fae_newfildes = newfildes;

	STAILQ_INSERT_TAIL(&(*fa)->fa_list, fae, fae_list);
	return (0);
}

int
_DEFUN(posix_spawn_file_actions_addclose, (fa, fildes),
	posix_spawn_file_actions_t *fa _AND
	int fildes)
{
	posix_spawn_file_actions_entry_t *fae;

	if (fildes < 0)
		return (EBADF);

	/* Allocate object */
	fae = malloc(sizeof(posix_spawn_file_actions_entry_t));
	if (fae == NULL)
		return (errno);

	/* Set values and store in queue */
	fae->fae_action = FAE_CLOSE;
	fae->fae_fildes = fildes;

	STAILQ_INSERT_TAIL(&(*fa)->fa_list, fae, fae_list);
	return (0);
}

/*
 * Spawn attributes
 */

int
_DEFUN(posix_spawnattr_init, (ret),
	posix_spawnattr_t *ret)
{
	posix_spawnattr_t sa;

	sa = calloc(1, sizeof(struct __posix_spawnattr));
	if (sa == NULL)
		return (errno);

	/* Set defaults as specified by POSIX, cleared above */
	*ret = sa;
	return (0);
}

int
_DEFUN(posix_spawnattr_destroy, (sa),
	posix_spawnattr_t *sa)
{
	free(*sa);
	return (0);
}

int
_DEFUN(posix_spawnattr_getflags, (sa, flags),
	_CONST posix_spawnattr_t * __restrict sa _AND
	short * __restrict flags)
{
	*flags = (*sa)->sa_flags;
	return (0);
}

int
_DEFUN(posix_spawnattr_getpgroup, (sa, pgroup),
	_CONST posix_spawnattr_t * __restrict sa _AND
	pid_t * __restrict pgroup)
{
	*pgroup = (*sa)->sa_pgroup;
	return (0);
}

int
_DEFUN(posix_spawnattr_getschedparam, (sa, schedparam),
	_CONST posix_spawnattr_t * __restrict sa _AND
	struct sched_param * __restrict schedparam)
{
	*schedparam = (*sa)->sa_schedparam;
	return (0);
}

int
_DEFUN(posix_spawnattr_getschedpolicy, (sa, schedpolicy),
	_CONST posix_spawnattr_t * __restrict sa _AND
	int * __restrict schedpolicy)
{
	*schedpolicy = (*sa)->sa_schedpolicy;
	return (0);
}

int
_DEFUN(posix_spawnattr_getsigdefault, (sa, sigdefault),
	_CONST posix_spawnattr_t * __restrict sa _AND
	sigset_t * __restrict sigdefault)
{
	*sigdefault = (*sa)->sa_sigdefault;
	return (0);
}

int
_DEFUN(posix_spawnattr_getsigmask, (sa, sigmask),
	_CONST posix_spawnattr_t * __restrict sa _AND
	sigset_t * __restrict sigmask)
{
	*sigmask = (*sa)->sa_sigmask;
	return (0);
}

int
_DEFUN(posix_spawnattr_setflags, (sa, flags),
	posix_spawnattr_t *sa _AND
	short flags)
{
	(*sa)->sa_flags = flags;
	return (0);
}

int
_DEFUN(posix_spawnattr_setpgroup, (sa, pgroup),
	posix_spawnattr_t *sa _AND
	pid_t pgroup)
{
	(*sa)->sa_pgroup = pgroup;
	return (0);
}

int
_DEFUN(posix_spawnattr_setschedparam, (sa, schedparam),
	posix_spawnattr_t * __restrict sa _AND
	_CONST struct sched_param * __restrict schedparam)
{
	(*sa)->sa_schedparam = *schedparam;
	return (0);
}

int
_DEFUN(posix_spawnattr_setschedpolicy, (sa, schedpolicy),
	posix_spawnattr_t *sa _AND
	int schedpolicy)
{
	(*sa)->sa_schedpolicy = schedpolicy;
	return (0);
}

int
_DEFUN(posix_spawnattr_setsigdefault, (sa, sigdefault),
	posix_spawnattr_t * __restrict sa _AND
	_CONST sigset_t * __restrict sigdefault)
{
	(*sa)->sa_sigdefault = *sigdefault;
	return (0);
}

int
_DEFUN(posix_spawnattr_setsigmask, (sa, sigmask),
	posix_spawnattr_t * __restrict sa _AND
	_CONST sigset_t * __restrict sigmask)
{
	(*sa)->sa_sigmask = *sigmask;
	return (0);
}

#endif  /* !_NO_POSIX_SPAWN */
