/*
 * Copyright (c) 2002-2004 MontaVista Software, Inc.
 *
 * All rights reserved.
 *
 * Author: Steven Dake (sdake@mvista.com)
 *
 * This software licensed under BSD license, the text of which follows:
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>

#include "util.h"

#include "../include/list.h"
#include "../include/saCkpt.h"
#include "../include/ipc_gen.h"
#include "../include/ipc_ckpt.h"

struct message_overlay {
	struct res_header header;
	char data[4096];
};

/*
 * Data structure for instance data
 */
struct ckptInstance {
	int response_fd;
	int dispatch_fd;
	SaCkptCallbacksT callbacks;
	int finalize;
	pthread_mutex_t response_mutex;
	pthread_mutex_t dispatch_mutex;
	struct list_head checkpoint_list;
};

struct ckptCheckpointInstance {
	int response_fd;
	SaCkptHandleT ckptHandle;
	SaCkptCheckpointHandleT checkpointHandle;
	SaCkptCheckpointOpenFlagsT checkpointOpenFlags;
	SaNameT checkpointName;
	pthread_mutex_t response_mutex;
	struct list_head list;
};

struct ckptSectionIterationInstance {
	int response_fd;
	SaCkptCheckpointHandleT checkpointHandle;
	struct list_head sectionIdListHead;
	pthread_mutex_t response_mutex;
};

void ckptHandleInstanceDestructor (void *instance);
void checkpointHandleInstanceDestructor (void *instance);
void ckptSectionIterationHandleInstanceDestructor (void *instance);

/*
 * All CKPT instances in this database
 */
static struct saHandleDatabase ckptHandleDatabase = {
	.handleCount				= 0,
	.handles					= 0,
	.mutex						= PTHREAD_MUTEX_INITIALIZER,
	.handleInstanceDestructor	= ckptHandleInstanceDestructor
};

/*
 *  All Checkpoint instances in this database
 */
static struct saHandleDatabase checkpointHandleDatabase = {
	.handleCount				= 0,
	.handles					= 0,
	.mutex						= PTHREAD_MUTEX_INITIALIZER,
	.handleInstanceDestructor	= checkpointHandleInstanceDestructor
};

/*
 * All section iterators in this database
 */
static struct saHandleDatabase ckptSectionIterationHandleDatabase = {
	.handleCount				= 0,
	.handles					= 0,
	.mutex						= PTHREAD_MUTEX_INITIALIZER,
	.handleInstanceDestructor	= ckptSectionIterationHandleInstanceDestructor
};

/*
 * Versions supported
 */
static SaVersionT ckptVersionsSupported[] = {
	{ 'B', 1, 1 }
};

static struct saVersionDatabase ckptVersionDatabase = {
	sizeof (ckptVersionsSupported) / sizeof (SaVersionT),
	ckptVersionsSupported
};


/*
 * Implementation
 */
void ckptHandleInstanceDestructor (void *instance)
{
struct ckptInstance *ckptInstance = (struct ckptInstance *)instance;

}

void checkpointHandleInstanceDestructor (void *instance)
{
	return;
}

void ckptSectionIterationHandleInstanceDestructor (void *instance)
{
	struct ckptSectionIterationInstance *ckptSectionIterationInstance = (struct ckptSectionIterationInstance *)instance;

	if (ckptSectionIterationInstance->response_fd != -1) {
		shutdown (ckptSectionIterationInstance->response_fd, 0);

		close (ckptSectionIterationInstance->response_fd);
	}
}

SaAisErrorT
saCkptInitialize (
	SaCkptHandleT *ckptHandle,
	const SaCkptCallbacksT *callbacks,
	SaVersionT *version)
{
	struct ckptInstance *ckptInstance;
	SaAisErrorT error = SA_AIS_OK;

	if (ckptHandle == NULL) {
		return (SA_AIS_ERR_INVALID_PARAM);
	}

	error = saVersionVerify (&ckptVersionDatabase, version);
	if (error != SA_AIS_OK) {
		goto error_no_destroy;
	}

	error = saHandleCreate (&ckptHandleDatabase, sizeof (struct ckptInstance),
		ckptHandle);
	if (error != SA_AIS_OK) {
		goto error_no_destroy;
	}

	error = saHandleInstanceGet (&ckptHandleDatabase, *ckptHandle,
		(void *)&ckptInstance);
	if (error != SA_AIS_OK) {
		goto error_destroy;
	}

	ckptInstance->response_fd = -1;

	error = saServiceConnectTwo (&ckptInstance->response_fd,
		&ckptInstance->dispatch_fd, CKPT_SERVICE);
	if (error != SA_AIS_OK) {
		goto error_put_destroy;
	}

	if (callbacks) {
		memcpy (&ckptInstance->callbacks, callbacks, sizeof (SaCkptCallbacksT));
	} else {
		memset (&ckptInstance->callbacks, 0, sizeof (SaCkptCallbacksT));
	}

	list_init (&ckptInstance->checkpoint_list);

	pthread_mutex_init (&ckptInstance->response_mutex, NULL);

	saHandleInstancePut (&ckptHandleDatabase, *ckptHandle);

	return (SA_AIS_OK);

error_put_destroy:
	saHandleInstancePut (&ckptHandleDatabase, *ckptHandle);
error_destroy:
	saHandleDestroy (&ckptHandleDatabase, *ckptHandle);
error_no_destroy:
	return (error);
}

SaAisErrorT
saCkptSelectionObjectGet (
	const SaCkptHandleT ckptHandle,
	SaSelectionObjectT *selectionObject)
{
	struct ckptInstance *ckptInstance;
	SaAisErrorT error;

	if (selectionObject == NULL) {
		return (SA_AIS_ERR_INVALID_PARAM);
	}
	error = saHandleInstanceGet (&ckptHandleDatabase, ckptHandle, (void *)&ckptInstance);
	if (error != SA_AIS_OK) {
		return (error);
	}

	*selectionObject = ckptInstance->dispatch_fd;

	saHandleInstancePut (&ckptHandleDatabase, ckptHandle);

	return (SA_AIS_OK);
}

