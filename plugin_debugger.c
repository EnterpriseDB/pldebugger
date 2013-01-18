/**********************************************************************
 * plugin_debugger.c	- Language-independent parts of debugger
 *
 * Copyright (c) 2004-2012 EnterpriseDB Corporation. All Rights Reserved.
 *
 * Licensed under the Artistic License, see 
 *		http://www.opensource.org/licenses/artistic-license.php
 * for full details
 *
 **********************************************************************/

#include "postgres.h"

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>

#ifdef WIN32
	#include<winsock2.h>
#else
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <arpa/inet.h>
#endif

#include "access/xact.h"
#include "lib/stringinfo.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "parser/parser.h"
#include "parser/parse_func.h"
#include "globalbp.h"
#include "storage/proc.h"							/* For MyProc		   */
#include "storage/procarray.h"						/* For BackendPidGetProc */
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "miscadmin.h"

#include "pldebugger.h"
#include "dbgcomm.h"

/* Include header for GETSTRUCT */
#if (PG_VERSION_NUM >= 90300)
#include "access/htup_details.h"
#endif

#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))

#define	TARGET_PROTO_VERSION	"1.0"

/**********************************************************************
 * Type and structure definitions
 **********************************************************************/

/* 
 * eConnectType
 *
 *	This enum defines the different ways that we can connect to the 
 *  debugger proxy.  
 *
 *		CONNECT_AS_SERVER means that we create a socket, bind an address to
 *		to that socket, send a NOTICE to our client application, and wait for
 *		a debugger proxy to attach to us.  That's what happens when	your 
 *		client application sets a local breakpoint and can handle the 
 *		NOTICE that we send.
 *
 *		CONNECT_AS_CLIENT means that a proxy has already created a socket
 *		and is waiting for a target (that's us) to connect to it. We do
 *		this kind of connection stuff when a debugger client sets a global
 *		breakpoint and we happen to blunder into that breakpoint.
 *
 *		CONNECT_UNKNOWN indicates a problem, we shouldn't ever see this.
 */

typedef enum
{
	CONNECT_AS_SERVER, 	/* Open a server socket and wait for a proxy to connect to us	*/
	CONNECT_AS_CLIENT,	/* Connect to a waiting proxy (global breakpoints do this)		*/
	CONNECT_UNKNOWN		/* Must already be connected 									*/
} eConnectType;

/**********************************************************************
 * Local (static) variables
 **********************************************************************/


per_session_ctx_t per_session_ctx;

errorHandlerCtx client_lost;

static debugger_language_t *debugger_languages[] = {
	&plpgsql_debugger_lang,
#ifdef INCLUDE_PACKAGE_SUPPORT
	&spl_debugger_lang,
#endif
	NULL
};

/**********************************************************************
 * Function declarations
 **********************************************************************/

void _PG_init( void );				/* initialize this module when we are dynamically loaded	*/

/**********************************************************************
 * Local (hidden) function prototypes
 **********************************************************************/

//static char       ** fetchArgNames( PLpgSQL_function * func, int * nameCount );
static void        * writen( int peer, void * src, size_t len );
static bool 		 connectAsServer( void );
static bool 		 connectAsClient( Breakpoint * breakpoint );
static bool 		 handle_socket_error(void);
static bool 		 parseBreakpoint( Oid * funcOID, int * lineNumber, char * breakpointString );
static bool 		 addLocalBreakpoint( Oid funcOID, int lineNo );
static void			 reserveBreakpoints( void );
static debugger_language_t *language_of_frame(ErrorContextCallback *frame);
static char * findSource( Oid oid, HeapTuple * tup );

static void do_deposit(ErrorContextCallback *frame, debugger_language_t *lang,
					   char *command);
static void send_breakpoints(Oid funcOid);
static void send_stack(void);
static void select_frame(int frameNo, ErrorContextCallback **frame_p, debugger_language_t **lang_p);


/**********************************************************************
 * Function definitions
 **********************************************************************/

void _PG_init( void )
{
	int i;

	/* Initialize all the per-language hooks. */
	for (i = 0; debugger_languages[i] != NULL; i++)
		debugger_languages[i]->initialize();

	reserveBreakpoints();
	dbgcomm_reserve();
}

/*
 * CREATE OR REPLACE FUNCTION pldbg_oid_debug( functionOID OID ) RETURNS INTEGER AS 'pldbg_oid_debug' LANGUAGE C;
 */

Datum pldbg_oid_debug(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pldbg_oid_debug);

Datum pldbg_oid_debug(PG_FUNCTION_ARGS)
{
	Oid			funcOid;
	HeapTuple	tuple;
	Oid			userid;

	if(( funcOid = PG_GETARG_OID( 0 )) == InvalidOid )
		ereport( ERROR, ( errcode( ERRCODE_UNDEFINED_FUNCTION ), errmsg( "no target specified" )));

	/* get the owner of the function */
	tuple = SearchSysCache(PROCOID,
				   ObjectIdGetDatum(funcOid),
				   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u",
			 funcOid);
	userid = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;
	ReleaseSysCache(tuple);

	if( !superuser() && (GetUserId() != userid))
		ereport( ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg( "must be owner or superuser to create a breakpoint" )));

	addLocalBreakpoint( funcOid, -1 );

	PG_RETURN_INT32( 0 );
}

/*
 * ---------------------------------------------------------------------
 * readn()
 *
 *	This function reads exactly 'len' bytes from the given socket or it 
 *	throws an error.  readn() will hang until the proper number of bytes 
 *	have been read (or an error occurs).
 *
 *	Note: dst must point to a buffer large enough to hold at least 'len' 
 *	bytes.  readn() returns dst (for convenience).
 */

static void * readn( int peer, void * dst, size_t len )
{
	size_t	bytesRemaining = len;
	char  * buffer         = (char *)dst;

	while( bytesRemaining > 0 )
	{
		ssize_t bytesRead = recv( peer, buffer, bytesRemaining, 0 );

		if( bytesRead <= 0 && errno != EINTR )
			handle_socket_error();

		bytesRemaining -= bytesRead;
		buffer         += bytesRead;
	}

	return( dst );
}

/*
 * ---------------------------------------------------------------------
 * readUInt32()
 *
 *	Reads a 32-bit unsigned value from the server (and returns it in the host's
 *	byte ordering)
 */

static uint32 readUInt32( int channel )
{
	uint32	netVal;

	readn( channel, &netVal, sizeof( netVal ));

	return( ntohl( netVal ));
}

