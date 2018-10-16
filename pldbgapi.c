/*
 * pldbgapi.c
 *
 *	This module defines (and implements) an API for debugging PL
 *	functions and procedures (in particular, functions and procedures
 *	written in PL/pgSQL or edb-spl).
 *
 *	To debug a function or procedure, you need two backend processes
 *	plus a debugger client (the client could be a command-line client
 *	such as psql but is more likely a graphical client such as pgAdmin).
 *
 *	The first backend is called the target - it's the process that's
 *	running the code that you want to debug.
 *
 *	The second backend is a 'proxy' process that shuttles data between
 *	the debugger client and the target.  The functions implemented in
 *	this module are called 'proxy functions'.
 *
 *	The proxy process provides an easy and secure way for the debugger
 *	client to connect to the target - the client opens a normal
 *	libpq-style connection that (presumably) knows how to work it's
 *	way through a firewall and through the authentication maze (once
 *	the connection process completes, the debugger client is connected
 *	to the proxy).
 *
 *	The debugger client can call any of the functions in this API.
 *	Each function is executed by the proxy process.  The proxy
 *	shuttles debugging requests (like 'step into' or 'show call
 *	stack') to the debugger server (running inside of the target
 *	process) and sends the results back to the debugger client.
 *
 *	There are a few basic rules for using this API:
 *
 *	You must call one of the connection functions before you can do
 *	anything else (at this point, the only connection function is
 *	'pldbg_attach_to_port()', but we'll add more as soon as we
 *	implement global breakpoints). Each connection function returns
 *	a session ID that identifies that debugging session (a debugger
 *	client can maintain multiple simultaneous sessions by keeping
 *	track of each session identifier).  You pass that session ID
 *	to all of the other proxy functions.
 *
 *	Once you have opened a session, you must wait for the target
 *	to reach a breakpoint (it may already be stopped at a breakpoint)
 *	by calling pldbg_wait_for_breakpoint( sessionID ) - that function
 *	will hang until the target reaches a breakpoint (or the target
 *	session ends).
 *
 *	When the target pauses, you can interact with the debugger server
 *  (running inside of the target process) by calling any of the other
 *  proxy functions.  For example, to tell the target to "step into" a
 *  function/procedure call, you would call pldbg_step_into() (and that
 *  function would hang until the target pauses).  To tell the target
 *  to continue until the next breakpoint, you would call
 *  pldbg_continue() (and, again, that function would hang until the
 *  target pauses).
 *
 *	Each time the target pauses, it returns a tuple of type 'breakpoint'.
 *  That tuple contains the OID of the function that the target has paused
 *  in, and the line number at which the target has paused. The fact that the
 *	target returns a tuple of type breakpoint does not imply that the target
 *	has paused at a breakpoint - it may have paused because of a step-over or
 *	step-into operation.
 *
 *	When the target is paused at a breakpoint (or has paused after
 *	a step-over or step-into), you can interrogate the target by calling
 *	pldbg_get_stack(), pldbg_get_source(), pldbg_get_breakpoints(), or
 *	pldbg_get_variables().
 *
 *	The debugger server groks the PL call stack and maintains a
 *	'focus' frame.  By default, the debugger server focuses on the most
 *	deeply nested frame (because that's the code that's actually
 *	running).  You can shift the debugger's focus to a different frame
 *	by calling pldbg_select_frame().
 *
 *	The focus is important because many functions (such as
 *	pldbg_get_variables()) work against the stack frame that has the focus.
 *
 *	Any of the proxy functions may throw an error - in particular, a proxy
 *	function will throw an error if the target process ends.  You're most
 *	likely to encounter an error when you call pldbg_continue() and the
 *	target process runs to completion (without hitting another breakpoint)
 *
 * Copyright (c) 2004-2018 EnterpriseDB Corporation. All Rights Reserved.
 *
 * Licensed under the Artistic License v2.0, see
 *		https://opensource.org/licenses/artistic-license-2.0
 * for full details
 */

#include "postgres.h"

#include "funcapi.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "storage/ipc.h"					/* For on_shmem_exit()  		*/
#include "storage/proc.h"					/* For MyProc		   			*/
#include "libpq/libpq-be.h"					/* For Port						*/
#include "miscadmin.h"						/* For MyProcPort				*/
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "access/htup.h"					/* For heap_form_tuple()		*/
#include "access/hash.h"					/* For dynahash stuff			*/

#include <errno.h>
#include <unistd.h>							/* For close()					*/
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "globalbp.h"
#include "dbgcomm.h"

/* Include header for GETSTRUCT */
#if (PG_VERSION_NUM >= 90300)
#include "access/htup_details.h"
#endif

#if PG_VERSION_NUM >= 110000
	#ifndef TRUE
		#define TRUE true
	#endif
	#ifndef FALSE
		#define FALSE false
	#endif
#endif

/*
 * Let the PG module loader know that we are compiled against
 * the right version of the PG header files
 */

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/*******************************************************************************
 * Proxy functions
 *******************************************************************************/

PG_FUNCTION_INFO_V1( pldbg_attach_to_port );		/* Attach to debugger server at the given port	*/
PG_FUNCTION_INFO_V1( pldbg_wait_for_breakpoint );  	/* Wait for the target to reach a breakpoint	*/
PG_FUNCTION_INFO_V1( pldbg_step_into );				/* Steop into a function/procedure call			*/
PG_FUNCTION_INFO_V1( pldbg_step_over );				/* Step over a function/procedure call			*/
PG_FUNCTION_INFO_V1( pldbg_continue );				/* Continue execution until next breakpoint		*/
PG_FUNCTION_INFO_V1( pldbg_get_source );			/* Get the source code for a function/procedure	*/
PG_FUNCTION_INFO_V1( pldbg_get_breakpoints );		/* SHOW BREAKPOINTS equivalent (deprecated)		*/
PG_FUNCTION_INFO_V1( pldbg_get_variables );			/* Get a list of variable names/types/values	*/
PG_FUNCTION_INFO_V1( pldbg_get_stack );				/* Get the call stack from the target			*/
PG_FUNCTION_INFO_V1( pldbg_set_breakpoint );		/* CREATE BREAKPOINT equivalent (deprecated)	*/
PG_FUNCTION_INFO_V1( pldbg_drop_breakpoint );		/* DROP BREAKPOINT equivalent (deprecated)		*/
PG_FUNCTION_INFO_V1( pldbg_select_frame );			/* Change the focus to a different stack frame	*/
PG_FUNCTION_INFO_V1( pldbg_deposit_value );		 	/* Change the value of an in-scope variable		*/
PG_FUNCTION_INFO_V1( pldbg_abort_target );			/* Abort execution of the target - throws error */
PG_FUNCTION_INFO_V1( pldbg_get_proxy_info );		/* Get server version, proxy API version, ...   */