SaAisErrorT
saCkptDispatch (
	const SaCkptHandleT ckptHandle,
	SaDispatchFlagsT dispatchFlags)
{
	struct pollfd ufds;
	int poll_fd;
	int timeout = 1;
	SaCkptCallbacksT callbacks;
	SaAisErrorT error;
	int dispatch_avail;
	struct ckptInstance *ckptInstance;
	int cont = 1; /* always continue do loop except when set to 0 */
	struct message_overlay dispatch_data;
	struct res_lib_ckpt_checkpointopenasync *res_lib_ckpt_checkpointopenasync;
	struct ckptCheckpointInstance *ckptCheckpointInstance;

	if (dispatchFlags != SA_DISPATCH_ONE &&
		dispatchFlags != SA_DISPATCH_ALL &&
		dispatchFlags != SA_DISPATCH_BLOCKING) {

		return (SA_AIS_ERR_INVALID_PARAM);
	}

	error = saHandleInstanceGet (&ckptHandleDatabase, ckptHandle,
		(void *)&ckptInstance);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	/*
	 * Timeout instantly for SA_DISPATCH_ALL
	 */
	if (dispatchFlags == SA_DISPATCH_ALL) {
		timeout = 0;
	}

	do {
		/*
		 * Read data directly from socket
		 */
		poll_fd = ckptInstance->dispatch_fd;
		ufds.fd = poll_fd;
		ufds.events = POLLIN;
		ufds.revents = 0;

		error = saPollRetry(&ufds, 1, timeout);
		if (error != SA_AIS_OK) {
			goto error_put;
		}
		pthread_mutex_lock(&ckptInstance->dispatch_mutex);

		if (ckptInstance->finalize == 1) {
			error = SA_AIS_OK;
			goto error_unlock;
		}

		if ((ufds.revents & (POLLERR|POLLHUP|POLLNVAL)) != 0) {
				error = SA_AIS_ERR_BAD_HANDLE;
				goto error_unlock;
		}
		
		dispatch_avail = (ufds.revents & POLLIN);

		if (dispatch_avail == 0 && dispatchFlags == SA_DISPATCH_ALL) {
			pthread_mutex_unlock(&ckptInstance->dispatch_mutex);
			break; /* exit do while cont is 1 loop */
		} else
		if (dispatch_avail == 0) {
			pthread_mutex_unlock(&ckptInstance->dispatch_mutex);
			continue;
		}
		
		memset(&dispatch_data,0, sizeof(struct message_overlay));
		error = saRecvRetry (ckptInstance->dispatch_fd, &dispatch_data.header, sizeof (struct res_header), MSG_WAITALL | MSG_NOSIGNAL);
		if (error != SA_AIS_OK) {
			goto error_unlock;
		}
		if (dispatch_data.header.size > sizeof (struct res_header)) {
			error = saRecvRetry (ckptInstance->dispatch_fd, &dispatch_data.data,
				dispatch_data.header.size - sizeof (struct res_header),
				MSG_WAITALL | MSG_NOSIGNAL);
			if (error != SA_AIS_OK) {
				goto error_unlock;
			}
		}

		/*
		* Make copy of callbacks, message data, unlock instance,
		* and call callback. A risk of this dispatch method is that
		* the callback routines may operate at the same time that
		* CkptFinalize has been called in another thread.
		*/
		memcpy(&callbacks,&ckptInstance->callbacks, sizeof(ckptInstance->callbacks));
		pthread_mutex_unlock(&ckptInstance->dispatch_mutex);
		/*
		 * Dispatch incoming response
		 */
		switch (dispatch_data.header.id) {
		case MESSAGE_RES_CKPT_CHECKPOINT_CHECKPOINTOPENASYNC:
			if (callbacks.saCkptCheckpointOpenCallback == NULL) {
				continue;
			}
			res_lib_ckpt_checkpointopenasync = (struct res_lib_ckpt_checkpointopenasync *) &dispatch_data;

			/*
			 * This instance get/listadd/put required so that close
			 * later has the proper list of checkpoints
			 */
			error = saHandleInstanceGet (&checkpointHandleDatabase,
				res_lib_ckpt_checkpointopenasync->checkpointHandle,
				(void *)&ckptCheckpointInstance);
			assert (error == SA_AIS_OK); /* should only be valid handles here */
			if (res_lib_ckpt_checkpointopenasync->header.error == SA_AIS_OK) {
				/*
				 * open succeeded without error
				 */
				list_init (&ckptCheckpointInstance->list);
				list_add (&ckptCheckpointInstance->list,
					&ckptInstance->checkpoint_list);

				callbacks.saCkptCheckpointOpenCallback(
					res_lib_ckpt_checkpointopenasync->invocation,
					res_lib_ckpt_checkpointopenasync->checkpointHandle,
					res_lib_ckpt_checkpointopenasync->header.error);
				saHandleInstancePut (&checkpointHandleDatabase,
					res_lib_ckpt_checkpointopenasync->checkpointHandle);
			} else {
				/*
				 * open failed with error
				 */
				saHandleInstancePut (&checkpointHandleDatabase,
					res_lib_ckpt_checkpointopenasync->checkpointHandle);
				saHandleDestroy (&checkpointHandleDatabase,
					res_lib_ckpt_checkpointopenasync->checkpointHandle);

				callbacks.saCkptCheckpointOpenCallback(
					res_lib_ckpt_checkpointopenasync->invocation,
					-1,
					res_lib_ckpt_checkpointopenasync->header.error);
			}
			break;
		default:
			/* TODO */
			break;
		}
		/*
		 * Determine if more messages should be processed
		 */
		switch (dispatchFlags) {
		case SA_DISPATCH_ONE:
				cont = 0;
			break;
		case SA_DISPATCH_ALL:
			break;
		case SA_DISPATCH_BLOCKING:
			break;
		}
	} while (cont);
error_unlock:
	pthread_mutex_unlock(&ckptInstance->dispatch_mutex);
error_put:
	saHandleInstancePut(&ckptHandleDatabase, ckptHandle);
error_exit:
	return (error);
}

SaAisErrorT
saCkptFinalize (
	const SaCkptHandleT ckptHandle)
{
	struct ckptInstance *ckptInstance;
	SaAisErrorT error;
	struct list_head *list;
	struct ckptCheckpointInstance *ckptCheckpointInstance;

	error = saHandleInstanceGet (&ckptHandleDatabase, ckptHandle,
		(void *)&ckptInstance);
	if (error != SA_AIS_OK) {
		return (error);
	}

	pthread_mutex_lock (&ckptInstance->response_mutex);

	/*
	 * Another thread has already started finalizing
	 */
	if (ckptInstance->finalize) {
		pthread_mutex_unlock (&ckptInstance->response_mutex);
		saHandleInstancePut (&ckptHandleDatabase, ckptHandle);
		return (SA_AIS_ERR_BAD_HANDLE);
	}

	ckptInstance->finalize = 1;

	pthread_mutex_unlock (&ckptInstance->response_mutex);

	for (list = ckptInstance->checkpoint_list.next;
		list != &ckptInstance->checkpoint_list;
		list = list->next) {

		ckptCheckpointInstance = list_entry (list,
			struct ckptCheckpointInstance, list);

		saHandleInstancePut (&checkpointHandleDatabase,
			ckptCheckpointInstance->checkpointHandle);
	}

	saHandleDestroy (&ckptHandleDatabase, ckptHandle);

	if (ckptInstance->response_fd != -1) {
		shutdown (ckptInstance->response_fd, 0);
		close (ckptInstance->response_fd);
	}
	if (ckptInstance->dispatch_fd != -1) {
		shutdown (ckptInstance->dispatch_fd, 0);
		close (ckptInstance->dispatch_fd);
	}
	saHandleInstancePut (&ckptHandleDatabase, ckptHandle);


	return (SA_AIS_OK);
}