/*
 * ---------------------------------------------------------------------
 * dbg_read_str()
 *
 *	This function reads a counted string from the given stream
 *	Returns a palloc'd, null-terminated string.
 *
 *	NOTE: the server-side of the debugger uses this function to read a 
 *		  string from the client side
 */

char *dbg_read_str( void )
{
	uint32 len;
	char *dst;
	int sock = per_session_ctx.client_r;

	len = readUInt32( sock );

	dst = palloc(len + 1);
	readn( sock, dst, len );
	
	dst[len] = '\0';
	return dst;
}

/*
 * ---------------------------------------------------------------------
 * writen()
 *
 *	This function writes exactly 'len' bytes to the given socket or it 
 *  	throws an error.  writen() will hang until the proper number of bytes
 *	have been written (or an error occurs).
 */

static void * writen( int peer, void * src, size_t len )
{
	size_t	bytesRemaining = len;
	char  * buffer         = (char *)src;

	while( bytesRemaining > 0 )
	{
		ssize_t bytesWritten;

		if(( bytesWritten = send( peer, buffer, bytesRemaining, 0 )) <= 0 )
			handle_socket_error();
		
		bytesRemaining -= bytesWritten;
		buffer         += bytesWritten;
	}

	return( src );
}


/*
 * ---------------------------------------------------------------------
 * sendUInt32()
 *
 *	This function sends a uint32 value (val) to the debugger server.
 */

static void sendUInt32( int channel, uint32 val )
{
	uint32	netVal = htonl( val );

	writen( channel, &netVal, sizeof( netVal ));
}

/*
 * ---------------------------------------------------------------------
 * dbg_send()
 *
 *	This function writes a formatted, counted string to the
 *	given stream.  The argument list for this function is identical to
 *	the argument list for the fprintf() function - you provide a socket,
 *	a format string, and then some number of arguments whose meanings 
 *	are defined by the format string.
 *
 *	NOTE:  the server-side of the debugger uses this function to send
 *		   data to the client side.  If the connection drops, dbg_send()
 *		   will longjmp() back to the debugger top-level so that the 
 *		   server-side can respond properly.
 */

void dbg_send( const char *fmt, ... )
{
	StringInfoData	result;
	char		   *data;
	size_t			remaining;
	int				sock = per_session_ctx.client_w;
	
	if( !sock )
		return;

	initStringInfo(&result);

	for (;;)
	{
		va_list	args;
		bool	success;

		va_start(args, fmt);
		success = appendStringInfoVA(&result, fmt, args);
		va_end(args);

		if (success)
			break;

		enlargeStringInfo(&result, result.maxlen);
	}

	data = result.data;
	remaining = strlen(data);

	sendUInt32(sock, remaining);

	while( remaining > 0 )
	{
		int written = send( sock, data, remaining, 0 );

		if(written < 0)
		{	
			handle_socket_error();
			continue;
		}

		remaining -= written;
		data      += written;
	}

	pfree(result.data);
}


/*
 * ---------------------------------------------------------------------
 * dbg_send_src()
 *
 *	dbg_send_src() sends the source code for a function to the client.
 *
 *  The client caches the source code that we send it and uses xmin/cmin
 *  to ensure the validity of the cache.
 */

static void dbg_send_src( char * command  )
{
    HeapTuple			tup;
    char				*procSrc;
	Oid					targetOid = InvalidOid;  /* Initialize to keep compiler happy */

	targetOid = atoi( command + 2 );

	/* Find the source code for this function */
	procSrc = findSource( targetOid, &tup );

	/* Found it - now send the source to the client */

	dbg_send( "%s", procSrc );

	/* Release the process tuple and send a footer to the client so he knows we're finished */

    ReleaseSysCache( tup );
}

/*
 * ---------------------------------------------------------------------
 * findSource()
 *
 *	This function locates and returns a pointer to a null-terminated string
 *	that contains the source code for the given function (identified by its
 *	OID).
 *
 *	In addition to returning a pointer to the requested source code, this
 *	function sets *tup to point to a HeapTuple (that you must release when 
 *	you are finished with it).
 */

static char * findSource( Oid oid, HeapTuple * tup )
{
	bool	isNull;

	*tup = SearchSysCache( PROCOID, ObjectIdGetDatum( oid ), 0, 0, 0 );

	if(!HeapTupleIsValid( *tup ))
		elog( ERROR, "pldebugger: cache lookup for proc %u failed", oid );

	return( DatumGetCString( DirectFunctionCall1( textout, SysCacheGetAttr( PROCOID, *tup, Anum_pg_proc_prosrc, &isNull ))));
}


/*
 * ---------------------------------------------------------------------
 * attach_to_proxy()
 *
 *	This function creates a connection to the debugger client (via the
 *  proxy process). attach_to_proxy() will hang the PostgreSQL backend
 *  until the debugger client completes the connection.
 *
 *	We start by asking the TCP/IP stack to allocate an unused port, then we 
 *	extract the port number from the resulting socket, send the port number to
 *	the client application (by raising a NOTICE), and finally, we wait for the
 *	client to connect.
 *
 *	We assume that the client application knows the IP address of the PostgreSQL
 *	backend process - if that turns out to be a poor assumption, we can include 
 *	the IP address in the notification string that we send to the client application.
 */

bool attach_to_proxy( Breakpoint * breakpoint )
{
	bool			result;
	errorHandlerCtx	save;

    if( per_session_ctx.client_w )
	{
		/* We're already connected to a live proxy, just go home */
		return( TRUE );
	}

	if( breakpoint == NULL )
	{
		/* 
		 * No breakpoint - that implies that we're 'stepping into'.
		 * We had better already have a connection to a proxy here
		 * (how could we be 'stepping into' if we aren't connected
		 * to a proxy?)
		 */
		return( FALSE );
	}

	/*
	 * When a networking error is detected, we longjmp() to the client_lost
	 * error handler - that normally points to a location inside of dbg_newstmt()
	 * but we want to handle any network errors that arise while we are 
	 * setting up a link to the proxy.  So, we save the original client_lost
	 * error handler context and push our own context on to the stack.
	 */

	save = client_lost;
	
	if( sigsetjmp( client_lost.m_savepoint, 1 ) != 0 )
	{
		client_lost = save;
		return( FALSE );
	}

    if( breakpoint->data.proxyPort == -1 )
	{
		/*
		 * proxyPort == -1 implies that this is a local breakpoint,
		 * create a server socket and wait for the proxy to contact
		 * us.
		 */
		result = connectAsServer();
	}
	else
	{
		/*
		 * proxyPort != -1 implies that this is a global breakpoint,
		 * a debugger proxy is already waiting for us at the given
		 * port (on this host), connect to that proxy.
		 */

		result = connectAsClient( breakpoint );
	}

	/*
	 * Now restore the original error handler context so that
	 * dbg_newstmt() can handle any future network errors.
	 */

	client_lost = save;
	return( result );
}