PG_FUNCTION_INFO_V1( pldbg_create_listener );		/* Create a listener for global breakpoints		*/
PG_FUNCTION_INFO_V1( pldbg_wait_for_target );		/* Wait for a global breakpoint to fire			*/
PG_FUNCTION_INFO_V1( pldbg_set_global_breakpoint );	/* Create a global breakpoint					*/

/*******************************************************************************
 * Structure debugSession
 *
 *	A debugger client may attach to many target sessions at the same time. We
 *	keep track of each connection in a debugSession structure. When the client
 *	makes a connection, we allocate a new debugSession structure and return
 *	a handle to that structure to the caller.  He gives us back the handle
 *	whenever he calls another proxy function. A handle is just a smallish
 *  integer value that we use to track each session - we use a hash to map
 *  handles into debugSession pointers.
 */

typedef struct
{
	int			serverSocket;	/* Socket connected to the debugger server */
	int			serverPort;		/* Port number where debugger server is listening */
	int			listener;		/* Socket where we wait for global breakpoints */
	char	   *breakpointString;
} debugSession;

/*******************************************************************************
 * Stucture sessionHashEntry
 *
 *	As mentioned above (see debugSession), a debugger proxy can manage many
 *	debug sessions at once.  To keep track of each session, we create a
 *	debugSession object and return a handle to that object to the caller.  The
 *	handle is an opaque value - it's just an integer value.  To convert a
 *	handle into an actual debugSession pointer, we create a hash that maps
 *	handles into debugSession pointers.
 *
 *  Each member of the hash is shaped like a sessionHashEntry object.
 */
typedef int32  sessionHandle;

typedef struct
{
	sessionHandle	m_handle;
	debugSession   *m_session;
} sessionHashEntry;

static debugSession * mostRecentSession;
static HTAB			* sessionHash;

/*******************************************************************************
 * The following symbols represent the magic strings that we send to the
 * debugger server running in the target process
 */

#define PLDBG_GET_VARIABLES		"i\n"
#define PLDBG_GET_BREAKPOINTS 	"l\n"
#define PLDBG_GET_STACK       	"$\n"
#define PLDBG_STEP_INTO			"s\n"
#define PLDBG_STEP_OVER			"o\n"
#define PLDBG_CONTINUE			"c\n"
#define PLDBG_ABORT				"x"
#define PLDBG_SELECT_FRAME		"^"			/* Followed by frame number 				*/
#define PLDBG_SET_BREAKPOINT		"b"			/* Followed by pkgoid:funcoid:linenumber 	*/
#define PLDBG_CLEAR_BREAKPOINT	"f"			/* Followed by pkgoid:funcoid:linenumber 	*/
#define PLDBG_GET_SOURCE			"#" 		/* Followed by pkgoid:funcoid				*/
#define PLDBG_DEPOSIT				"d"			/* Followed by var.line=value				*/

#define PLDBG_STRING_MAX_LEN   128

#define PROXY_API_VERSION		3			/* API version number						*/

/*******************************************************************************
 * We currently define three PostgreSQL data types (all tuples) - the following
 * symbols correspond to the names for those types.
 */

#define	TYPE_NAME_BREAKPOINT	"breakpoint"	/* May change to pldbg.breakpoint later	*/
#define TYPE_NAME_FRAME			"frame"			/* May change to pldbg.frame later		*/
#define TYPE_NAME_VAR			"var"			/* May change to pldbg.var later		*/

#define GET_STR( textp ) 		DatumGetCString( DirectFunctionCall1( textout, PointerGetDatum( textp )))
#define PG_GETARG_SESSION( n )  (sessionHandle)PG_GETARG_UINT32( n )

Datum pldbg_select_frame( PG_FUNCTION_ARGS );
Datum pldbg_attach_to_port( PG_FUNCTION_ARGS );
Datum pldbg_get_source( PG_FUNCTION_ARGS );
Datum pldbg_get_breakpoints( PG_FUNCTION_ARGS );
Datum pldbg_get_variables( PG_FUNCTION_ARGS );
Datum pldbg_get_stack( PG_FUNCTION_ARGS );
Datum pldbg_wait_for_breakpoint( PG_FUNCTION_ARGS );
Datum pldbg_set_breakpoint( PG_FUNCTION_ARGS );
Datum pldbg_drop_breakpoint( PG_FUNCTION_ARGS );
Datum pldbg_step_into( PG_FUNCTION_ARGS );
Datum pldbg_step_over( PG_FUNCTION_ARGS );
Datum pldbg_continue(  PG_FUNCTION_ARGS );
Datum pldbg_deposit_value( PG_FUNCTION_ARGS );
Datum pldbg_get_proxy_info( PG_FUNCTION_ARGS );
Datum pldbg_get_pkg_cons( PG_FUNCTION_ARGS );
Datum pldbg_abort_target( PG_FUNCTION_ARGS );

Datum pldbg_create_listener( PG_FUNCTION_ARGS );
Datum pldbg_wait_for_target( PG_FUNCTION_ARGS );
Datum pldbg_set_global_breakpoint( PG_FUNCTION_ARGS );

/************************************************************
 * Local function forward declarations
 ************************************************************/