SaAisErrorT
saCkptCheckpointOpen (
	SaCkptHandleT ckptHandle,
	const SaNameT *checkpointName,
	const SaCkptCheckpointCreationAttributesT *checkpointCreationAttributes,
	SaCkptCheckpointOpenFlagsT checkpointOpenFlags,
	SaTimeT timeout,
	SaCkptCheckpointHandleT *checkpointHandle)
{
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct ckptInstance *ckptInstance;
	struct req_lib_ckpt_checkpointopen req_lib_ckpt_checkpointopen;
	struct res_lib_ckpt_checkpointopen res_lib_ckpt_checkpointopen;

	if ((checkpointOpenFlags & SA_CKPT_CHECKPOINT_CREATE) && 
		checkpointCreationAttributes == NULL) {

		return (SA_AIS_ERR_INVALID_PARAM);
	}

	if (((checkpointOpenFlags & SA_CKPT_CHECKPOINT_CREATE) == 0) && 
		checkpointCreationAttributes != NULL) {

		return (SA_AIS_ERR_INVALID_PARAM);
	}

	error = saHandleInstanceGet (&ckptHandleDatabase, ckptHandle,
		(void *)&ckptInstance);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	error = saHandleCreate (&checkpointHandleDatabase,
		sizeof (struct ckptCheckpointInstance), checkpointHandle);
	if (error != SA_AIS_OK) {
		goto error_put_ckpt;
	}

	error = saHandleInstanceGet (&checkpointHandleDatabase,
		*checkpointHandle, (void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		goto error_destroy;
	}

	ckptCheckpointInstance->response_fd = ckptInstance->response_fd;

	ckptCheckpointInstance->ckptHandle = ckptHandle;
	ckptCheckpointInstance->checkpointHandle = *checkpointHandle;
	ckptCheckpointInstance->checkpointOpenFlags = checkpointOpenFlags;

	req_lib_ckpt_checkpointopen.header.size = sizeof (struct req_lib_ckpt_checkpointopen);
	req_lib_ckpt_checkpointopen.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_CHECKPOINTOPEN;
	memcpy (&req_lib_ckpt_checkpointopen.checkpointName, checkpointName, sizeof (SaNameT));
	memcpy (&ckptCheckpointInstance->checkpointName, checkpointName, sizeof (SaNameT));
	req_lib_ckpt_checkpointopen.checkpointCreationAttributesSet = 0;
	if (checkpointCreationAttributes) {
		memcpy (&req_lib_ckpt_checkpointopen.checkpointCreationAttributes,
			checkpointCreationAttributes,
			sizeof (SaCkptCheckpointCreationAttributesT));
		req_lib_ckpt_checkpointopen.checkpointCreationAttributesSet = 1;
	}
	req_lib_ckpt_checkpointopen.checkpointOpenFlags = checkpointOpenFlags;

	error = saSendRetry (ckptCheckpointInstance->response_fd, &req_lib_ckpt_checkpointopen,
		sizeof (struct req_lib_ckpt_checkpointopen), MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_put_destroy;
	}

	error = saRecvRetry (ckptCheckpointInstance->response_fd, &res_lib_ckpt_checkpointopen,
		sizeof (struct res_lib_ckpt_checkpointopen), MSG_WAITALL | MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_put_destroy;
	}
	
	if (res_lib_ckpt_checkpointopen.header.error != SA_AIS_OK) {
		error = res_lib_ckpt_checkpointopen.header.error;
		goto error_put_destroy;
	}

	pthread_mutex_init (&ckptCheckpointInstance->response_mutex, NULL);

	saHandleInstancePut (&checkpointHandleDatabase, *checkpointHandle);

	saHandleInstancePut (&ckptHandleDatabase, ckptHandle);

	list_init (&ckptCheckpointInstance->list);

	list_add (&ckptCheckpointInstance->list, &ckptInstance->checkpoint_list);
	return (error);

error_put_destroy:
	saHandleInstancePut (&checkpointHandleDatabase, *checkpointHandle);
error_destroy:
	saHandleDestroy (&checkpointHandleDatabase, *checkpointHandle);
error_put_ckpt:
	saHandleInstancePut (&ckptHandleDatabase, ckptHandle);
error_exit:
	return (error);
}

SaAisErrorT
saCkptCheckpointOpenAsync (
	const SaCkptHandleT ckptHandle,
	SaInvocationT invocation,	
	const SaNameT *checkpointName,
	const SaCkptCheckpointCreationAttributesT *checkpointCreationAttributes,
	SaCkptCheckpointOpenFlagsT checkpointOpenFlags)
{
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct ckptInstance *ckptInstance;
	SaCkptCheckpointHandleT checkpointHandle;
	SaAisErrorT error;
	struct req_lib_ckpt_checkpointopenasync req_lib_ckpt_checkpointopenasync;

	if ((checkpointOpenFlags & SA_CKPT_CHECKPOINT_CREATE) && 
		checkpointCreationAttributes == NULL) {

		return (SA_AIS_ERR_INVALID_PARAM);
	}

	if (((checkpointOpenFlags & SA_CKPT_CHECKPOINT_CREATE) == 0) && 
		checkpointCreationAttributes != NULL) {

		return (SA_AIS_ERR_INVALID_PARAM);
	}
	error = saHandleInstanceGet (&ckptHandleDatabase, ckptHandle,
		(void *)&ckptInstance);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	error = saHandleCreate (&checkpointHandleDatabase,
		sizeof (struct ckptCheckpointInstance), &checkpointHandle);
	if (error != SA_AIS_OK) {
		goto error_put_ckpt;
	}

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		goto error_destroy;
	}

	ckptCheckpointInstance->response_fd = ckptInstance->response_fd;
	ckptCheckpointInstance->ckptHandle = ckptHandle;
	ckptCheckpointInstance->checkpointHandle = checkpointHandle;
	ckptCheckpointInstance->checkpointOpenFlags = checkpointOpenFlags;
	memcpy (&ckptCheckpointInstance->checkpointName, checkpointName, sizeof (SaNameT));

	req_lib_ckpt_checkpointopenasync.header.size = sizeof (struct req_lib_ckpt_checkpointopenasync);
	req_lib_ckpt_checkpointopenasync.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_CHECKPOINTOPENASYNC;
	req_lib_ckpt_checkpointopenasync.invocation = invocation;
	memcpy (&req_lib_ckpt_checkpointopenasync.checkpointName, checkpointName, sizeof (SaNameT));
	req_lib_ckpt_checkpointopenasync.checkpointCreationAttributesSet = 0;
	if (checkpointCreationAttributes) {
		memcpy (&req_lib_ckpt_checkpointopenasync.checkpointCreationAttributes,
			checkpointCreationAttributes,
			sizeof (SaCkptCheckpointCreationAttributesT));
		req_lib_ckpt_checkpointopenasync.checkpointCreationAttributesSet = 1;
	}
	
	req_lib_ckpt_checkpointopenasync.checkpointOpenFlags = checkpointOpenFlags;
	req_lib_ckpt_checkpointopenasync.checkpointHandle = checkpointHandle;

	error = saSendRetry (ckptInstance->response_fd, &req_lib_ckpt_checkpointopenasync,
		sizeof (struct req_lib_ckpt_checkpointopenasync), MSG_NOSIGNAL);
	
	if (error != SA_AIS_OK) {
		goto error_put_destroy;
	}
	
	pthread_mutex_init (&ckptCheckpointInstance->response_mutex, NULL);

	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);

	saHandleInstancePut (&ckptHandleDatabase, ckptHandle);

	return (error);