/*
 * ---------------------------------------------------------------------
 * connectAsServer()
 *
 *	This function creates a socket, asks the TCP/IP stack to bind it to
 *	an unused port, and then waits for a debugger proxy to connect to
 *	that port.  We send a NOTICE to our client process (on the other 
 *	end of the fe/be connection) to let the client know that it should
 *	fire up a debugger and attach to that port (the NOTICE includes 
 *	the port number)
 */

static bool connectAsServer( void )
{
	int			client_sock;

	client_sock = dbgcomm_listen_for_proxy();
	if (client_sock < 0)
	{
		per_session_ctx.client_w = per_session_ctx.client_r = 0;
	    return( FALSE );
	}
	else
	{
		per_session_ctx.client_w = client_sock;
		per_session_ctx.client_r = client_sock;
		return( TRUE );
	}
}

/*
 * ---------------------------------------------------------------------
 * connectAsClient()
 *
 *	This function connects to a waiting proxy process over the given
 *  port. We got the port number from a global breakpoint (the proxy
 *	stores it's port number in the breakpoint so we'll know how to 
 *	find that proxy).
 */

static bool connectAsClient( Breakpoint * breakpoint )
{
	int					 proxySocket;

	proxySocket = dbgcomm_connect_to_proxy(breakpoint->data.proxyPort);

	if (proxySocket < 0 )
	{
		/* dbgcomm_connect_to_proxy already logged the reason */
		return false;
	}
	else
	{
		per_session_ctx.client_w = proxySocket;
		per_session_ctx.client_r = proxySocket;

		BreakpointBusySession( breakpoint->data.proxyPid );
		return true;
	}
}

/*
 * ---------------------------------------------------------------------
 * parseBreakpoint()
 *
 *	Given a string that formatted like "funcOID:linenumber", 
 *	this function parses out the components and returns them to the 
 *	caller.  If the string is well-formatted, this function returns 
 *	TRUE, otherwise, we return FALSE.
 */

static bool parseBreakpoint( Oid * funcOID, int * lineNumber, char * breakpointString )
{
	int a, b;
	int n;

	n = sscanf(breakpointString, "%d:%d", &a, &b);
	if (n == 2)
	{
		*funcOID = a;
		*lineNumber = b;
	}
	else
		return false;

	return( TRUE );
}

/*
 * ---------------------------------------------------------------------
 * addLocalBreakpoint()
 *
 *	This function adds a local breakpoint for the given function and 
 *	line number
 */

static bool addLocalBreakpoint( Oid funcOID, int lineNo )
{
	Breakpoint breakpoint;
	
	breakpoint.key.databaseId = MyProc->databaseId;
	breakpoint.key.functionId = funcOID;
	breakpoint.key.lineNumber = lineNo;
	breakpoint.key.targetPid  = MyProc->pid;
	breakpoint.data.isTmp     = FALSE;
	breakpoint.data.proxyPort = -1;
	breakpoint.data.proxyPid  = -1;

	return( BreakpointInsert( BP_LOCAL, &breakpoint.key, &breakpoint.data ));
}

/*
 * ---------------------------------------------------------------------
 * setBreakpoint()
 *
 *	The debugger client can set a local breakpoint at a given 
 *	function/procedure and line number by calling	this function
 *  (through the debugger proxy process).
 */

void setBreakpoint( char * command )
{
	/* 
	 *  Format is 'b funcOID:lineNumber'
	 */
	int			  lineNo;
	Oid			  funcOID;

	if( parseBreakpoint( &funcOID, &lineNo, command + 2 ))
	{
		if( addLocalBreakpoint( funcOID, lineNo ))
			dbg_send( "%s", "t" );
		else
			dbg_send( "%s", "f" );
	}
	else
	{
		dbg_send( "%s", "f" );
	}
}

/*
 * ---------------------------------------------------------------------
 * clearBreakpoint()
 *
 *	This function deletes the breakpoint at the package,
 *	function/procedure, and line number indicated by the
 *	given command.
 *
 *	For now, we maintain our own private list of breakpoints -
 *	later, we'll use the same list managed by the CREATE/
 *	DROP BREAKPOINT commands.
 */

void clearBreakpoint( char * command )
{
	/* 
	 *  Format is 'f funcOID:lineNumber'
	 */
	int			  lineNo;
	Oid			  funcOID;

	if( parseBreakpoint( &funcOID, &lineNo, command + 2 ))
	{
		Breakpoint breakpoint;
	
		breakpoint.key.databaseId = MyProc->databaseId;
		breakpoint.key.functionId = funcOID;
		breakpoint.key.lineNumber = lineNo;
		breakpoint.key.targetPid  = MyProc->pid;

		if( BreakpointDelete( BP_LOCAL, &breakpoint.key ))
			dbg_send( "t" );
		else
			dbg_send( "f" );
	}
	else
	{
		dbg_send( "f" ); 
	}
}

bool breakAtThisLine( Breakpoint ** dst, eBreakpointScope * scope, Oid funcOid, int lineNumber )
{
	BreakpointKey		key;

	key.databaseId = MyProc->databaseId;
	key.functionId = funcOid;
    key.lineNumber = lineNumber;

	if( per_session_ctx.step_into_next_func )
	{
		*dst   = NULL;
		*scope = BP_LOCAL;
		return( TRUE );
	}

	/*
	 *  We conduct 3 searches here.  
	 *	
	 *	First, we look for a global breakpoint at this line, targeting our
	 *  specific backend process.
	 *
	 *  Next, we look for a global breakpoint (at this line) that does
	 *  not target a specific backend process.
	 *
	 *	Finally, we look for a local breakpoint at this line (implicitly 
	 *  targeting our specific backend process).
	 *
	 *	NOTE:  We must do the local-breakpoint search last because, when the
	 *		   proxy attaches to our process, it marks all of its global
	 *		   breakpoints as busy (so other potential targets will ignore
	 *		   those breakpoints) and we copy all of those global breakpoints
	 *		   into our local breakpoint hash.  If the debugger client exits
	 *		   and the user starts another debugger session, we want to see the
	 *		   new breakpoints instead of our obsolete local breakpoints (we
	 *		   don't have a good way to detect that the proxy has disconnected
	 *		   until it's inconvenient - we have to read-from or write-to the
	 *		   proxy before we can tell that it's died).
	 */

	key.targetPid = MyProc->pid;		/* Search for a global breakpoint targeted at our process ID */
  
	if((( *dst = BreakpointLookup( BP_GLOBAL, &key )) != NULL ) && ((*dst)->data.busy == FALSE ))
	{
		*scope = BP_GLOBAL;
		return( TRUE );
	}

	key.targetPid = -1;					/* Search for a global breakpoint targeted at any process ID */

	if((( *dst = BreakpointLookup( BP_GLOBAL, &key )) != NULL ) && ((*dst)->data.busy == FALSE ))
	{
		*scope = BP_GLOBAL;
		return( TRUE );
	}

	key.targetPid = MyProc->pid;	 	/* Search for a local breakpoint (targeted at our process ID) */

	if(( *dst = BreakpointLookup( BP_LOCAL, &key )) != NULL )
	{
		*scope = BP_LOCAL;
		return( TRUE );
	}

	return( FALSE );
}   