static char 		   * tokenize( char * src, const char * delimiters, char ** ctx );
static void 		   * readn( int serverHandle, void * dst, size_t len );
static void 		   * writen( int serverHandle, void * dst, size_t len );
static void   		  	 sendBytes( debugSession * session, void * src, size_t len );
static void   		  	 sendUInt32( debugSession * session, uint32 val );
static void   		  	 sendString( debugSession * session, char * src );
static bool   		  	 getBool( debugSession * session );
static uint32 		  	 getUInt32( debugSession * session );
static char 		   * getNString( debugSession * session );
static void 		  	 initializeModule( void );
static void 		  	 cleanupAtExit( int code, Datum arg );
static void 			 initSessionHash();
static debugSession    * defaultSession( sessionHandle handle );
static sessionHandle     addSession( debugSession * session );
static debugSession    * findSession( sessionHandle handle );
static TupleDesc	  	 getResultTupleDesc( FunctionCallInfo fcinfo );


/*******************************************************************************
 * Exported functions
 *******************************************************************************/

/*******************************************************************************
 * pldbg_attach_to_port( portNumber INTEGER ) RETURNS INTEGER
 *
 *	This function attaches to a debugging target listening on the given port. A
 *  debugger client should invoke this function in response to a PLDBGBREAK
 *  NOTICE (the notice contains the port number that you should connect to).
 *
 *	This function returns a session handle that identifies this particular debug
 *  session. When you call any of the other pldbg functions, you must supply
 *	the session handle returned by pldbg_attach_to_port().
 *
 *	A given debugger client can maintain multiple simultaneous sessions
 *	by calling pldbg_attach_to_port() many times (with different port
 *	numbers) and keeping track of the returned session handles.
 */

Datum pldbg_attach_to_port( PG_FUNCTION_ARGS )
{
	int32		targetBackend = PG_GETARG_INT32( 0 );
	debugSession *session;

	initializeModule();

	session = MemoryContextAllocZero( TopMemoryContext, sizeof( *session ));
	session->listener   = -1;

	session->serverSocket = dbgcomm_connect_to_target(targetBackend);

	if (session->serverSocket < 0)
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("could not connect to debug target")));

	/*
	 * After the handshake, the target process will send us information about
	 * the local breakpoint that it hit. Read it. We will hand it to the client
	 * if it calls wait_for_breakpoint().
	 */
	session->breakpointString = MemoryContextStrdup(TopMemoryContext,
													getNString(session));

	/*
	 * For convenience, remember the most recent session - if you call
	 * another pldbg_xxx() function with sessionHandle = 0, we'll use
	 * the most recent session.
	 */
	mostRecentSession = session;

	PG_RETURN_INT32(addSession(session));
}

Datum pldbg_create_listener( PG_FUNCTION_ARGS )
{
	debugSession * session = MemoryContextAllocZero( TopMemoryContext, sizeof( *session ));

	initializeModule();

	session->listener = dbgcomm_listen_for_target(&session->serverPort);
	session->serverSocket = -1;

	mostRecentSession = session;

	PG_RETURN_INT32( addSession( session ));
}

/*******************************************************************************
 * pldbg_wait_for_target( ) RETURNS INTEGER
 *
 *	This function advertises the proxy process as an active debugger, waiting
 *	for global breakpoints.
 *
 *	This function returns a session handle that identifies this particular debug
 *	session. When you call any of the other pldbg functions, you must supply
 *	this session handle.
 *
 *	A given debugger client can maintain multiple simultaneous sessions
 *	by calling pldbg_attach_to_port() many times (with different port
 *	numbers) and keeping track of the returned session handles.
 */

Datum pldbg_wait_for_target( PG_FUNCTION_ARGS )
{
	debugSession *session = defaultSession(PG_GETARG_SESSION( 0 ));
	int			serverSocket;
	int			serverPID;

	/*
	 * Now mark all of our global breakpoints as 'available' (that is, not
	 * busy)
	 */
	BreakpointFreeSession( MyProc->pid );

	serverSocket = dbgcomm_accept_target(session->listener, &serverPID);
	if (serverSocket < 0)
		ereport(ERROR,
				(errmsg("could not accept a connection from debugging target")));

	session->serverSocket = serverSocket;

	/*
	 * After the handshake, the target process will send us information about
	 * the local breakpoint that it hit. Read it. We will hand it to the client
	 * if it calls wait_for_breakpoint().
	 */
	session->breakpointString = MemoryContextStrdup(TopMemoryContext,
													getNString(session));

	PG_RETURN_UINT32( serverPID );
}

/*******************************************************************************
 * pldbg_set_global_breakpoint(sessionID INT, function OID, lineNumber INT)
 *	RETURNS boolean
 *
 *	This function registers a breakpoint in the global breakpoint table.
 */

Datum pldbg_set_global_breakpoint( PG_FUNCTION_ARGS )
{
	debugSession * session    = defaultSession( PG_GETARG_SESSION( 0 ));
	Breakpoint	   breakpoint;

	if( !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be a superuser to create a breakpoint")));

	if( session->listener == -1 )
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("given session is not a listener")));

	breakpoint.key.databaseId = MyProc->databaseId;
	breakpoint.key.functionId = PG_GETARG_OID( 1 );

	if( PG_ARGISNULL( 2 ))
		breakpoint.key.lineNumber = -1;
	else
		breakpoint.key.lineNumber = PG_GETARG_INT32( 2 );

	if( PG_ARGISNULL( 3 ))
		breakpoint.key.targetPid = -1;
	else
		breakpoint.key.targetPid  = PG_GETARG_INT32( 3 );

	breakpoint.data.isTmp     = TRUE;
	breakpoint.data.proxyPort = session->serverPort;
	breakpoint.data.proxyPid  = MyProc->pid;

	if( !BreakpointInsert( BP_GLOBAL, &breakpoint.key, &breakpoint.data ))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("another debugger is already waiting for that breakpoint")));

	PG_RETURN_BOOL( true );
}

/*******************************************************************************
 * pldbg_wait_for_breakpoint( sessionID INTEGER ) RETURNS breakpoint
 *
 *	This function waits for the debug target to reach a breakpoint.  You should
 *	call this function immediately after pldbg_attach_to_port() returns a
 *	session ID.  pldbg_wait_for_breakpoint() is nearly identical to
 *	pldbg_step_into(), pldbg_step_over(), and pldbg_continue(), (they all wait
 *	for the target) but	this function does not send a command to the target
 *	first.
 *
 *	This function returns a tuple of type 'breakpoint' - such a tuple contains
 *	the function OID and line number where the target is currently stopped.
 */

