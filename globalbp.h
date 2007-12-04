/*
 * globalbp.h -
 *
 *	This file defines the (shared-memory) structures used by the PL debugger
 *  to keep track of global breakpoints.
 *
 * Copyright (c) 2004-2007 EnterpriseDB Corporation. All Rights Reserved.
 *
 * Licensed under the Artistic License, see 
 *		http://www.opensource.org/licenses/artistic-license.php
 * for full details
 */
#ifndef GLOBALBP_H
#define GLOBALBP_H

#include "nodes/parsenodes.h"
#include "utils/hsearch.h"
/* 
 * defs used in guc.c 
 */
#define	GBP_MIN_SIZE	12
#define GBP_MAX_SIZE	500
#define GBP_RESET_VAL	255

extern int gbp_tab_size;

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
	bool			isTmp;							/* tmp breakpoints are removed whenever they are hit		*/
	bool			busy;							/* is this session already in use by a target?				*/
	int				proxyPort;						/* port number of the proxy listener 						*/
	int				proxyPid;						/* pid of the proxy process 								*/
} BreakpointData;

/* 
 * The key of the global breakpoints hash table. For now holds only have an Oid field.
 * but it may contain more fields in future.
 */
typedef struct BreakpointKey
{
	Oid			databaseId;
#if INCLUDE_PACKAGE_SUPPORT
	Oid		   	packageId;		/* invalid OID means this is not a package-related breakpoint  */
#endif
	Oid			functionId;
	int			lineNumber;
	int			targetPid;		/* -1 means any process */
} BreakpointKey;

typedef struct Breakpoint
{
	BreakpointKey		key;
	BreakpointData		data;		
} Breakpoint;

extern Size 		BreakpointShmemSize(void);
extern void 		InitBreakpoints(void);
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