bool breakpointsForFunction( Oid funcOid )
{
	if( BreakpointOnId( BP_LOCAL, funcOid ) || BreakpointOnId( BP_GLOBAL, funcOid ))
		return( TRUE );
	else
		return( FALSE );

}

/* ---------------------------------------------------------------------
 * handle_socket_error()
 *
 * when invoked after a socket operation it would check socket operation's
 * last error status and invoke siglongjmp incase the error is fatal. 
 */
static bool handle_socket_error(void)
{
	int		err;
	bool	abort = TRUE;

#ifdef WIN32
		err = WSAGetLastError();
			
		switch(err) 
		{
		
			case WSAEINTR:
			case WSAEBADF:
			case WSAEACCES:
			case WSAEFAULT:
			case WSAEINVAL:
			case WSAEMFILE:
				
			/*
			 * Windows Sockets definitions of regular Berkeley error constants
			 */
			case WSAEWOULDBLOCK:
			case WSAEINPROGRESS:
			case WSAEALREADY:
			case WSAENOTSOCK:
			case WSAEDESTADDRREQ:
			case WSAEMSGSIZE:
			case WSAEPROTOTYPE:
			case WSAENOPROTOOPT:
			case WSAEPROTONOSUPPORT:
			case WSAESOCKTNOSUPPORT:
			case WSAEOPNOTSUPP:
			case WSAEPFNOSUPPORT:
			case WSAEAFNOSUPPORT:
			case WSAEADDRINUSE:
			case WSAEADDRNOTAVAIL:
			case WSAENOBUFS:
			case WSAEISCONN:
			case WSAENOTCONN:
			case WSAETOOMANYREFS:
			case WSAETIMEDOUT:
			case WSAELOOP:
			case WSAENAMETOOLONG:
			case WSAEHOSTUNREACH:
			case WSAENOTEMPTY:
			case WSAEPROCLIM:
			case WSAEUSERS:
			case WSAEDQUOT:
			case WSAESTALE:
			case WSAEREMOTE:
				  
			/*
			 *	Extended Windows Sockets error constant definitions
			 */
			case WSASYSNOTREADY:
			case WSAVERNOTSUPPORTED:
			case WSANOTINITIALISED:
			case WSAEDISCON:
			case WSAENOMORE:
			case WSAECANCELLED:
			case WSAEINVALIDPROCTABLE:
			case WSAEINVALIDPROVIDER:
			case WSAEPROVIDERFAILEDINIT:
			case WSASYSCALLFAILURE:
			case WSASERVICE_NOT_FOUND:
			case WSATYPE_NOT_FOUND:
			case WSA_E_NO_MORE:
			case WSA_E_CANCELLED:
			case WSAEREFUSED:
				break;

			/*
			 *	Server should shut down its socket on these errors.
			 */		
			case WSAENETDOWN:
			case WSAENETUNREACH:
			case WSAENETRESET:
			case WSAECONNABORTED:
			case WSAESHUTDOWN:
			case WSAEHOSTDOWN:
			case WSAECONNREFUSED:
			case WSAECONNRESET:		
				abort = TRUE;
				break;
			
			default:
				;
		}

		if(abort)
		{
			LPVOID lpMsgBuf;
			FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,	NULL,err,
					   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),(LPTSTR) &lpMsgBuf,0, NULL );
		
			elog(COMMERROR,"%s", (char *)lpMsgBuf);		
			LocalFree(lpMsgBuf);
			
			siglongjmp(client_lost.m_savepoint, 1);
		}
		
#else	
		
		err = errno;
		switch(err) 
		{
			case EINTR:
			case ECONNREFUSED:
			case EPIPE:
			case ENOTCONN:
				abort =	TRUE;
				break;
				
			case ENOTSOCK:
			case EAGAIN:
			case EFAULT:
			case ENOMEM:
			case EINVAL:
			default:		
				break;
		}
		
		if(abort)
		{
			if(( err ) && ( err != EPIPE ))
				elog(COMMERROR, "%s", strerror(err)); 

			siglongjmp(client_lost.m_savepoint, 1);	
		}		
			
		errno = err;
#endif
		
	return abort;
}

/*
 * Returns true if we continue stepping in this frame. False otherwise.
 */