static Datum buildBreakpointDatum( char * breakpointString )
{
	char		 * values[3];
	char         * ctx = NULL;
	HeapTuple	   result;
	TupleDesc	   tupleDesc = RelationNameGetTupleDesc( TYPE_NAME_BREAKPOINT );

	values[0] = tokenize( breakpointString, ":", &ctx );  	/* function OID		*/
	values[1] = tokenize( NULL, ":", &ctx );  				/* linenumber		*/
	values[2] = tokenize( NULL, ":", &ctx );				/* targetName		*/

	result = BuildTupleFromCStrings( TupleDescGetAttInMetadata( tupleDesc ), values );

	return( HeapTupleGetDatum( result ));
}

Datum pldbg_wait_for_breakpoint( PG_FUNCTION_ARGS )
{
	debugSession * session           = defaultSession( PG_GETARG_SESSION( 0 ));
	char         * breakpointString;

	if (!session->breakpointString)
		PG_RETURN_NULL();

	breakpointString = pstrdup(session->breakpointString);
	pfree(session->breakpointString);
	session->breakpointString = NULL;

	PG_RETURN_DATUM( buildBreakpointDatum( breakpointString ));
}

/*******************************************************************************
 * pldbg_step_into( sessionID INTEGER ) RETURNS breakpoint
 *
 *	This function sends a "step/into" command to the debugger target and then
 *  waits for target to reach the next executable statement.
 *
 *	This function returns a tuple of type 'breakpoint' that contains the
 *	function OID and line number where the target is currently stopped.
 */

Datum pldbg_step_into( PG_FUNCTION_ARGS )
{
	debugSession * session = defaultSession( PG_GETARG_SESSION( 0 ));

	sendString( session, PLDBG_STEP_INTO );

	PG_RETURN_DATUM( buildBreakpointDatum( getNString( session )));
}

/*******************************************************************************
 * pldbg_step_over( sessionID INTEGER ) RETURNS breakpoint
 *
 *	This function sends a "step/over" command to the debugger target and then
 *  waits for target to reach the next executable statement within the current
 *	function.  If the target encounters a breakpoint (presumably in a child
 *	invocation) before reaching the next executable line, it will stop at the
 *	breakpoint.
 *
 *	This function returns a tuple of type 'breakpoint' that contains the
 *	function OID and line number where the target is currently stopped.
 */

Datum pldbg_step_over( PG_FUNCTION_ARGS )
{
	debugSession * session = defaultSession( PG_GETARG_SESSION( 0 ));

	sendString( session, PLDBG_STEP_OVER );

	PG_RETURN_DATUM( buildBreakpointDatum( getNString( session )));
}

/*******************************************************************************
 * pldbg_continue( sessionID INTEGER ) RETURNS breakpoint
 *
 *	This function sends a "continue" command to the debugger target and then
 *  waits for target to reach a breakpoint.
 *
 *	This function returns a tuple of type 'breakpoint' that contains the
 *	function OID and line number where the target is currently stopped.
 */

Datum pldbg_continue( PG_FUNCTION_ARGS )
{
	debugSession * session = defaultSession( PG_GETARG_SESSION( 0 ));

	sendString( session, PLDBG_CONTINUE );

	PG_RETURN_DATUM( buildBreakpointDatum( getNString( session )));
}

/*******************************************************************************
 * pldbg_abort_target( sessionID INTEGER ) RETURNS breakpoint
 *
 *	This function sends an "abort" command to the debugger target and then
 *  waits for a reply
 */

Datum pldbg_abort_target( PG_FUNCTION_ARGS )
{
	debugSession * session = defaultSession( PG_GETARG_SESSION( 0 ));

	sendString( session, PLDBG_ABORT );

	PG_RETURN_BOOL( getBool( session ));

}

/*******************************************************************************
 * pldbg_select_frame( sessionID INTEGER, frameNumber INTEGER )
 *   RETURNS breakpoint
 *
 *	This function changes the debugger focus to the indicated frame (in the call
 *	stack). Whenever the target stops (at a breakpoint or as the result of a
 *	step/into or step/over), the debugger changes focus to most deeply nested
 *  function in the call stack (because that's the function that's executing).
 *
 *	You can change the debugger focus to other stack frames - once you do that,
 *	you can examine the source code for that frame, the variable values in that
 *	frame, and the breakpoints in that target.
 *
 *	The debugger focus remains on the selected frame until you change it or
 *	the target stops at another breakpoint.
 *
 *	This function returns a tuple of type 'breakpoint' that contains the
 *	function OID, and line number where the target is currently stopped in
 *	the selected frame.
 */

Datum pldbg_select_frame( PG_FUNCTION_ARGS )
{
	if( PG_ARGISNULL( 0 ))
		PG_RETURN_NULL();
	else
	{
		debugSession * session 	   = defaultSession( PG_GETARG_SESSION( 0 ));
		int32		   frameNumber = PG_GETARG_INT32( 1 );
		char		   frameString[PLDBG_STRING_MAX_LEN];
		char         * resultString;
		Datum		   result;

		snprintf(
			frameString, PLDBG_STRING_MAX_LEN, "%s %d", PLDBG_SELECT_FRAME,
			frameNumber
		);

		sendString( session, frameString );

		resultString = getNString( session );

		result = buildBreakpointDatum( resultString );

		PG_RETURN_DATUM( result );
	}
}

/*******************************************************************************
 * pldbg_get_source( sessionID INTEGER, functionOID OID )
 *   RETURNS CSTRING
 *
 *	This function returns the source code for the given function. A debugger
 *	client should always retrieve source code using this function instead of
 *  reading pg_proc.  If you read pg_proc instead, the source code that you
 *	read may not match the source that the target is actually executing
 *	(because the source code may have been modified in a different transaction).
 *
 *  pldbg_get_source() always retrieves the source code from the target and
 *  ensures that the source code that you get is the source code that the
 *  target is executing.
 *
 */

