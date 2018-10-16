/*
 * globalbp.h -
 *
 *	This file defines the (shared-memory) structures used by the PL debugger
 *  to keep track of global breakpoints.
 *
 * Copyright (c) 2004-2018 EnterpriseDB Corporation. All Rights Reserved.
 *
 * Licensed under the Artistic License v2.0, see
 *		https://opensource.org/licenses/artistic-license-2.0
 * for full details
 */
#ifndef GLOBALBP_H
#define GLOBALBP_H

#include "utils/hsearch.h"

typedef enum
{
	BP_LOCAL = 0,
	BP_GLOBAL
} eBreakpointScope;

/*
 * Stores information pertaining to a global breakpoint.
 */
typedef struct BreakpointData
{
	bool		isTmp;		/* tmp breakpoints are removed when hit */
	bool		busy;		/* is this session already in use by a target? */
	int			proxyPort;	/* port number of the proxy listener */
	int			proxyPid;	/* process id of the proxy process */
} BreakpointData;

/*
 * The key of the global breakpoints hash table. For now holds only have an Oid field.
 * but it may contain more fields in future.
 */
typedef struct BreakpointKey
{
	Oid			databaseId;
	Oid			functionId;
	int			lineNumber;
	int			targetPid;		/* -1 means any process */
} BreakpointKey;

typedef struct Breakpoint
{
	BreakpointKey		key;
	BreakpointData		data;
} Breakpoint;

extern Breakpoint * BreakpointLookup(eBreakpointScope scope, BreakpointKey *key);
extern bool 		BreakpointInsert(eBreakpointScope scope, BreakpointKey *key, BreakpointData *brkpnt);
extern bool 		BreakpointDelete(eBreakpointScope scope, BreakpointKey *key);
extern void 		BreakpointShowAll(eBreakpointScope scope);
extern bool			BreakpointInsertOrUpdate(eBreakpointScope scope, BreakpointKey *key, BreakpointData *data);
extern bool 		BreakpointOnId(eBreakpointScope scope, Oid funcOid);
extern void 		BreakpointCleanupProc(int pid);
extern void			BreakpointGetList(eBreakpointScope scope, HASH_SEQ_STATUS *scan);
extern void			BreakpointReleaseList(eBreakpointScope scope);
extern void 		BreakpointBusySession(int pid);
extern void 		BreakpointFreeSession(int pid);
#endif