bool
plugin_debugger_main_loop(void)
{
	ErrorContextCallback *frame;
	debugger_language_t *lang; /* language of the selected frame */
	bool	need_more     = TRUE;
	char   *command;
	bool	retval = TRUE;

	/* Initially, set focus on the topmost frame in the stack */

	for( frame = error_context_stack; frame; frame = frame->previous )
	{
		/*
		 * ignore unrecognized stack frames.
		 */
		lang = language_of_frame(frame);
		if (lang)
			break;
	}
	if (frame == NULL)
	{
		/*
		 * Oops, couldn't find a frame that we recognize in the stack. This
		 * shouldn't happen since we're stopped at a breakpoint.
		 */
		elog(WARNING, "could not find PL/pgSQL frame at the top of the stack");
		return false;
	}

	/* Report the current location */
	lang->send_cur_line(frame);

	/* 
	 * Loop through the following chunk of code until we get a command
	 * from the user that would let us execute this PL/pgSQL statement.
	 */
	while( need_more )
	{
		/* Wait for a command from the debugger client */
		command = dbg_read_str();

		/*
		 * The debugger client sent us a null-terminated command string
		 *
		 * Each command starts with a single character and is
		 * followed by set of optional arguments.
		 */
		switch( command[0] )
		{
			case PLDBG_CONTINUE:
			{
				/*
				 * Continue (stop single-stepping and just run to the next breakpoint)
				 */
				retval = false;
				need_more = FALSE;
				break;
			}

			case PLDBG_SET_BREAKPOINT:
			{
				setBreakpoint( command );
				break;
			}
				
			case PLDBG_CLEAR_BREAKPOINT:
			{
				clearBreakpoint( command );
				break;
			}

			case PLDBG_PRINT_VAR:
			{
				/*
				 * Print value of given variable 
				 */

				lang->print_var( frame, &command[2], -1 );
				break;
			}

			case PLDBG_LIST_BREAKPOINTS:
			{
				send_breakpoints( lang->get_func_oid(frame) );
				break;
			}

			case PLDBG_STEP_INTO:
			{
				/* 
				 * Single-step/step-into
				 */
				per_session_ctx.step_into_next_func = TRUE;
				need_more = FALSE;
				break;
			}

			case PLDBG_STEP_OVER:
			{
				/*
				 * Single-step/step-over
				 */
				need_more = FALSE;
				break;
			}

			case PLDBG_LIST:
			{
				/*
				 * Send source code for given function
				 */
				dbg_send_src( command );
				break;
			}

			case PLDBG_PRINT_STACK:
			{
				send_stack();
				break;
			}

			case PLDBG_SELECT_FRAME:
			{
				select_frame(atoi( &command[2] ), &frame, &lang);
				/* Report the new location */
				lang->send_cur_line( frame );
				break;
				
			}

			case PLDBG_DEPOSIT:
			{
				/* 
				 * Deposit a new value into the given variable
				 */
				do_deposit(frame, lang, command);
				break;
			}

			case PLDBG_INFO_VARS:
			{
				/*
				 * Send list of variables (and their values)
				 */
				lang->send_vars( frame );
				break;
			}
					
			case PLDBG_RESTART:
			case PLDBG_STOP:
			{
				/* stop the debugging session */
				dbg_send( "%s", "t" );

				ereport(ERROR,
						(errcode(ERRCODE_QUERY_CANCELED),
						 errmsg("canceling statement due to user request")));
				break;
			}

			default:
				elog(WARNING, "unrecognized message %c", command[0]);
		}
		pfree(command);
	}

	return retval;
}

static void
do_deposit(ErrorContextCallback *frame, debugger_language_t *lang,
		   char *command)
{
	char    	  *var_name;
	char      	  *value;
	char      	  *lineno_s;
	int			   lineno;

	/* command = d:var.line=expr */

	var_name = command + 2;
    value  = strchr( var_name, '=' ); /* FIXME: handle quoted identifiers here */
	if (!value)
	{
		dbg_send( "%s", "f" );
		return;
	}
	*value = '\0';
	value++;

    lineno_s = strchr( var_name, '.' ); /* FIXME: handle quoted identifiers here */
	if (!lineno_s)
	{
		dbg_send( "%s", "f" );
		return;
	}
	*lineno_s = '\0';
	lineno_s++;
	if (strlen(lineno_s) == 0)
		lineno = -1;
	else
		lineno = atoi(lineno_s);

	if (lang->do_deposit(frame, var_name, lineno, value))
		dbg_send( "%s", "t" );
	else
		dbg_send( "%s", "f" );
}


/*
 * ---------------------------------------------------------------------
 * sendBreakpoints()
 *
 *	This function sends a list of breakpoints to the proxy process.
 *
 *	We only send the breakpoints defined in the given frame.
 *	
 *	For now, we maintain our own private list of breakpoints -
 *	later, we'll use the same list managed by the CREATE/
 *	DROP BREAKPOINT commands.
 */
static void
send_breakpoints(Oid funcOid)
{
	Breakpoint      * breakpoint;
	HASH_SEQ_STATUS	  scan;

	BreakpointGetList( BP_GLOBAL, &scan );

	while(( breakpoint = (Breakpoint *) hash_seq_search( &scan )) != NULL )
	{
		if(( breakpoint->key.targetPid == -1 ) || ( breakpoint->key.targetPid == MyProc->pid ))
			if( breakpoint->key.databaseId == MyProc->databaseId )
				if( breakpoint->key.functionId == funcOid )
					dbg_send( "%d:%d:%s", funcOid, breakpoint->key.lineNumber, "" );
	}

	BreakpointReleaseList( BP_GLOBAL );

	BreakpointGetList( BP_LOCAL, &scan );

	while(( breakpoint = (Breakpoint *) hash_seq_search( &scan )) != NULL )
	{
		if(( breakpoint->key.targetPid == -1 ) || ( breakpoint->key.targetPid == MyProc->pid ))
			if( breakpoint->key.databaseId == MyProc->databaseId )
				if( breakpoint->key.functionId == funcOid )
					dbg_send( "%d:%d:%s", funcOid, breakpoint->key.lineNumber, "" );
	}

	BreakpointReleaseList( BP_LOCAL );

	dbg_send( "%s", "" );	/* empty string indicates end of list */
}


/*
 * ---------------------------------------------------------------------
 * select_frame()
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
 */
static void
select_frame(int frameNo, ErrorContextCallback **frame_p, debugger_language_t **lang_p)
{
	ErrorContextCallback *frame;

	for( frame = error_context_stack; frame; frame = frame->previous )
	{
		debugger_language_t *lang = language_of_frame(frame);
		if (!lang)
			continue;

		if( frameNo-- == 0 )
		{
			lang->select_frame(frame);

			*frame_p = frame;
			*lang_p = lang;
		}
	}

	/* Not found. Keep frame unchanged */
}

/*
 * Returns the debugger_language_t struct representing the language that
 * this stack frame belongs to. Or NULL if we don't have a handler for it.
 */
static debugger_language_t *
language_of_frame(ErrorContextCallback *frame)
{
	debugger_language_t *lang;
	int			i;

	for (i = 0; debugger_languages[i] != NULL; i++)
	{
		lang = debugger_languages[i];
		if (lang->frame_belongs_to_me(frame))
			return lang;
	}
	return NULL;
}

/* ------------------------------------------------------------------
 * send_stack()
 * 
 *   This function sends the call stack to the debugger client.  For
 *	 each PL/pgSQL stack frame that we find, we send the function name,
 *	 argument names and values, and the current line number (within 
 *	 that particular invocation).
 */