Datum pldbg_get_source( PG_FUNCTION_ARGS )
{
	debugSession * session = defaultSession( PG_GETARG_SESSION( 0 ));
	Oid			   funcOID = PG_GETARG_OID( 1 );
	char		   sourceString[PLDBG_STRING_MAX_LEN];
	char		 * source;

	snprintf(
		sourceString, PLDBG_STRING_MAX_LEN, "%s %u",
		PLDBG_GET_SOURCE, funcOID
	);

	sendString( session, sourceString );

	source 		 = getNString( session );

	PG_RETURN_TEXT_P(cstring_to_text(source));
}

/*******************************************************************************
 * pldbg_get_breakpoints( sessionID INTEGER ) RETURNS SETOF breakpoint
 *
 *	This function returns a SETOF breakpoint tuples.  Each tuple in the result
 *	set identifies a breakpoint.
 *
 *	NOTE: the result set returned by this function should be identical to
 *	the result set returned by a SHOW BREAKPOINTS command.  This function
 *	may become obsolete when SHOW BREAKPOINTS is complete.
 */

Datum pldbg_get_breakpoints( PG_FUNCTION_ARGS )
{
	FuncCallContext * srf;

	debugSession * session = defaultSession( PG_GETARG_SESSION( 0 ));
	char         * breakpointString;

	if( SRF_IS_FIRSTCALL())
	{
		MemoryContext oldContext;

		srf = SRF_FIRSTCALL_INIT();

		oldContext = MemoryContextSwitchTo( srf->multi_call_memory_ctx );
		srf->attinmeta = TupleDescGetAttInMetadata( RelationNameGetTupleDesc( TYPE_NAME_BREAKPOINT ));
		MemoryContextSwitchTo( oldContext );

		sendString( session, PLDBG_GET_BREAKPOINTS );
	}
	else
	{
		srf = SRF_PERCALL_SETUP();
	}

	if(( breakpointString = getNString( session )) != NULL )
	{
		SRF_RETURN_NEXT( srf, buildBreakpointDatum( breakpointString ));
	}
	else
	{
		SRF_RETURN_DONE( srf );
	}
}

/*******************************************************************************
 * pldbg_get_variables( sessionID INTEGER ) RETURNS SETOF var
 *
 *	This function returns a SETOF var tuples.  Each tuple in the result
 *	set contains information about one local variable (or parameter) in the
 *	stack frame that has the focus.  Each tuple contains the name of the
 *	variable, the line number at which the variable was declared, a flag
 *	that tells you whether the name is unique within the scope of the function
 *	(if the name is not unique, a debugger client may use the line number to
 *	distinguish between variables with the same name), a flag that tells you
 *	whether the variables is a CONST, a flag that tells you whether the variable
 *	is NOT NULL, the data type of the variable (the OID of the corresponding
 *	pg_type) and the value of the variable.
 *
 *	To view variables defined in a different stack frame, call
 *	pldbg_select_frame() to change the debugger's focus to that frame.
 */

Datum pldbg_get_variables( PG_FUNCTION_ARGS )
{
	FuncCallContext * srf;

	debugSession * session = defaultSession( PG_GETARG_SESSION( 0 ));
	char         * variableString;

	if( SRF_IS_FIRSTCALL())
	{
		MemoryContext oldContext;

		srf = SRF_FIRSTCALL_INIT();

		oldContext = MemoryContextSwitchTo( srf->multi_call_memory_ctx );
		srf->attinmeta = TupleDescGetAttInMetadata( RelationNameGetTupleDesc( TYPE_NAME_VAR ));
		MemoryContextSwitchTo( oldContext );

		sendString( session, PLDBG_GET_VARIABLES );
	}
	else
	{
		srf = SRF_PERCALL_SETUP();
	}

	if(( variableString = getNString( session )) != NULL )
	{
		char	  * values[8];
		char      * ctx = NULL;
		HeapTuple   result;

		/*
		 * variableString points to a string like:
		 *	varName:class:lineNumber:unique:isConst:notNull:dataTypeOID
		 */
		values[0] = pstrdup( tokenize( variableString, ":", &ctx ));	/* variable name			*/
		values[1] = pstrdup( tokenize( NULL, ":", &ctx ));				/* var class				*/
		values[2] = pstrdup( tokenize( NULL, ":", &ctx ));				/* line number				*/
		values[3] = pstrdup( tokenize( NULL, ":", &ctx ));				/* unique					*/
		values[4] = pstrdup( tokenize( NULL, ":", &ctx ));				/* isConst					*/
		values[5] = pstrdup( tokenize( NULL, ":", &ctx ));				/* notNull					*/
		values[6] = pstrdup( tokenize( NULL, ":", &ctx ));				/* data type OID			*/
		values[7] = pstrdup( tokenize( NULL, NULL, &ctx ));				/* value (rest of string)	*/

		result = BuildTupleFromCStrings( srf->attinmeta, values );

		SRF_RETURN_NEXT( srf, HeapTupleGetDatum( result ));
	}
	else
	{
		SRF_RETURN_DONE( srf );
	}
}

/*******************************************************************************
 * pldbg_get_stack( sessionID INTEGER ) RETURNS SETOF frame
 *
 *	This function returns a SETOF frame tuples.  Each tuple in the result
 *	set contains information about one stack frame: the tuple contains the
 *	function OID, and line number within that function.  Each tuple also
 *	contains a string that you can use to display the name and value of each
 *	argument to that particular invocation.
 */