error_put_destroy:
	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);
error_destroy:
	saHandleDestroy (&checkpointHandleDatabase, checkpointHandle);
error_put_ckpt:
	saHandleInstancePut (&ckptHandleDatabase, ckptHandle);
error_exit:
	return (error);
}

SaAisErrorT
saCkptCheckpointClose (
	SaCkptCheckpointHandleT checkpointHandle)
{
	struct req_lib_ckpt_checkpointclose req_lib_ckpt_checkpointclose;
	struct res_lib_ckpt_checkpointclose res_lib_ckpt_checkpointclose;
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	req_lib_ckpt_checkpointclose.header.size = sizeof (struct req_lib_ckpt_checkpointclose);
	req_lib_ckpt_checkpointclose.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_CHECKPOINTCLOSE;
	memcpy (&req_lib_ckpt_checkpointclose.checkpointName,
		&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));

	error = saSendRetry (ckptCheckpointInstance->response_fd, &req_lib_ckpt_checkpointclose,
		sizeof (struct req_lib_ckpt_checkpointclose), MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto exit_put;
	}

	error = saRecvRetry (ckptCheckpointInstance->response_fd, &res_lib_ckpt_checkpointclose,
		sizeof (struct res_lib_ckpt_checkpointclose), MSG_WAITALL | MSG_NOSIGNAL);


	list_del (&ckptCheckpointInstance->list);

	saHandleDestroy (&checkpointHandleDatabase, checkpointHandle);

exit_put:
	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);

error_exit:
	return (error);
}

SaAisErrorT
saCkptCheckpointUnlink (
	SaCkptHandleT ckptHandle,
	const SaNameT *checkpointName)
{
	SaAisErrorT error;
	struct ckptInstance *ckptInstance;
	struct req_lib_ckpt_checkpointunlink req_lib_ckpt_checkpointunlink;
	struct res_lib_ckpt_checkpointunlink res_lib_ckpt_checkpointunlink;

	error = saHandleInstanceGet (&ckptHandleDatabase, ckptHandle, (void *)&ckptInstance);
	if (error != SA_AIS_OK) {
		goto exit_noclose;
	}

	req_lib_ckpt_checkpointunlink.header.size = sizeof (struct req_lib_ckpt_checkpointunlink);
	req_lib_ckpt_checkpointunlink.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_CHECKPOINTUNLINK;
	memcpy (&req_lib_ckpt_checkpointunlink.checkpointName, checkpointName, sizeof (SaNameT));


	error = saSendRetry (ckptInstance->response_fd, &req_lib_ckpt_checkpointunlink,
		sizeof (struct req_lib_ckpt_checkpointunlink), MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto exit_put;
	}

	error = saRecvRetry (ckptInstance->response_fd, &res_lib_ckpt_checkpointunlink,
		sizeof (struct res_lib_ckpt_checkpointunlink), MSG_WAITALL | MSG_NOSIGNAL);

exit_put:
	saHandleInstancePut (&ckptHandleDatabase, ckptHandle);
	
	return (error == SA_AIS_OK ? res_lib_ckpt_checkpointunlink.header.error : error);

exit_noclose:
	return (error);
}

SaAisErrorT
saCkptCheckpointRetentionDurationSet (
	SaCkptCheckpointHandleT checkpointHandle,
	SaTimeT retentionDuration)
{
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct req_lib_ckpt_checkpointretentiondurationset req_lib_ckpt_checkpointretentiondurationset;
	struct res_lib_ckpt_checkpointretentiondurationset res_lib_ckpt_checkpointretentiondurationset;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		goto error_exit_noput;
	}

	req_lib_ckpt_checkpointretentiondurationset.header.size = sizeof (struct req_lib_ckpt_checkpointretentiondurationset);
	req_lib_ckpt_checkpointretentiondurationset.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_CHECKPOINTRETENTIONDURATIONSET;

	req_lib_ckpt_checkpointretentiondurationset.retentionDuration = retentionDuration;
	memcpy (&req_lib_ckpt_checkpointretentiondurationset.checkpointName,
		&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));

	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	error = saSendRetry (ckptCheckpointInstance->response_fd, &req_lib_ckpt_checkpointretentiondurationset, sizeof (struct req_lib_ckpt_checkpointretentiondurationset), MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	error = saRecvRetry (ckptCheckpointInstance->response_fd,
		&res_lib_ckpt_checkpointretentiondurationset,
		sizeof (struct res_lib_ckpt_checkpointretentiondurationset),
		MSG_WAITALL | MSG_NOSIGNAL);

	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

error_exit:
	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);
error_exit_noput:
	return (error == SA_AIS_OK ? res_lib_ckpt_checkpointretentiondurationset.header.error : error);
}

SaAisErrorT
saCkptActiveReplicaSet (
	SaCkptCheckpointHandleT checkpointHandle)
{
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct req_lib_ckpt_activereplicaset req_lib_ckpt_activereplicaset;
	struct res_lib_ckpt_activereplicaset res_lib_ckpt_activereplicaset;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		 (void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		goto error_noput;
	}

	if ((ckptCheckpointInstance->checkpointOpenFlags & SA_CKPT_CHECKPOINT_WRITE) == 0) {
		error = SA_AIS_ERR_ACCESS;
		goto error_put;
	}

	req_lib_ckpt_activereplicaset.header.size = sizeof (struct req_lib_ckpt_activereplicaset);
	req_lib_ckpt_activereplicaset.header.id = MESSAGE_REQ_CKPT_ACTIVEREPLICASET;
	memcpy (&req_lib_ckpt_activereplicaset.checkpointName,
		&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));

	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	error = saSendRetry (ckptCheckpointInstance->response_fd, &req_lib_ckpt_activereplicaset,
		sizeof (struct req_lib_ckpt_activereplicaset), MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_put;
	}

	error = saRecvRetry (ckptCheckpointInstance->response_fd,
		&res_lib_ckpt_activereplicaset,
		sizeof (struct res_lib_ckpt_activereplicaset),
		MSG_WAITALL | MSG_NOSIGNAL);

	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

error_put:
	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);
error_noput:
	return (error == SA_AIS_OK ? res_lib_ckpt_activereplicaset.header.error : error);
}