static void
send_stack( void )
{
	ErrorContextCallback * entry;

	for( entry = error_context_stack; entry; entry = entry->previous )
	{
		/*
		 * ignore frames we don't recognize
		 */
		debugger_language_t *lang = language_of_frame(entry);
		if (lang != NULL)
			lang->send_stack_frame(entry);
	}

	dbg_send( "%s", "" );	/* empty string indicates end of list */
}

////////////////////////////////////////////////////////////////////////////////


/*-------------------------------------------------------------------------------------
 * The shared hash table for global breakpoints. It is protected by 
 * breakpointLock
 *-------------------------------------------------------------------------------------
 */
static LWLockId  breakpointLock;
static HTAB    * globalBreakpoints = NULL;
static HTAB    * localBreakpoints  = NULL;

/*-------------------------------------------------------------------------------------
 * The size of Breakpoints is determined by globalBreakpointCount (should be a GUC)
 *-------------------------------------------------------------------------------------
 */
static int		globalBreakpointCount = 20;
static Size		breakpoint_hash_size;
static Size		breakcount_hash_size;

/*-------------------------------------------------------------------------------------
 * Another shared hash table which tracks number of breakpoints created
 * against each entity.
 *
 * It is also protected by breakpointLock, thus making operations on Breakpoints
 * BreakCounts atomic.
 *-------------------------------------------------------------------------------------
 */
static HTAB *globalBreakCounts;
static HTAB *localBreakCounts;

typedef struct BreakCountKey
{
	Oid			databaseId;
    Oid			functionId;
} BreakCountKey;

typedef struct BreakCount
{
	BreakCountKey	key;
	int				count;
} BreakCount;

/*-------------------------------------------------------------------------------------
 * Prototypes for functions which operate on GlobalBreakCounts.
 *-------------------------------------------------------------------------------------
 */
static void initLocalBreakpoints(void);
static void initLocalBreakCounts(void);

static void   breakCountInsert(eBreakpointScope scope, BreakCountKey *key);
static void   breakCountDelete(eBreakpointScope scope, BreakCountKey *key);
static int    breakCountLookup(eBreakpointScope scope, BreakCountKey *key, bool *found);
static HTAB * getBreakpointHash(eBreakpointScope scope);
static HTAB * getBreakCountHash(eBreakpointScope scope);

static void reserveBreakpoints( void )
{
	breakpoint_hash_size = hash_estimate_size(globalBreakpointCount, sizeof(Breakpoint));
	breakcount_hash_size = hash_estimate_size(globalBreakpointCount, sizeof(BreakCount));

	RequestAddinShmemSpace( add_size( breakpoint_hash_size, breakcount_hash_size ));
	RequestAddinLWLocks( 1 );
}

static void
initializeHashTables(void)
{
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	initGlobalBreakpoints();

	LWLockRelease(AddinShmemInitLock);

	initLocalBreakpoints();
	initLocalBreakCounts();
}

static void
initLocalBreakpoints(void)
{
	HASHCTL	ctl = {0};

	ctl.keysize   = sizeof(BreakpointKey);
	ctl.entrysize = sizeof(Breakpoint);
	ctl.hash      = tag_hash;

	localBreakpoints = hash_create("Local Breakpoints", 128, &ctl, HASH_ELEM | HASH_FUNCTION);
}

void
initGlobalBreakpoints(void)
{
	bool   	  		found;
	LWLockId	   *lockId;
	int				tableEntries = globalBreakpointCount;

	if(( lockId = ((LWLockId *)ShmemInitStruct( "Global Breakpoint LockId", sizeof( LWLockId ), &found ))) == NULL )
		elog(ERROR, "out of shared memory");
	else
	{
		HASHCTL breakpointCtl = {0};
		HASHCTL breakcountCtl = {0};

		/*
		 * Request a LWLock, store the ID in breakpointLock and store the ID
		 * in shared memory so other processes can find it later.
		 */
		if (!found)
			*lockId = breakpointLock = LWLockAssign();
		else
			breakpointLock = *lockId;

		/*
		 * Now create a shared-memory hash to hold our global breakpoints
		 */
		breakpointCtl.keysize   = sizeof(BreakpointKey);
		breakpointCtl.entrysize = sizeof(Breakpoint);
		breakpointCtl.hash 	  	= tag_hash;

		globalBreakpoints = ShmemInitHash("Global Breakpoints Table", tableEntries, tableEntries, &breakpointCtl, HASH_ELEM | HASH_FUNCTION);

		if (!globalBreakpoints)
			elog(FATAL, "could not initialize global breakpoints hash table");

		/*
		 * And create a shared-memory hash to hold our global breakpoint counts
		 */
		breakcountCtl.keysize   = sizeof(BreakCountKey);
		breakcountCtl.entrysize = sizeof(BreakCount);
		breakcountCtl.hash    	= tag_hash;

		globalBreakCounts = ShmemInitHash("Global BreakCounts Table", tableEntries, tableEntries, &breakcountCtl, HASH_ELEM | HASH_FUNCTION);

		if (!globalBreakCounts)
			elog(FATAL, "could not initialize global breakpoints count hash table");
	}
}


/* ---------------------------------------------------------
 * getPLDebuggerLock()
 *
 *	Returns the lockid of the lock used to protect pldebugger shared memory
 *  structures. The lock is called breakpointLock in this file, but it's
 *  also shared by dbgcommm.c.
 */

LWLockId
getPLDebuggerLock(void)
{
	if( localBreakpoints == NULL )
		initializeHashTables();

	return breakpointLock;
}

/* ---------------------------------------------------------
 * acquireLock()
 *
 *	This function waits for a lightweight lock that protects
 *  the breakpoint and breakcount hash tables at the given
 *	scope.  If scope is BP_GLOBAL, this function locks
 * 	breakpointLock. If scope is BP_LOCAL, this function
 *	doesn't lock anything because local breakpoints are,
 *	well, local (clever naming convention, huh?)
 */

static void
acquireLock(eBreakpointScope scope, LWLockMode mode)
{
	if( localBreakpoints == NULL )
		initializeHashTables();

	if (scope == BP_GLOBAL)
		LWLockAcquire(breakpointLock, mode);
}

/* ---------------------------------------------------------
 * releaseLock()
 *
 *	This function releases the lightweight lock that protects
 *  the breakpoint and breakcount hash tables at the given
 *	scope.  If scope is BP_GLOBAL, this function releases
 * 	breakpointLock. If scope is BP_LOCAL, this function
 *	doesn't do anything because local breakpoints are not
 *  protected by a lwlock.
 */