Datum pldbg_get_stack( PG_FUNCTION_ARGS )
{
	FuncCallContext * srf;

	debugSession * session = defaultSession( PG_GETARG_SESSION( 0 ));
	char         * frameString;

	if( SRF_IS_FIRSTCALL())
	{
		MemoryContext oldContext;

		srf = SRF_FIRSTCALL_INIT();

		oldContext = MemoryContextSwitchTo( srf->multi_call_memory_ctx );
		srf->attinmeta = TupleDescGetAttInMetadata( RelationNameGetTupleDesc( TYPE_NAME_FRAME ));
		MemoryContextSwitchTo( oldContext );

		sendString( session, PLDBG_GET_STACK );
	}
	else
	{
		srf = SRF_PERCALL_SETUP();
	}

	if(( frameString = getNString( session )) != NULL )
	{
		char	  * values[5];
		char		callCount[PLDBG_STRING_MAX_LEN];
		char      * ctx = NULL;
		HeapTuple   result;

		/*
		 * frameString points to a string like:
		 *	targetName:funcOID:lineNumber:arguments
		 */
		snprintf(
			callCount, PLDBG_STRING_MAX_LEN, UINT64_FORMAT,
			(uint64)srf->call_cntr
		);

		values[0] = callCount;
		values[1] = tokenize( frameString, ":", &ctx );	/* targetName					*/
		values[2] = tokenize( NULL, ":", &ctx );		/* funcOID						*/
		values[3] = tokenize( NULL, ":", &ctx );		/* lineNumber					*/
		values[4] = tokenize( NULL, NULL, &ctx );		/* arguments - rest of string 	*/

		result = BuildTupleFromCStrings( srf->attinmeta, values );

		SRF_RETURN_NEXT( srf, HeapTupleGetDatum( result ));
	}
	else
	{
		SRF_RETURN_DONE( srf );
	}
}

/********************************************************************************
 * pldbg_get_proxy_info( ) RETURNS proxyInfo
 *
 *  This function retrieves a small collection of parameters from the server, all
 *  parameters are related to the version of the server and the version of this
 *  proxy API.
 *
 *  You can call this function (from the debugger client process) to find out
 *  which version of the proxy API you are talking to - if this function does
 *  not exist, you can assume that you are talking to a version 1 proxy server.
 */

Datum pldbg_get_proxy_info( PG_FUNCTION_ARGS )
{
	Datum	  values[4] = {0};
	bool	  nulls[4]  = {0};
	TupleDesc tupleDesc = getResultTupleDesc( fcinfo );
	HeapTuple result;

	values[0] = DirectFunctionCall1( textin, PointerGetDatum( PG_VERSION_STR ));
	values[1] = Int32GetDatum( PG_VERSION_NUM );
	values[2] = Int32GetDatum( PROXY_API_VERSION );
	values[3] = Int32GetDatum( MyProcPid );

	result = heap_form_tuple( tupleDesc, values, nulls );

	PG_RETURN_DATUM( HeapTupleGetDatum( result ));
}

/*******************************************************************************
 * pldbg_set_breakpoint(sessionID INT, function OID, lineNumber INT)
 *	RETURNS boolean
 *
 * Sets a *local* breakpoint in the target process.
 */

Datum pldbg_set_breakpoint( PG_FUNCTION_ARGS )
{
	debugSession * session    = defaultSession( PG_GETARG_SESSION( 0 ));
	Oid			   funcOID    = PG_GETARG_OID( 1 );
	int			   lineNumber = PG_GETARG_INT32( 2 );
	char		   breakpointString[PLDBG_STRING_MAX_LEN];

	snprintf(
		breakpointString, PLDBG_STRING_MAX_LEN, "%s %u:%d",
		PLDBG_SET_BREAKPOINT, funcOID, lineNumber
	);

	sendString( session, breakpointString );

	PG_RETURN_BOOL( getBool( session ));
}

/*******************************************************************************
 * pldbg_drop_breakpoint(sessionID INT, function OID, lineNumber INT)
 *	RETURNS boolean
 */

Datum pldbg_drop_breakpoint( PG_FUNCTION_ARGS )
{
	debugSession * session    = defaultSession( PG_GETARG_SESSION( 0 ));
	Oid			   funcOID    = PG_GETARG_OID( 1 );
	int			   lineNumber = PG_GETARG_INT32( 2 );
	char		   breakpointString[PLDBG_STRING_MAX_LEN];

	snprintf(
		breakpointString, PLDBG_STRING_MAX_LEN, "%s %u:%d",
		PLDBG_CLEAR_BREAKPOINT, funcOID, lineNumber
	);

	sendString( session, breakpointString );

	PG_RETURN_BOOL( getBool( session ));
}

/*******************************************************************************
 * pldbg_deposit_value( sessionID INT, varName TEXT, lineNumber INT, value TEXT)
 *	RETURNS boolean
 *
 *	This function 'deposits' a new value into the given variable (identified by
 *	name and optional line number).  'value' is evaluated as an expression that
 *	must result in a value whose type matches the given variable (or whose type
 *  is coerce'able to the type of the given variable).
 */

Datum pldbg_deposit_value( PG_FUNCTION_ARGS )
{
	debugSession * session 	     = defaultSession( PG_GETARG_SESSION( 0 ));
	char         * varName 		 = GET_STR( PG_GETARG_TEXT_P( 1 ));
	int			   lineNumber 	 = PG_GETARG_INT32( 2 );
	char		 * value       	 = GET_STR( PG_GETARG_TEXT_P( 3 ));
	StringInfoData buf;

	initStringInfo( &buf );

	appendStringInfo( &buf, "%s %s.%d=%s", PLDBG_DEPOSIT, varName, lineNumber, value );

	sendString( session, buf.data );

	pfree( buf.data );

	PG_RETURN_BOOL( getBool( session ));

}

/*******************************************************************************
 * Local supporting (static) functions
 *******************************************************************************/

/*******************************************************************************
 * initializeModule()
 *
 *	Initializes the debugger proxy module.  For now, we just register a callback
 *	(cleanupAtExit()) that this backend will invoke on exit - we use that
 *	callback to gracefully close any outstanding connections.
 *
 *	NOTE: this would also be a good place to load the tuple descriptions for
 *		  each of the complex datatypes that we use (breakpoint, var, frame).
 */

static void initializeModule( void )
{
	static bool	initialized = FALSE;

	if( !initialized )
	{
		initialized = TRUE;

		on_shmem_exit( cleanupAtExit, 0 );
	}
}

/*******************************************************************************
 * defaultSession()
 *
 *	This function is designed to make it a little easier to build a simple
 *  debugger client.  Instead of managing session identifiers, you can simply
 *	pass '0' to each function that requires a session ID.  When a proxy function
 *  encounters a session ID of 0, it assumes that you want to work with the most
 *	recently used session.  If you have only one session, you can simply pass
 *  '0' to every function.  This is particularly handy if you're using the proxy
 *	API from a command line application like psql.
 *
 *	NOTE: If you give this function an invalid sessionHandle it will throw an
 *		  error. A sessionHandle is valid if returned by addSession().
 */