SaAisErrorT
saCkptCheckpointStatusGet (
	SaCkptCheckpointHandleT checkpointHandle,
	SaCkptCheckpointDescriptorT *checkpointStatus)
{
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct req_lib_ckpt_checkpointstatusget req_lib_ckpt_checkpointstatusget;
	struct res_lib_ckpt_checkpointstatusget res_lib_ckpt_checkpointstatusget;

	if (checkpointStatus == NULL) {
		return (SA_AIS_ERR_INVALID_PARAM);
	}
	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		return (error);
	}

	req_lib_ckpt_checkpointstatusget.header.size = sizeof (struct req_lib_ckpt_checkpointstatusget);
	req_lib_ckpt_checkpointstatusget.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_CHECKPOINTSTATUSGET;

	memcpy (&req_lib_ckpt_checkpointstatusget.checkpointName,
		&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));

	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	error = saSendRetry (ckptCheckpointInstance->response_fd, &req_lib_ckpt_checkpointstatusget,
		sizeof (struct req_lib_ckpt_checkpointstatusget), MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	error = saRecvRetry (ckptCheckpointInstance->response_fd,
		&res_lib_ckpt_checkpointstatusget,
		sizeof (struct res_lib_ckpt_checkpointstatusget),
		MSG_WAITALL | MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

	memcpy (checkpointStatus,
		&res_lib_ckpt_checkpointstatusget.checkpointDescriptor,
		sizeof (SaCkptCheckpointDescriptorT));

printf ("error is %d\n", res_lib_ckpt_checkpointstatusget.header.error);
error_exit:
	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);
	return (error == SA_AIS_OK ? res_lib_ckpt_checkpointstatusget.header.error : error);
}

SaAisErrorT
saCkptSectionCreate (
	SaCkptCheckpointHandleT checkpointHandle,
	SaCkptSectionCreationAttributesT *sectionCreationAttributes,
	const void *initialData,
	SaUint32T initialDataSize)
{
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct req_lib_ckpt_sectioncreate req_lib_ckpt_sectioncreate;
	struct res_lib_ckpt_sectioncreate res_lib_ckpt_sectioncreate;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		return (error);
	}

	if ((ckptCheckpointInstance->checkpointOpenFlags & SA_CKPT_CHECKPOINT_WRITE) == 0) {
		error = SA_AIS_ERR_ACCESS;
		goto error_exit;
	}

	req_lib_ckpt_sectioncreate.header.size =
		sizeof (struct req_lib_ckpt_sectioncreate) +
		sectionCreationAttributes->sectionId->idLen +
		initialDataSize; 

	req_lib_ckpt_sectioncreate.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_SECTIONCREATE;
	req_lib_ckpt_sectioncreate.idLen = sectionCreationAttributes->sectionId->idLen;
	req_lib_ckpt_sectioncreate.expirationTime = sectionCreationAttributes->expirationTime;
	req_lib_ckpt_sectioncreate.initialDataSize = initialDataSize;

	memcpy (&req_lib_ckpt_sectioncreate.checkpointName,
		&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));

	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	error = saSendRetry (ckptCheckpointInstance->response_fd, &req_lib_ckpt_sectioncreate,
		sizeof (struct req_lib_ckpt_sectioncreate), MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	/*
	 * Write section identifier to server
	 */
	error = saSendRetry (ckptCheckpointInstance->response_fd, sectionCreationAttributes->sectionId->id,
		sectionCreationAttributes->sectionId->idLen, MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	error = saSendRetry (ckptCheckpointInstance->response_fd, initialData,
		initialDataSize, MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	error = saRecvRetry (ckptCheckpointInstance->response_fd,
		&res_lib_ckpt_sectioncreate,
		sizeof (struct res_lib_ckpt_sectioncreate),
		MSG_WAITALL | MSG_NOSIGNAL);

	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

error_exit:
	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);

	return (error == SA_AIS_OK ? res_lib_ckpt_sectioncreate.header.error : error);
}


SaAisErrorT
saCkptSectionDelete (
	SaCkptCheckpointHandleT checkpointHandle,
	const SaCkptSectionIdT *sectionId)
{
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct req_lib_ckpt_sectiondelete req_lib_ckpt_sectiondelete;
	struct res_lib_ckpt_sectiondelete res_lib_ckpt_sectiondelete;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		return (error);
	}

	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	req_lib_ckpt_sectiondelete.header.size = sizeof (struct req_lib_ckpt_sectiondelete) + sectionId->idLen; 
	req_lib_ckpt_sectiondelete.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_SECTIONDELETE;
	req_lib_ckpt_sectiondelete.idLen = sectionId->idLen;

	memcpy (&req_lib_ckpt_sectiondelete.checkpointName,
		&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));

	error = saSendRetry (ckptCheckpointInstance->response_fd, &req_lib_ckpt_sectiondelete,
		sizeof (struct req_lib_ckpt_sectiondelete), MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	/*
	 * Write section identifier to server
	 */
	error = saSendRetry (ckptCheckpointInstance->response_fd, sectionId->id,
		sectionId->idLen, MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}
	error = saRecvRetry (ckptCheckpointInstance->response_fd,
		&res_lib_ckpt_sectiondelete,
		sizeof (struct res_lib_ckpt_sectiondelete),
		MSG_WAITALL | MSG_NOSIGNAL);

	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

error_exit:
	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);
	return (error == SA_AIS_OK ? res_lib_ckpt_sectiondelete.header.error : error);
}

SaAisErrorT
saCkptSectionExpirationTimeSet (
	SaCkptCheckpointHandleT checkpointHandle,
	const SaCkptSectionIdT *sectionId,
	SaTimeT expirationTime)
{
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct req_lib_ckpt_sectionexpirationtimeset req_lib_ckpt_sectionexpirationtimeset;
	struct res_lib_ckpt_sectionexpirationtimeset res_lib_ckpt_sectionexpirationtimeset;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		goto error_exit_noput;
	}

	req_lib_ckpt_sectionexpirationtimeset.header.size = sizeof (struct req_lib_ckpt_sectionexpirationtimeset) + sectionId->idLen; 
	req_lib_ckpt_sectionexpirationtimeset.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_SECTIONEXPIRATIONTIMESET;
	req_lib_ckpt_sectionexpirationtimeset.idLen = sectionId->idLen;
	req_lib_ckpt_sectionexpirationtimeset.expirationTime = expirationTime;

	memcpy (&req_lib_ckpt_sectionexpirationtimeset.checkpointName,
		&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));

	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	error = saSendRetry (ckptCheckpointInstance->response_fd, &req_lib_ckpt_sectionexpirationtimeset,
		sizeof (struct req_lib_ckpt_sectionexpirationtimeset), MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	/*
	 * Write section identifier to server
	 */
	if (sectionId->idLen) {
		error = saSendRetry (ckptCheckpointInstance->response_fd, sectionId->id,
			sectionId->idLen, MSG_NOSIGNAL);
		if (error != SA_AIS_OK) {
			goto error_exit;
		}
	}

	error = saRecvRetry (ckptCheckpointInstance->response_fd,
		&res_lib_ckpt_sectionexpirationtimeset,
		sizeof (struct res_lib_ckpt_sectionexpirationtimeset),
		MSG_WAITALL | MSG_NOSIGNAL);

	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

error_exit:
	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);