static void
releaseLock(eBreakpointScope scope)
{
	if (scope == BP_GLOBAL)
		LWLockRelease(breakpointLock);
}

/* ---------------------------------------------------------
 * BreakpointLookup()
 *
 * lookup the given global breakpoint hash key. Returns an instance
 * of Breakpoint structure
 */
Breakpoint *
BreakpointLookup(eBreakpointScope scope, BreakpointKey *key)
{
	Breakpoint	*entry;
	bool		 found;

	acquireLock(scope, LW_SHARED);
	entry = (Breakpoint *) hash_search( getBreakpointHash(scope), (void *) key, HASH_FIND, &found);
	releaseLock(scope);

	return entry;
}

/* ---------------------------------------------------------
 * BreakpointOnId()
 *
 * This is where we see the real advantage of the existence of BreakCounts.
 * It returns true if there is a global breakpoint on the given id, false
 * otherwise. The hash key of Global breakpoints table is a composition of Oid
 * and lineno. Therefore lookups on the basis of Oid only are not possible.
 * With this function however callers can determine whether a breakpoint is
 * marked on the given entity id with the cost of one lookup only.
 *
 * The check is made by looking up id in BreakCounts.
 */
bool
BreakpointOnId(eBreakpointScope scope, Oid funcOid)
{
	bool			found = false;
	BreakCountKey	key;

	key.databaseId = MyProc->databaseId;
	key.functionId = funcOid;

	acquireLock(scope, LW_SHARED);
	breakCountLookup(scope, &key, &found);
	releaseLock(scope);

	return found;
}

/* ---------------------------------------------------------
 * BreakpointInsert()
 *
 * inserts the global breakpoint (brkpnt) in the global breakpoints
 * hash table against the supplied key.
 */
bool
BreakpointInsert(eBreakpointScope scope, BreakpointKey *key, BreakpointData *data)
{
	Breakpoint	*entry;
	bool		 found;
	
	acquireLock(scope, LW_EXCLUSIVE);

	entry = (Breakpoint *) hash_search(getBreakpointHash(scope), (void *)key, HASH_ENTER, &found);

	if(found)
	{
		releaseLock(scope);
		return FALSE;
	}

	entry->data      = *data;
	entry->data.busy = FALSE;		/* Assume this breakpoint has not been nabbed by a target */

	
	/* register this insert in the count hash table*/
	breakCountInsert(scope, ((BreakCountKey *)key));

	releaseLock(scope);

	return( TRUE );
}

/* ---------------------------------------------------------
 * BreakpointInsertOrUpdate()
 *
 * inserts the global breakpoint (brkpnt) in the global breakpoints
 * hash table against the supplied key.
 */

bool
BreakpointInsertOrUpdate(eBreakpointScope scope, BreakpointKey *key, BreakpointData *data)
{
	Breakpoint	*entry;
	bool		 found;
	
	acquireLock(scope, LW_EXCLUSIVE);

	entry = (Breakpoint *) hash_search(getBreakpointHash(scope), (void *)key, HASH_ENTER, &found);

	if(found)
	{
		entry->data = *data;
		releaseLock(scope);
		return FALSE;
	}

	entry->data      = *data;
	entry->data.busy = FALSE;		/* Assume this breakpoint has not been nabbed by a target */

	
	/* register this insert in the count hash table*/
	breakCountInsert(scope, ((BreakCountKey *)key));
	
	releaseLock(scope);

	return( TRUE );
}

/* ---------------------------------------------------------
 * BreakpointBusySession()
 *
 * This function marks all breakpoints that belong to the given
 * proxy (identified by pid) as 'busy'. When a potential target
 * runs into a busy breakpoint, that means that the breakpoint
 * has already been hit by some other target and that other 
 * target is engaged in a conversation with the proxy (in other
 * words, the debugger proxy and debugger client are busy).
 *
 * We also copy all global breakpoints for the given proxy
 * to the local breakpoints list - that way, the target that's
 * actually interacting with the debugger client will continue
 * to hit those breakpoints until the target process ends.
 * 
 * When that debugging session ends, the debugger proxy calls
 * BreakpointFreeSession() to let other potential targets know
 * that the proxy can handle another target.
 *
 * FIXME: it might make more sense to simply move all of the
 *		  global breakpoints into the local hash instead, then
 *		  the debugger client would have to recreate all of
 *		  it's global breakpoints before waiting for another
 *		  target.
 */

void 
BreakpointBusySession(int pid)
{
	HASH_SEQ_STATUS status;
	Breakpoint	   *entry;

	acquireLock(BP_GLOBAL, LW_EXCLUSIVE);

	hash_seq_init(&status, getBreakpointHash(BP_GLOBAL));

	while((entry = (Breakpoint *) hash_seq_search(&status)))
	{
		if( entry->data.proxyPid == pid )
		{
			Breakpoint 	localCopy = *entry;

			entry->data.busy = TRUE;
			
			/* 
			 * Now copy the global breakpoint into the
			 * local breakpoint hash so that the target
			 * process will hit it again (other processes 
			 * will ignore it)
			 */

			localCopy.key.targetPid = MyProc->pid;

			BreakpointInsertOrUpdate(BP_LOCAL, &localCopy.key, &localCopy.data );
		}
	}

	releaseLock(BP_GLOBAL);
}

/* ---------------------------------------------------------
 * BreakpointFreeSession()
 *
 * This function marks all of the breakpoints that belong to 
 * the given proxy (identified by pid) as 'available'.  
 *
 * See the header comment for BreakpointBusySession() for
 * more information
 */

void
BreakpointFreeSession(int pid)
{
	HASH_SEQ_STATUS status;
	Breakpoint	   *entry;

	acquireLock(BP_GLOBAL, LW_EXCLUSIVE);

	hash_seq_init(&status, getBreakpointHash(BP_GLOBAL));

	while((entry = (Breakpoint *) hash_seq_search(&status)))
	{
		if( entry->data.proxyPid == pid )
			entry->data.busy = FALSE;
	}

	releaseLock(BP_GLOBAL);
}
/* ------------------------------------------------------------
 * BreakpointDelete()
 *
 * delete the given key from the global breakpoints hash table.
 */
bool 
BreakpointDelete(eBreakpointScope scope, BreakpointKey *key)
{
	Breakpoint	*entry;
	
	acquireLock(scope, LW_EXCLUSIVE);

	entry = (Breakpoint *) hash_search(getBreakpointHash(scope), (void *) key, HASH_REMOVE, NULL);

	if (entry)
 		breakCountDelete(scope, ((BreakCountKey *)key));
	
	releaseLock(scope);

	if(entry == NULL)
		return( FALSE );
	else
		return( TRUE );
}