static debugSession * defaultSession( sessionHandle handle )
{
	debugSession * session;

	if( handle == 0 )
	{
		if( mostRecentSession == NULL )
			ereport( ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg( "invalid session handle" )));
		else
			return( mostRecentSession );
	}
	else
	{
		session = findSession( handle );

		if( session == NULL )
			ereport( ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg( "invalid session handle" )));
		else
		{
			mostRecentSession = session;
			return( session );
		}
	}

	return( NULL );	  /* keep the compiler happy */
}

/*******************************************************************************
 * initSessionHash()
 *
 *	Initialize a hash table that we use to map session handles (simple integer
 *	values) into debugSession pointers.
 *
 *  You should call this function before you use the hash - you can call it
 *  as many times as you like, it will only initialize the hash table on the
 *  first invocation.
 */

static void initSessionHash()
{
	if( sessionHash )
		return;
	else
	{
		HASHCTL	ctl = {0};

		ctl.keysize   = sizeof( sessionHandle );
		ctl.entrysize = sizeof( sessionHashEntry );
		ctl.hash      = tag_hash;

		sessionHash = hash_create( "Debugger sessions", 5, &ctl, HASH_ELEM | HASH_FUNCTION );
	}
}

/*******************************************************************************
 * addSession()
 *
 *	Adds a session (debugSession *) to the hash that we use to map session
 *  handles into debugSession pointers.  This function returns a handle that
 *	you should give back to the debugger client process.  When the debugger
 *  client calls us again, he gives us the handle and we map that back into
 *  a debugSession pointer.  That way, we don't have to expose a pointer to
 *	the debugger client (which can make for nasty denial of service hacks, not
 *  to mention 32-bit vs. 64-bit hassles).
 */

static sessionHandle addSession( debugSession * session )
{
	static sessionHandle nextHandle;
	sessionHashEntry   * entry;
	bool			   	 found;
	sessionHandle		 handle;

	initSessionHash();

	handle = ++nextHandle;

	entry = (sessionHashEntry *)hash_search( sessionHash, &handle, HASH_ENTER, &found );

	entry->m_handle  = handle;
	entry->m_session = session;

	return( handle );
}

/*******************************************************************************
 * findSession()
 *
 *	Given a sessionHandle (integer), this function returns the corresponding
 *  debugSession pointer.  If the sessionHandle is invalid (that is, it's a
 *  number not returned by addSession()), this function returns NULL.
 */

static debugSession * findSession( sessionHandle handle )
{
	sessionHashEntry * entry;

	initSessionHash();

	if(( entry = hash_search( sessionHash, &handle, HASH_FIND, NULL )) != NULL )
	{
		return( entry->m_session );
	}
	else
	{
		return( NULL );
	}
}


/*******************************************************************************
 * tokenize()
 *
 *	This is a re-entrant safe version of the standard C strtok() function.
 *	tokenize() will split a string (src) into multiple substrings separated by
 *	any of the characters in the delimiter string (delimiters).  Each time you
 *	call tokenize(), it returns the next subtstring (or NULL when all substrings
 *	have been exhausted). The first time you call this function, ctx should be
 *	NULL and src should point to the start of the string you are splitting.
 *	For every subsequent call, src should be NULL and tokenize() will manage
 *	ctx itself.
 *
 *	NOTE: the search string (src) is brutally altered by this function - make
 *		  a copy of the search string before you call tokenize() if you need the
 *		  original string.
 */

static char * tokenize( char * src, const char * delimiters, char ** ctx )
{
	char * start;
	char * end;

	if( src == NULL )
		src = *ctx;

	/*
	 * Special case - if delimiters is NULL, we just return the
	 * remainder of the string.
	 */

	if( delimiters == NULL )
		return( src );

	if( src == NULL )
		elog(ERROR, "debugger protocol error: token expected");

	/*
	 *	Skip past any leading delimiters
	 */

	start = src = ( src + strspn( src, delimiters ));

	if( *src == '\0' )
		return( "" );

	if(( end = strpbrk( start, delimiters )) == NULL )
	{
		*ctx = strchr( start, '\0' );
	}
	else
	{
		*end = '\0';
		*ctx = end + 1;
	}

	return( start );
}

/*******************************************************************************
 * readn()
 *
 *	This function reads exactly 'len' bytes from the given socket or it
 *  throws an error (ERRCODE_CONNECTION_FAILURE).  readn() will hang until
 *	the proper number of bytes have been read (or an error occurs).
 *
 *	Note: dst must point to a buffer large enough to hold at least 'len'
 *	bytes.  readn() returns dst (for convenience).
 */

static void * readn( int serverHandle, void * dst, size_t len )
{
	size_t	bytesRemaining = len;
	char  * buffer         = (char *)dst;

	if( serverHandle == -1 )
		ereport( ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg( "given session is not connected" )));

	while( bytesRemaining > 0 )
	{
		fd_set		rmask;
		ssize_t		bytesRead;

		/*
		 * Note: we want to wait for some number of bytes to arrive from the
		 * target process, but we also want to notice if the client process
		 * disappears.  To do that, we'll call select() before we call recv()
		 * and we'll tell select() to return as soon as something interesting
		 * happens on *either* of the sockets.  If the target sends us data
		 * first, we're ok (that's what we are expecting to happen).  If we
		 * detect any activity on the client-side socket (which is the libpq
		 * socket), we can assume that something's gone horribly wrong (most
		 * likely, the user killed the client by clicking the close button).
		 */

		FD_ZERO( &rmask );
		FD_SET( serverHandle, &rmask );
		FD_SET( MyProcPort->sock, &rmask );

		switch( select(( serverHandle > MyProcPort->sock ? serverHandle : MyProcPort->sock ) + 1, &rmask, NULL, NULL, NULL ))
		{
			case -1:
			{
				ereport( ERROR, ( ERRCODE_CONNECTION_FAILURE, errmsg( "select() failed waiting for target" )));
				break;
			}

			case 0:
			{
				/* Timer expired */
				return( NULL );
				break;
			}

			default:
			{
				/*
				 * We got traffic on one of the two sockets.  If we see traffic
				 * from the client (libpq) connection, just return to the
				 * caller so that libpq can process whatever's waiting.
				 * Presumably, the only time we'll see any libpq traffic here
				 * is when the client process has killed itself...
				 */

				if( FD_ISSET( MyProcPort->sock, &rmask ))
					ereport( ERROR, ( ERRCODE_CONNECTION_FAILURE, errmsg( "debugger connection(client side) terminated" )));
				break;
			}
		}

		bytesRead = recv( serverHandle, buffer, bytesRemaining, 0 );

		if( bytesRead <= 0 && errno != EINTR )
		{
			ereport( ERROR, (errcode(ERRCODE_CONNECTION_FAILURE), errmsg( "debugger connection terminated" )));
			return( NULL );
		}

		bytesRemaining -= bytesRead;
		buffer         += bytesRead;
	}

	return( dst );
}