error_exit_noput:
	return (error == SA_AIS_OK ? res_lib_ckpt_sectionexpirationtimeset.header.error : error);
}

SaAisErrorT
saCkptSectionIterationInitialize (
	SaCkptCheckpointHandleT checkpointHandle,
	SaCkptSectionsChosenT sectionsChosen,
	SaTimeT expirationTime,
	SaCkptSectionIterationHandleT *sectionIterationHandle)
{
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct ckptSectionIterationInstance *ckptSectionIterationInstance;
	struct req_lib_ckpt_sectioniteratorinitialize req_lib_ckpt_sectioniteratorinitialize;
	struct res_lib_ckpt_sectioniteratorinitialize res_lib_ckpt_sectioniteratorinitialize;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		goto error_no_destroy;
	}

	error = saHandleCreate (&ckptSectionIterationHandleDatabase,
		sizeof (struct ckptSectionIterationInstance), sectionIterationHandle);
	if (error != SA_AIS_OK) {
		goto error_put_checkpoint_db;
	}

	error = saHandleInstanceGet (&ckptSectionIterationHandleDatabase,
		*sectionIterationHandle, (void *)&ckptSectionIterationInstance);
	if (error != SA_AIS_OK) {
		goto error_destroy;
	}

	ckptSectionIterationInstance->response_fd = ckptCheckpointInstance->response_fd;

	ckptSectionIterationInstance->checkpointHandle = checkpointHandle;

	pthread_mutex_init (&ckptSectionIterationInstance->response_mutex, NULL);

	/*
	 * Setup section id list for iterator next
	 */
	list_init (&ckptSectionIterationInstance->sectionIdListHead);

	req_lib_ckpt_sectioniteratorinitialize.header.size = sizeof (struct req_lib_ckpt_sectioniteratorinitialize); 
	req_lib_ckpt_sectioniteratorinitialize.header.id = MESSAGE_REQ_CKPT_SECTIONITERATOR_SECTIONITERATORINITIALIZE;
	req_lib_ckpt_sectioniteratorinitialize.sectionsChosen = sectionsChosen;
	req_lib_ckpt_sectioniteratorinitialize.expirationTime = expirationTime;
	memcpy (&req_lib_ckpt_sectioniteratorinitialize.checkpointName,
		&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));

	pthread_mutex_lock (&ckptSectionIterationInstance->response_mutex);

	error = saSendRetry (ckptSectionIterationInstance->response_fd,
		&req_lib_ckpt_sectioniteratorinitialize,
		sizeof (struct req_lib_ckpt_sectioniteratorinitialize),
		MSG_NOSIGNAL);

	if (error != SA_AIS_OK) {
		goto error_put_destroy;
	}

	error = saRecvRetry (ckptSectionIterationInstance->response_fd,
		&res_lib_ckpt_sectioniteratorinitialize,
		sizeof (struct res_lib_ckpt_sectioniteratorinitialize),
		MSG_WAITALL | MSG_NOSIGNAL);

	pthread_mutex_unlock (&ckptSectionIterationInstance->response_mutex);

	saHandleInstancePut (&ckptSectionIterationHandleDatabase, *sectionIterationHandle);

	return (error == SA_AIS_OK ? res_lib_ckpt_sectioniteratorinitialize.header.error : error);

error_put_destroy:
	saHandleInstancePut (&ckptSectionIterationHandleDatabase, *sectionIterationHandle);
error_destroy:
	saHandleDestroy (&ckptSectionIterationHandleDatabase, *sectionIterationHandle);
error_put_checkpoint_db:
	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);
error_no_destroy:
	return (error);
}

struct iteratorSectionIdListEntry {
	struct list_head list;
	char data[0];
};

SaAisErrorT
saCkptSectionIterationNext (
	SaCkptSectionIterationHandleT sectionIterationHandle,
	SaCkptSectionDescriptorT *sectionDescriptor)
{
	SaAisErrorT error;
	struct ckptSectionIterationInstance *ckptSectionIterationInstance;
	struct req_lib_ckpt_sectioniteratornext req_lib_ckpt_sectioniteratornext;
	struct res_lib_ckpt_sectioniteratornext res_lib_ckpt_sectioniteratornext;
	struct iteratorSectionIdListEntry *iteratorSectionIdListEntry;

	error = saHandleInstanceGet (&ckptSectionIterationHandleDatabase,
		sectionIterationHandle, (void *)&ckptSectionIterationInstance);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}
	/*
	 * Allocate section id storage area
	 */
	/*
	 *  TODO max section id size is 500
	 */
	iteratorSectionIdListEntry = malloc (sizeof (struct list_head) + 500);
	if (iteratorSectionIdListEntry == 0) {
		error = SA_AIS_ERR_NO_MEMORY;
		goto error_put_nounlock;
	}

	req_lib_ckpt_sectioniteratornext.header.size = sizeof (struct req_lib_ckpt_sectioniteratornext); 
	req_lib_ckpt_sectioniteratornext.header.id = MESSAGE_REQ_CKPT_SECTIONITERATOR_SECTIONITERATORNEXT;

	pthread_mutex_lock (&ckptSectionIterationInstance->response_mutex);

	error = saSendRetry (ckptSectionIterationInstance->response_fd,
		&req_lib_ckpt_sectioniteratornext,
		sizeof (struct req_lib_ckpt_sectioniteratornext), MSG_NOSIGNAL);

	if (error != SA_AIS_OK) {
		goto error_put;
	}

	error = saRecvRetry (ckptSectionIterationInstance->response_fd, &res_lib_ckpt_sectioniteratornext,
		sizeof (struct res_lib_ckpt_sectioniteratornext), MSG_WAITALL | MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_put;
	}

	memcpy (sectionDescriptor,
		&res_lib_ckpt_sectioniteratornext.sectionDescriptor,
		sizeof (SaCkptSectionDescriptorT));

	sectionDescriptor->sectionId.id = &iteratorSectionIdListEntry->data[0];
	
	if ((res_lib_ckpt_sectioniteratornext.header.size - sizeof (struct res_lib_ckpt_sectioniteratornext)) > 0) {
		error = saRecvRetry (ckptSectionIterationInstance->response_fd,
			sectionDescriptor->sectionId.id,
			res_lib_ckpt_sectioniteratornext.header.size -
				sizeof (struct res_lib_ckpt_sectioniteratornext),
			MSG_WAITALL | MSG_NOSIGNAL);
	}

	/*
	 * Add to persistent memory list for this sectioniterator
	 */
	if (error == SA_AIS_OK && res_lib_ckpt_sectioniteratornext.header.error == SA_AIS_OK) {
		list_init (&iteratorSectionIdListEntry->list);
		list_add (&iteratorSectionIdListEntry->list, &ckptSectionIterationInstance->sectionIdListHead);
	}