/* ------------------------------------------------------------
 * BreakpointGetList()
 *
 *	This function returns an iterator (*scan) to the caller.
 *	The caller can use this iterator to scan through the 
 *	given hash table (either global or local).  The caller
 *  must call BreakpointReleaseList() when finished.
 */

void
BreakpointGetList(eBreakpointScope scope, HASH_SEQ_STATUS * scan)
{
	acquireLock(scope, LW_SHARED);
	hash_seq_init(scan, getBreakpointHash(scope));
}

/* ------------------------------------------------------------
 * BreakpointReleaseList()
 *
 *	This function releases the iterator lock returned by an 
 *	earlier call to BreakpointGetList().
 */

void 
BreakpointReleaseList(eBreakpointScope scope)
{
	releaseLock(scope);
}

/* ------------------------------------------------------------
 * BreakpointShowAll()
 *
 * sequentially traverse the Global breakpoints hash table and 
 * display all the break points via elog(INFO, ...)
 *
 * Note: The display format is still not decided.
 */

void
BreakpointShowAll(eBreakpointScope scope)
{
	HASH_SEQ_STATUS status;
	Breakpoint	   *entry;
	BreakCount     *count;
	
	acquireLock(scope, LW_SHARED);
	
	hash_seq_init(&status, getBreakpointHash(scope));

	elog(INFO, "BreakpointShowAll - %s", scope == BP_GLOBAL ? "global" : "local" );

	while((entry = (Breakpoint *) hash_seq_search(&status)))
	{
		elog(INFO, "Database(%d) function(%d) lineNumber(%d) targetPid(%d) proxyPort(%d) proxyPid(%d) busy(%c) tmp(%c)",
			 entry->key.databaseId,
			 entry->key.functionId,
			 entry->key.lineNumber,
			 entry->key.targetPid,
			 entry->data.proxyPort,
			 entry->data.proxyPid,
			 entry->data.busy ? 'T' : 'F',
			 entry->data.isTmp ? 'T' : 'F' );
	}

	elog(INFO, "BreakpointCounts" );

	hash_seq_init(&status, getBreakCountHash(scope));

	while((count = (BreakCount *) hash_seq_search(&status)))
	{
		elog(INFO, "Database(%d) function(%d) count(%d)",
			 count->key.databaseId,
			 count->key.functionId,
			 count->count );
	}
	releaseLock( scope );
}

/* ------------------------------------------------------------
 * BreakpointCleanupProc()
 *
 * sequentially traverse the Global breakpoints hash table and 
 * delete any breakpoints for the given process (identified by
 * its process ID).
 */

void BreakpointCleanupProc(int pid)
{
	HASH_SEQ_STATUS status;
	Breakpoint	   *entry;

	/*
	 * NOTE: we don't care about local breakpoints here, only
	 * global breakpoints
	 */

	acquireLock(BP_GLOBAL, LW_SHARED);
	
	hash_seq_init(&status, getBreakpointHash(BP_GLOBAL));

	while((entry = (Breakpoint *) hash_seq_search(&status)))
	{	
		if( entry->data.proxyPid == pid )
		{
			entry = (Breakpoint *) hash_search(getBreakpointHash(BP_GLOBAL), &entry->key, HASH_REMOVE, NULL);

			breakCountDelete(BP_GLOBAL, ((BreakCountKey *)&entry->key));
		}
	}

	releaseLock(BP_GLOBAL);
}

/* ==========================================================================
 * Function definitions for BreakCounts hash table
 *
 * Note: All the underneath functions assume that the caller has taken care
 * of all concurrency issues and thus does not do any locking
 * ==========================================================================
 */

static void
initLocalBreakCounts(void)
{
	HASHCTL ctl = {0};

	ctl.keysize   = sizeof(BreakCountKey);
	ctl.entrysize = sizeof(BreakCount);
	ctl.hash 	  = tag_hash;

	localBreakCounts = hash_create("Local Breakpoint Count Table",
								   32,
								  &ctl,
								  HASH_ELEM | HASH_FUNCTION );

	if (!globalBreakCounts)
		elog(FATAL, "could not initialize global breakpoints count hash table");
}

/* ---------------------------------------------------------
 * breakCountInsert()
 *
 * should be invoked when a breakpoint is added in Breakpoints
 */
void
breakCountInsert(eBreakpointScope scope, BreakCountKey *key)
{
	BreakCount *entry;
	bool		found;
	
	entry = hash_search(getBreakCountHash(scope), key, HASH_ENTER, &found);
	
	if (found)
		entry->count++;
	else
		entry->count = 1;
}

/* ---------------------------------------------------------
 * breakCountDelete()
 *
 * should be invoked when a breakpoint is removed from Breakpoints
 */
void
breakCountDelete(eBreakpointScope scope, BreakCountKey *key)
{
	BreakCount		*entry;
	
	entry = hash_search(getBreakCountHash(scope), key, HASH_FIND, NULL);
	
	if (entry)
	{
		entry->count--;
		
		/* remove entry only if entry->count is zero */
		if (entry->count == 0 )
			hash_search(getBreakCountHash(scope), key, HASH_REMOVE, NULL);
	}
		
}

/* ---------------------------------------------------------
 * breakCountLookup()
 *
 */
static int
breakCountLookup(eBreakpointScope scope, BreakCountKey *key, bool *found)
{
	BreakCount		*entry;
	
	entry = hash_search(getBreakCountHash(scope), key, HASH_FIND, found);
	
	if (entry)
		return entry->count;
		
	return -1;
}

/* ---------------------------------------------------------
 * getBreakpointHash()
 *
 *	Returns a pointer to the global or local breakpoint hash,
 *	depending on the given scope.
 */

static HTAB *
getBreakpointHash(eBreakpointScope scope )
{
	if( localBreakpoints == NULL )
		initializeHashTables();

	if (scope == BP_GLOBAL)
		return globalBreakpoints;
	else
		return localBreakpoints;
}

/* ---------------------------------------------------------
 * getBreakCountHash()
 *
 *	Returns a pointer to the global or local breakcount hash,
 *	depending on the given scope.
 */

static HTAB *
getBreakCountHash(eBreakpointScope scope)
{
	if( localBreakCounts == NULL )
		initializeHashTables();

	if (scope == BP_GLOBAL)
		return globalBreakCounts;
	else
		return localBreakCounts;
}