/*******************************************************************************
 * writen()
 *
 *	This function writes exactly 'len' bytes to the given socket or it
 *  throws an error (ERRCODE_CONNECTION_FAILURE).  writen() will hang until
 *	the proper number of bytes have been written (or an error occurs).
 */

static void * writen( int serverHandle, void * src, size_t len )
{
	size_t	bytesRemaining = len;
	char  * buffer         = (char *)src;

	while( bytesRemaining > 0 )
	{
		ssize_t bytesWritten;

		if(( bytesWritten = send( serverHandle, buffer, bytesRemaining, 0 )) <= 0 )
		{
			ereport( ERROR, ( errcode( ERRCODE_CONNECTION_FAILURE ), errmsg( "debugger connection terminated" )));
			return( NULL );
		}

		bytesRemaining -= bytesWritten;
		buffer         += bytesWritten;
	}

	return( src );
}

/*******************************************************************************
 * sendBytes()
 *
 *	This function sends 'len' bytes to the server (identfied by a debugSession
 *	pointer).  'src' should point to the bytes that you want to send to the
 *	server.
 */

static void sendBytes( debugSession * session, void * src, size_t len )
{
	writen( session->serverSocket, src, len );
}


/*******************************************************************************
 * sendUInt32()
 *
 *	This function sends a uint32 value (val) to the debugger server.
 */

static void sendUInt32( debugSession * session, uint32 val )
{
	uint32	netVal = htonl( val );

	sendBytes( session, &netVal, sizeof( netVal ));
}

/*******************************************************************************
 * sendString()
 *
 *	This function sends a string value (src) to the debugger server.  'src'
 *	should point to a null-terminated string.  We send the length of the string
 *	(as a 32-bit unsigned integer), then the bytes that make up the string - we
 *	don't send the null-terminator.
 */

static void sendString( debugSession * session, char * src )
{
	size_t	len = strlen( src );

	sendUInt32( session, len );
	sendBytes( session, src, len );
}

/*******************************************************************************
 * getBool()
 *
 *	getBool() retreives a boolean value (TRUE or FALSE) from the server.  We
 *	call this function after we ask the server to do something that returns a
 *	boolean result (like deleting a breakpoint or depositing a new value).
 */

static bool getBool( debugSession * session )
{
	char * str;
	bool   result;

	str = getNString( session );

	if (str == NULL)
		elog(ERROR, "debugger protocol error; bool expected");

	if( str[0] == 't' )
		result = TRUE;
	else
		result = FALSE;

	pfree( str );

	return( result );
}


/*******************************************************************************
 * getUInt32()
 *
 *	Reads a 32-bit unsigned value from the server (and returns it in the host's
 *	byte ordering)
 */

static uint32 getUInt32( debugSession * session )
{
	uint32	result;

	readn( session->serverSocket, &result, sizeof( result ));

	return( ntohl( result ));
}

/******************************************************************************
 * getNstring()
 *
 *	This function is the opposite of sendString() - it reads a string from the
 *	debugger server.  The server sends the length of the string and then the
 *	bytes that make up the string (minus the null-terminator).  We palloc()
 *	enough space to hold the entire string (including the null-terminator) and
 *	return a pointer to that space (after, of course, reading the string from
 *	the server and tacking on the null-terminator).
 */

static char * getNString( debugSession * session )
{
	uint32 len = getUInt32( session );

	if( len == 0 )
		return( NULL );
	else
	{
		char * result = palloc( len + 1 );

		readn( session->serverSocket, result, len );

		result[len] = '\0';

		return( result );
	}
}

/*******************************************************************************
 * closeSession()
 *
 *	This function closes (in an orderly manner) the connection with the debugger
 *	server.
 */

static void closeSession( debugSession * session )
{
	if( session->serverSocket )
		closesocket( session->serverSocket );

	if( session->listener )
		BreakpointCleanupProc( MyProcPid );

	if( session->breakpointString )
		pfree( session->breakpointString );

	pfree( session );
}

/******************************************************************************
 * cleanupAtExit()
 *
 *	This is a callback function that the backend invokes when exiting.  At exit,
 *	we close any connections that we may still have (connections to debugger
 *	servers, that is).
 */

static void cleanupAtExit( int code, Datum arg )
{
	/*
	 * FIXME: we should clean up all of the sessions stored in the
	 *		  sessionHash.
	 */

	if( mostRecentSession )
		closeSession( mostRecentSession );

	mostRecentSession = NULL;
}

/*******************************************************************************
 * getResultTupleDesc()
 *
 *  If this function returns (without throwing an error), it returns a pointer
 *  to a description of the tuple that should be returned by the caller.
 *
 *	NOTE: the caller must have been called in a context that can accept a
 *		  set, not a context that expects a tuple.  That means that you
 *	      must invoke our caller with:
 *				select * from foo();
 * 		  instead of:
 *				select foo();
 */

static TupleDesc getResultTupleDesc( FunctionCallInfo fcinfo )
{
	ReturnSetInfo * rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;

	if( rsinfo == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));
	}
	return( rsinfo->expectedDesc );
}