error_put:
	pthread_mutex_unlock (&ckptSectionIterationInstance->response_mutex);
error_put_nounlock:
	saHandleInstancePut (&ckptSectionIterationHandleDatabase, sectionIterationHandle);
error_exit:
	return (error == SA_AIS_OK ? res_lib_ckpt_sectioniteratornext.header.error : error);
}
	
SaAisErrorT
saCkptSectionIterationFinalize (
	SaCkptSectionIterationHandleT sectionIterationHandle)
{
	SaAisErrorT error;
	struct ckptSectionIterationInstance *ckptSectionIterationInstance;
	struct iteratorSectionIdListEntry *iteratorSectionIdListEntry;
	struct list_head *sectionIdIterationList;
	struct list_head *sectionIdIterationListNext;

	error = saHandleInstanceGet (&ckptSectionIterationHandleDatabase,
		sectionIterationHandle, (void *)&ckptSectionIterationInstance);
	if (error != SA_AIS_OK) {
		goto error_noput;
	}

	/*
	 * iterate list of section ids for this iterator to free the allocated memory
	 * be careful to cache next pointer because free removes memory from use
	 */
	for (sectionIdIterationList = ckptSectionIterationInstance->sectionIdListHead.next,
		sectionIdIterationListNext = sectionIdIterationList->next;
		sectionIdIterationList != &ckptSectionIterationInstance->sectionIdListHead;
		sectionIdIterationList = sectionIdIterationListNext,
		sectionIdIterationListNext = sectionIdIterationList->next) {

		iteratorSectionIdListEntry = list_entry (sectionIdIterationList,
			struct iteratorSectionIdListEntry, list);

		free (iteratorSectionIdListEntry);
	}

	saHandleInstancePut (&checkpointHandleDatabase,
		ckptSectionIterationInstance->checkpointHandle);

	saHandleInstancePut (&ckptSectionIterationHandleDatabase, sectionIterationHandle);

    saHandleDestroy (&ckptSectionIterationHandleDatabase, sectionIterationHandle);

error_noput:
	return (error);
}

SaAisErrorT
saCkptCheckpointWrite (
	SaCkptCheckpointHandleT checkpointHandle,
	const SaCkptIOVectorElementT *ioVector,
	SaUint32T numberOfElements,
	SaUint32T *erroneousVectorIndex)
{
	SaAisErrorT error = SA_AIS_OK;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct req_lib_ckpt_sectionwrite req_lib_ckpt_sectionwrite;
	struct res_lib_ckpt_sectionwrite res_lib_ckpt_sectionwrite;
	int i;
	struct iovec iov[3];
	int iov_len = 0;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		return (error);
	}

	if ((ckptCheckpointInstance->checkpointOpenFlags & SA_CKPT_CHECKPOINT_WRITE) == 0) {
		error = SA_AIS_ERR_ACCESS;
		goto error_put;
	}
	req_lib_ckpt_sectionwrite.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_SECTIONWRITE;

	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	for (i = 0; i < numberOfElements; i++) {

		req_lib_ckpt_sectionwrite.header.size = sizeof (struct req_lib_ckpt_sectionwrite) + ioVector[i].sectionId.idLen + ioVector[i].dataSize; 

		req_lib_ckpt_sectionwrite.dataOffset = ioVector[i].dataOffset;
		req_lib_ckpt_sectionwrite.dataSize = ioVector[i].dataSize;
		req_lib_ckpt_sectionwrite.idLen = ioVector[i].sectionId.idLen;

		memcpy (&req_lib_ckpt_sectionwrite.checkpointName,
			&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));

		iov_len = 0;
/* TODO check for zero length stuff */
		iov[0].iov_base = (char *)&req_lib_ckpt_sectionwrite;
		iov[0].iov_len = sizeof (struct req_lib_ckpt_sectionwrite);
		iov[1].iov_base = ioVector[i].sectionId.id;
		iov[1].iov_len = ioVector[i].sectionId.idLen;
		iov[2].iov_base = ioVector[i].dataBuffer;
		iov[2].iov_len = ioVector[i].dataSize;

		error = saSendMsgRetry (ckptCheckpointInstance->response_fd,
			iov,
			3);
		if (error != SA_AIS_OK) {
			goto error_exit;
		}

		/*
		 * Receive response
		 */
		error = saRecvRetry (ckptCheckpointInstance->response_fd, &res_lib_ckpt_sectionwrite,
			sizeof (struct res_lib_ckpt_sectionwrite), MSG_WAITALL | MSG_NOSIGNAL);
		if (error != SA_AIS_OK) {
			goto error_exit;
		}

		if (res_lib_ckpt_sectionwrite.header.error == SA_AIS_ERR_TRY_AGAIN) {
			error = SA_AIS_ERR_TRY_AGAIN;
			goto error_exit;
		}
		/*
		 * If error, report back erroneous index
		 */
		if (res_lib_ckpt_sectionwrite.header.error != SA_AIS_OK) {
			if (erroneousVectorIndex) {
				*erroneousVectorIndex = i;
			}
			goto error_exit;
		}
	}

error_exit:
	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

error_put:
	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);

	return (error == SA_AIS_OK ? res_lib_ckpt_sectionwrite.header.error : error);
}

SaAisErrorT
saCkptSectionOverwrite (
	SaCkptCheckpointHandleT checkpointHandle,
	const SaCkptSectionIdT *sectionId,
	const void *dataBuffer,
	SaSizeT dataSize)
{
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct req_lib_ckpt_sectionoverwrite req_lib_ckpt_sectionoverwrite;
	struct res_lib_ckpt_sectionoverwrite res_lib_ckpt_sectionoverwrite;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		return (error);
	}

	if ((ckptCheckpointInstance->checkpointOpenFlags & SA_CKPT_CHECKPOINT_WRITE) == 0) {
		return (SA_AIS_ERR_ACCESS);
	}

	req_lib_ckpt_sectionoverwrite.header.size = sizeof (struct req_lib_ckpt_sectionoverwrite) + sectionId->idLen + dataSize; 
	req_lib_ckpt_sectionoverwrite.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_SECTIONOVERWRITE;
	req_lib_ckpt_sectionoverwrite.idLen = sectionId->idLen;
	req_lib_ckpt_sectionoverwrite.dataSize = dataSize;
	memcpy (&req_lib_ckpt_sectionoverwrite.checkpointName,
		&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));
	
	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	error = saSendRetry (ckptCheckpointInstance->response_fd, &req_lib_ckpt_sectionoverwrite,
		sizeof (struct req_lib_ckpt_sectionoverwrite), MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	if (sectionId->idLen) {
		error = saSendRetry (ckptCheckpointInstance->response_fd, sectionId->id,
			sectionId->idLen, MSG_NOSIGNAL);
		if (error != SA_AIS_OK) {
			goto error_exit;
		}
	}
	error = saSendRetry (ckptCheckpointInstance->response_fd, dataBuffer, dataSize, MSG_NOSIGNAL);
	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	error = saRecvRetry (ckptCheckpointInstance->response_fd,
		&res_lib_ckpt_sectionoverwrite,
		sizeof (struct res_lib_ckpt_sectionoverwrite),
		MSG_WAITALL | MSG_NOSIGNAL);

error_exit:
	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);

	return (error == SA_AIS_OK ? res_lib_ckpt_sectionoverwrite.header.error : error);
}

