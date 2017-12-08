/*
 * Copyright (C) 2017 OpenSIPS Project
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */


#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "ipc.h"
#include "dprint.h"
#include "mem/mem.h"

#include <fcntl.h>

#define IPC_HANDLER_NAME_MAX  32
typedef struct _ipc_handler {
	/* handler function */
	ipc_handler_f *func;
	/* same name/description, null terminated */
	char name[IPC_HANDLER_NAME_MAX+1];
} ipc_handler;

typedef struct _ipc_job {
	/* the ID (internal) of the process sending the job */
	int snd_proc;
	/* the job's handler type */
	ipc_handler_type handler_type;
	/* the payload of the job, just a pointer */
	void *payload;
} ipc_job;

static ipc_handler *ipc_handlers = NULL;
static unsigned int ipc_handlers_no = 0;

/* shared IPC support: dispatching a job to a random OpenSIPS worker */
static int ipc_shared_pipe[2];

int ipc_shared_fd_read;

int init_ipc(void)
{
	int optval;

	/* create the pipe for dispatching the timer jobs */
	if (pipe(ipc_shared_pipe) != 0) {
		LM_ERR("failed to create ipc pipe (%s)!\n", strerror(errno));
		return -1;
	}

	/* make reading fd non-blocking */
	optval = fcntl(ipc_shared_pipe[0], F_GETFL);
	if (optval == -1) {
		LM_ERR("fcntl failed: (%d) %s\n", errno, strerror(errno));
		return -1;
	}

	if (fcntl(ipc_shared_pipe[0], F_SETFL, optval|O_NONBLOCK) == -1) {
		LM_ERR("set non-blocking failed: (%d) %s\n", errno, strerror(errno));
		return -1;
	}

	ipc_shared_fd_read = ipc_shared_pipe[0];

	return 0;
}

ipc_handler_type ipc_register_handler( ipc_handler_f *hdl, char *name)
{
	ipc_handler *new;

	/* allocate an n+1 new buffer to accomodate the new handler */
	new = (ipc_handler*)
		pkg_malloc( (ipc_handlers_no+1)*sizeof(ipc_handler) );
	if (new==NULL) {
		LM_ERR("failed to alloctes IPC handler array for size %d\n",
			ipc_handlers_no+1);
		return -1;
	}

	/* copy previous records, if any */
	if (ipc_handlers) {
		memcpy( new, ipc_handlers, ipc_handlers_no*sizeof(ipc_handler) );
		pkg_free( ipc_handlers );
	}

	/* copy handler function */
	new[ipc_handlers_no].func = hdl;

	/* copy the name, trunkate it needed, but keep it null terminated */
	strncpy( new[ipc_handlers_no].name , name, IPC_HANDLER_NAME_MAX);
	new[ipc_handlers_no].name[IPC_HANDLER_NAME_MAX] = 0;

	ipc_handlers = new;

	LM_DBG("IPC type %d [%s] registered with handler %p\n",
		ipc_handlers_no, ipc_handlers[ipc_handlers_no].name, hdl );

	return ipc_handlers_no++;
}

static inline int __ipc_send_job(int fd, ipc_handler_type type, void *payload)
{
	ipc_job job;
	int n;

	// FIXME - we should check if the destination process really listens
	// for read, otherwise we may end up filling in the pipe and block

	job.snd_proc = process_no;
	job.handler_type = type;
	job.payload = payload;

again:
	// TODO - should we do this non blocking and discard if we block ??
	n = write(fd, &job, sizeof(job) );
	if (n<0) {
		if (errno==EINTR)
			goto again;
		LM_ERR("sending job type %d[%s] on %d failed: %s\n",
			type, ipc_handlers[type].name, fd, strerror(errno));
		return -1;
	}
	return 0;
}

int ipc_send_job(int dst_proc, ipc_handler_type type, void *payload)
{
	return __ipc_send_job(IPC_FD_WRITE(dst_proc), type, payload);
}

int ipc_dispatch_job(ipc_handler_type type, void *payload)
{
	return __ipc_send_job(ipc_shared_pipe[1], type, payload);
}

void ipc_handle_job(void)
{
	ipc_job job;
	int n;

	/* read one IPC job from the pipe; even if the read is blocking,
	 * we are here triggered from the reactor, on a READ event, so 
	 * we shouldn;t ever block */
	n = read( IPC_FD_READ_SELF, &job, sizeof(job) );
	if (n==-1) {
		if (errno==EAGAIN || errno==EINTR || errno==EWOULDBLOCK )
			return;
		LM_ERR("read failed:[%d] %s\n", errno, strerror(errno));
		return;
	}

	LM_DBG("received job type %d[%s] from process %d\n",
		job.handler_type, ipc_handlers[job.handler_type].name, job.snd_proc);

	ipc_handlers[job.handler_type].func( job.snd_proc, job.payload );

	return;
}