SaAisErrorT
saCkptCheckpointRead (
	SaCkptCheckpointHandleT checkpointHandle,
	SaCkptIOVectorElementT *ioVector,
	SaUint32T numberOfElements,
	SaUint32T *erroneousVectorIndex)
{
	SaAisErrorT error = SA_AIS_OK;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct req_lib_ckpt_sectionread req_lib_ckpt_sectionread;
	struct res_lib_ckpt_sectionread res_lib_ckpt_sectionread;
	int dataLength;
	int i;
	struct iovec iov[3];

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		return (error);
	}

	if ((ckptCheckpointInstance->checkpointOpenFlags & SA_CKPT_CHECKPOINT_READ) == 0) {
		return (SA_AIS_ERR_ACCESS);
	}

	req_lib_ckpt_sectionread.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_SECTIONREAD;

	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	for (i = 0; i < numberOfElements; i++) {
		req_lib_ckpt_sectionread.header.size = sizeof (struct req_lib_ckpt_sectionread) +
			ioVector[i].sectionId.idLen;

		req_lib_ckpt_sectionread.idLen = ioVector[i].sectionId.idLen;
		req_lib_ckpt_sectionread.dataOffset = ioVector[i].dataOffset;
		req_lib_ckpt_sectionread.dataSize = ioVector[i].dataSize;

		memcpy (&req_lib_ckpt_sectionread.checkpointName,
			&ckptCheckpointInstance->checkpointName, sizeof (SaNameT));

		iov[0].iov_base = (char *)&req_lib_ckpt_sectionread;
		iov[0].iov_len = sizeof (struct req_lib_ckpt_sectionread);
		iov[1].iov_base = ioVector[i].sectionId.id;
		iov[1].iov_len = ioVector[i].sectionId.idLen;

		error = saSendMsgRetry (ckptCheckpointInstance->response_fd,
			iov,
			2);

		/*
		 * Receive response header
		 */
		error = saRecvRetry (ckptCheckpointInstance->response_fd, &res_lib_ckpt_sectionread,
			sizeof (struct res_lib_ckpt_sectionread), MSG_WAITALL | MSG_NOSIGNAL);
		if (error != SA_AIS_OK) {
				goto error_exit;
		}
		
		dataLength = res_lib_ckpt_sectionread.header.size - sizeof (struct res_lib_ckpt_sectionread);

		/*
		 * Receive checkpoint section data
		 */
		if (dataLength > 0) {
			error = saRecvRetry (ckptCheckpointInstance->response_fd, ioVector[i].dataBuffer,
				dataLength, MSG_WAITALL | MSG_NOSIGNAL);
			if (error != SA_AIS_OK) {
					goto error_exit;
			}
		}
		if (res_lib_ckpt_sectionread.header.error != SA_AIS_OK) {
			if (erroneousVectorIndex) {
				*erroneousVectorIndex = i;
			}
			goto error_exit;
		}

		/*
		 * Report back bytes of data read
		 */
		ioVector[i].readSize = res_lib_ckpt_sectionread.dataRead;
	}

error_exit:
	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);

	return (error == SA_AIS_OK ? res_lib_ckpt_sectionread.header.error : error);
}

SaAisErrorT
saCkptCheckpointSynchronize (
	SaCkptCheckpointHandleT checkpointHandle,
	SaTimeT timeout)
{
	SaAisErrorT error;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	struct req_lib_ckpt_checkpointsynchronize req_lib_ckpt_checkpointsynchronize;
	struct res_lib_ckpt_checkpointsynchronize res_lib_ckpt_checkpointsynchronize;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {
		return (error);
	}

	req_lib_ckpt_checkpointsynchronize.header.size = sizeof (struct req_lib_ckpt_checkpointsynchronize); 
	req_lib_ckpt_checkpointsynchronize.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_CHECKPOINTSYNCHRONIZE;

	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	error = saSendRetry (ckptCheckpointInstance->response_fd, &req_lib_ckpt_checkpointsynchronize,
		sizeof (struct req_lib_ckpt_checkpointsynchronize), MSG_NOSIGNAL);

	if (error != SA_AIS_OK) {
		goto error_exit;
	}

	error = saRecvRetry (ckptCheckpointInstance->response_fd,
		&res_lib_ckpt_checkpointsynchronize,
		sizeof (struct res_lib_ckpt_checkpointsynchronize),
		MSG_WAITALL | MSG_NOSIGNAL);

error_exit:
	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

	saHandleInstancePut (&checkpointHandleDatabase, checkpointHandle);

	return (error == SA_AIS_OK ? res_lib_ckpt_checkpointsynchronize.header.error : error);
}

SaAisErrorT
saCkptCheckpointSynchronizeAsync (
	SaCkptHandleT ckptHandle,
	SaCkptCheckpointHandleT checkpointHandle,
	SaInvocationT invocation)
{

	return (SA_AIS_OK);

/* TODO not implemented in executive

	struct ckptInstance *ckptInstance;
	struct ckptCheckpointInstance *ckptCheckpointInstance;
	SaAisErrorT error;
	struct req_lib_ckpt_checkpointsynchronizeasync req_lib_ckpt_checkpointsynchronizeasync;

	error = saHandleInstanceGet (&checkpointHandleDatabase, checkpointHandle,
		(void *)&ckptCheckpointInstance);
	if (error != SA_AIS_OK) {

		return (error);
	}

	req_lib_ckpt_checkpointsynchronizeasync.header.size = sizeof (struct req_lib_ckpt_checkpointsynchronizeasync);
	req_lib_ckpt_checkpointsynchronizeasync.header.id = MESSAGE_REQ_CKPT_CHECKPOINT_CHECKPOINTSYNCHRONIZEASYNC;
	req_lib_ckpt_checkpointsynchronizeasync.invocation = invocation;

	pthread_mutex_lock (&ckptCheckpointInstance->response_mutex);

	pthread_mutex_lock (&ckptInstance->response_mutex);

	error = saSendRetry (ckptInstance->response_fd, &req_lib_ckpt_checkpointsynchronizeasync,
		sizeof (struct req_lib_ckpt_checkpointsynchronizeasync), MSG_NOSIGNAL);

	pthread_mutex_unlock (&ckptInstance->response_mutex);

	pthread_mutex_unlock (&ckptCheckpointInstance->response_mutex);

	saHandleInstancePut (&checkpointHandleDatabase, *checkpointHandle);

	return (error);
*/
}
