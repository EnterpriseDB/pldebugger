/**********************************************************************
 * plugin_debugger.c	- Debugger for the PL/pgSQL procedural language
 *
 * Copyright (c) 2004-2007 EnterpriseDB Corporation. All Rights Reserved.
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

#include "nodes/pg_list.h"
#include "lib/dllist.h"
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

#if INCLUDE_PACKAGE_SUPPORT
#include "spl.h"
#include "catalog/edb_variable.h"
#else
#include "plpgsql.h"
#endif

/*
 * Let the PG module loader know that we are compiled against
 * the right version of the PG header files
 */

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))

#define PLDBG_HELP				'?'	
#define PLDBG_CONTINUE			'c'
#define PLDBG_SET_BREAKPOINT	   	'b'
#define PLDBG_CLEAR_BREAKPOINT    'f'
#define PLDBG_PRINT_VAR		    'p'
#define PLDBG_PRINT_STACK			'$'
#define PLDBG_LIST_BREAKPOINTS 	'l'
#define PLDBG_STEP_INTO			's'
#define PLDBG_STEP_OVER			'o'
#define PLDBG_LIST				'#'
#define PLDBG_INFO_VARS			'i'
#define PLDBG_SELECT_FRAME		'^'
#define PLDBG_DEPOSIT				'd'
#define PLDBG_RESTART				'r'
#define PLDBG_STOP				'x'

#define	TARGET_PROTO_VERSION	"1.0"

/**********************************************************************
 * Type and structure definitions
 **********************************************************************/

/*
 * We use a var_value structure to record  a little extra information about
 * each variable. 
 */

typedef struct
{
    bool	    isnull;			/* TRUE -> this variable IS NULL */
	bool		visible;		/* hidden or visible? see is_visible_datum() */
	bool		duplicate_name;	/* Is this one of many vars with same name? */
} var_value;

/*
 * When the debugger decides that it needs to step through (or into) a
 * particular function invocation, it allocates a dbg_ctx and records the
 * address of that structure in the executor's context structure
 * (estate->plugin_info).
 *
 * The dbg_ctx keeps track of all of the information we need to step through
 * code and display variable values
 */

typedef struct
{
    PLpgSQL_function *	func;		/* Function definition */
    bool				stepping;	/* If TRUE, stop at next statement */
    var_value	     *  symbols;	/* Extra debugger-private info about variables */
	char			 ** argNames;	/* Argument names */
	int					argNameCount; /* Number of names pointed to by argNames */
	void 			 (* error_callback)(void *arg);
	void 			 (* assign_expr)( PLpgSQL_execstate *estate, PLpgSQL_datum *target, PLpgSQL_expr *expr );
#if INCLUDE_PACKAGE_SUPPORT
	PLpgSQL_package   * package;
#endif
} dbg_ctx;

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

/*
 * errorHandlerCtx
 *
 *	We use setjmp() and longjmp() to handle network errors.  Because we want to 
 *  be able to stack setjmp()/longjmp() savepoints, we define a structure to 
 *  wrap sigjmp_buf's - we have to do that because sigjmp_buf is defined as an 
 *  array on some platforms (like Win32).
 */

typedef struct
{
	sigjmp_buf	m_savepoint;
} errorHandlerCtx;

/**********************************************************************
 * Local (static) variables
 **********************************************************************/

/* 
 * We keep one per_session_ctx structure per backend. This structure holds all
 * of the stuff that we need to track from one function call to the next.
 */

static struct
{
	bool	 step_into_next_func;	/* Should we step into the next function?				 */
	int		 client_r;				/* Read stream connected to client						 */
	int		 client_w;				/* Write stream connected to client						 */
	int		 client_port;			/* TCP Port that we are connected to 					 */
} per_session_ctx;

static errorHandlerCtx client_lost;

/**********************************************************************
 * Function declarations
 **********************************************************************/

void _PG_init( void );				/* initialize this module when we are dynamically loaded	*/
void _PG_fini( void );				/* shutdown this module when we are dynamically unloaded	*/

/**********************************************************************
 * Local (hidden) function prototypes
 **********************************************************************/
static void			dbg_send( int sock, const char *fmt, ... )
#ifdef PG_PRINTF_ATTRIBUTE
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)))
#endif
	;

static void 		 dbg_startup( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void 		 dbg_newstmt( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt );
static void 		 initialize_plugin_info( PLpgSQL_execstate * estate, PLpgSQL_function * func );

static char       ** fetchArgNames( PLpgSQL_function * func, int * nameCount );
static uint32 		 resolveHostName( const char * hostName );
static bool 		 getBool( int channel );
static char        * getNString( int channel );
static void 		 sendString( int channel, char * src );
static void        * writen( int peer, void * src, size_t len );
static char 	   * dbg_read_str( int channel );
static PLpgSQL_var * find_var_by_name( const PLpgSQL_execstate * estate, const char * var_name, int lineno, int * index );
static void 	     dbg_printvar( PLpgSQL_execstate * estate, const char * var_name, int lineno );
static bool 		 breakAtThisLine( Breakpoint ** dst, eBreakpointScope * scope, Oid funcOid, int lineNumber );
static bool 		 attach_to_proxy( Breakpoint * breakpoint );
static bool 		 connectAsServer( void );
static bool 		 connectAsClient( Breakpoint * breakpoint );
static bool 		 is_datum_visible( PLpgSQL_datum * datum );
static bool			 is_var_visible( PLpgSQL_execstate * frame, int var_no );
static bool			 datumIsNull(PLpgSQL_datum *datum);
static bool 		 handle_socket_error(void);
static bool 		 parseBreakpoint( Oid * funcOID, int * lineNumber, char * breakpointString );
static bool 		 addLocalBreakpoint( Oid funcOID, int lineNo );
static bool          varIsArgument( const PLpgSQL_execstate * frame, int varNo );

static void			 reserveBreakpoints( void );

/**********************************************************************
 * Function definitions
 **********************************************************************/
/*
 * ---------------------------------------------------------------------
 * _PG_init()
 *
 *	This function is invoked by the server when the PL debugger is 
 *	loaded.  It creates a rendezvous variable (PLpgSQL_plugin) that 
 *	the PL/pgSQL interpreter can use to find this plugin. By mutual
 *	agreement, the PL/pgSQL interpreter expects to find a PLpgSQL_plugin
 *	structure in that rendezvous variable.
 */

static PLpgSQL_plugin plugin_funcs = { dbg_startup, NULL, NULL, dbg_newstmt, NULL };

#if INCLUDE_PACKAGE_SUPPORT

static const char * plugin_name  = "spl_plugin";
#define OID_DEBUG_FUNCTION	edb_oid_debug

#else

static const char * plugin_name  = "PLpgSQL_plugin";
#define OID_DEBUG_FUNCTION	plpgsql_oid_debug

#endif

Datum OID_DEBUG_FUNCTION(PG_FUNCTION_ARGS);

void _PG_init( void )
{
	PLpgSQL_plugin ** var_ptr = (PLpgSQL_plugin **) find_rendezvous_variable( plugin_name );

	reserveBreakpoints();

	*var_ptr = &plugin_funcs;
}


/*
 * ---------------------------------------------------------------------
 * _PG_fini()
 *
 *	This function is invoked by the server when the PL debugger is
 *	unloaded.  It clears out the PLpgSQL_plugin rendezvous variable.
 */

void _PG_fini( void )
{
	PLpgSQL_plugin ** var_ptr = (PLpgSQL_plugin **) find_rendezvous_variable( plugin_name );

	*var_ptr = NULL;
}

/*
 * CREATE OR REPLACE FUNCTION xxx_oid_debug( functionOID OID ) RETURNS INTEGER AS 'plpgsql' LANGUAGE C;
 */

PG_FUNCTION_INFO_V1(OID_DEBUG_FUNCTION);

Datum OID_DEBUG_FUNCTION(PG_FUNCTION_ARGS)
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

static char *dbg_read_str( int sock )
{
	uint32 len;
	char *dst;

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

/*******************************************************************************
 * getBool()
 *
 *	getBool() retreives a boolean value (TRUE or FALSE) from the server.  We
 *  call this function after we ask the server to do something that returns a
 *  boolean result (like deleting a breakpoint or depositing a new value).
 */

static bool getBool( int channel )
{
	char * str;
	bool   result;

	str = getNString( channel );

	if( str[0] == 't' )
		result = TRUE;
	else
		result = FALSE;

	pfree( str );

	return( result );
}

/*******************************************************************************
 * sendString()
 *
 *	This function sends a string value (src) to the debugger server.  'src' 
 *	should point to a null-termianted string.  We send the length of the string 
 *	(as a 32-bit unsigned integer), then the bytes that make up the string - we
 *	don't send the null-terminator.
 */

static void sendString( int channel, char * src )
{
	size_t	len = strlen( src );

	sendUInt32( channel, len );
	writen( channel, src, len );
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

static char * getNString( int channel )
{
	uint32 len = readUInt32( channel );

	if( len == 0 )
		return( NULL );
	else
	{
		char * result = palloc( len + 1 );

		readn( channel, result, len );

		result[len] = '\0';

		return( result );
	}
}


/*******************************************************************************
 * resolveHostName()
 *
 *	Given the name of a host (hostName), this function returns the IP address
 *	of that host (or 0 if the name does not resolve to an address).
 *
 *	FIXME: this function should probably be a bit more flexibile.
 */

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long int) -1)    /* For Solaris */
#endif

static uint32 resolveHostName( const char * hostName )
{
    struct hostent * hostDesc;
    uint32           hostAddress;

    if(( hostDesc = gethostbyname( hostName )))
		hostAddress = ((struct in_addr *)hostDesc->h_addr )->s_addr;
    else
		hostAddress = inet_addr( hostName );

    if(( hostAddress == -1 ) || ( hostAddress == INADDR_NONE ))
		return( 0 );
	else
		return( hostAddress );
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

static void dbg_send( int sock, const char *fmt, ... )
{
	StringInfoData	result;
	char		   *data;
	size_t			remaining;
	
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
 * dbg_send_src()
 *
 *	dbg_send_src() sends the source code for a function to the client.
 *
 *  The client caches the source code that we send it and uses xmin/cmin
 *  to ensure the validity of the cache.
 */

static void dbg_send_src( PLpgSQL_execstate * frame, char * command  )
{
    HeapTuple			tup;
    char				*procSrc;
	Oid					targetOid = InvalidOid;  /* Initialize to keep compiler happy */
	TransactionId		xmin;
	CommandId			cmin;

	targetOid = atoi( command + 2 );

	/* Find the source code for this function */
	procSrc = findSource( targetOid, &tup );
										
	xmin = HeapTupleHeaderGetXmin( tup->t_data );

	if( TransactionIdIsCurrentTransactionId( xmin ))
		cmin = HeapTupleHeaderGetCmin( tup->t_data );
	else
		cmin = 0;

	/* FIXME: We need to send the xmin and cmin to the client too so he can know when to flush his cache */

	/* Found it - now send the source to the client */

	dbg_send( per_session_ctx.client_w, "%s", procSrc );

	/* Release the process tuple and send a footer to the client so he knows we're finished */

    ReleaseSysCache( tup );
}

/*
 * ---------------------------------------------------------------------
 * find_var_by_name()
 *
 *	This function returns the PLpgSQL_var pointer that corresponds to 
 *	named variable (var_name).  If the named variable can't be found,
 *  find_var_by_name() returns NULL.
 *
 *  If the index is non-NULL, this function will set *index to the 
 *  named variables index withing estate->datums[]
 */

static PLpgSQL_var * find_var_by_name( const PLpgSQL_execstate * estate, const char * var_name, int lineno, int * index )
{
    dbg_ctx          * dbg_info = (dbg_ctx *)estate->plugin_info;
    PLpgSQL_function * func     = dbg_info->func;
    int                i;

	for( i = 0; i < func->ndatums; i++ )
	{	
		PLpgSQL_var * var = (PLpgSQL_var *) estate->datums[i];      
		size_t 		  len = strlen(var->refname);	
		
		if(len != strlen(var_name)) 
			continue;
		
		if( strncmp( var->refname, var_name, len) == 0 )
		{
		 	if(( lineno == -1 ) || ( var->lineno == lineno ))
			{
				/* Found the named variable - return the index if the caller wants it */

				if( index )
					*index = i;
			}
			
			return( var );
		}
	}

	/* We can't find the variable named by the caller - return NULL */

    return( NULL );

}

static PLpgSQL_datum * find_datum_by_name( const PLpgSQL_execstate * frame, const char * var_name, int lineNo, int * index )
{
	dbg_ctx * dbg_info = (dbg_ctx *)frame->plugin_info;
	int		  i;
		
#if INCLUDE_PACKAGE_SUPPORT

	if( var_name[0] == '@' )
	{
		/* This is a package variable (it's name starts with a '@') */
		int		  varIndex;

		if( dbg_info == NULL )
			return( NULL );

		if( dbg_info->package == NULL )
			return( NULL );

		for( varIndex = 0; varIndex < dbg_info->package->ndatums; ++varIndex )
		{
			PLpgSQL_datum * datum = dbg_info->package->datums[varIndex];

			switch( datum->dtype )
			{
				case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var * var = (PLpgSQL_var *) datum;

					if( strcmp( var->refname, var_name+1 ) == 0 )
						return( datum );
					break;
				}
			}
		}

		return( NULL );
	}
#endif

	for( i = 0; i < frame->ndatums; ++i )
	{
		char	*	datumName = NULL;
		int			datumLineno = -1;

		switch( frame->datums[i]->dtype )
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_ROW:
			case PLPGSQL_DTYPE_REC:
			{
				PLpgSQL_variable * var = (PLpgSQL_variable *)frame->datums[i];

				datumName   = var->refname;
				datumLineno = var->lineno;

				if( varIsArgument( frame, i ))
				{
					if( dbg_info->argNames && dbg_info->argNames[i] && dbg_info->argNames[i][0] )
						datumName = dbg_info->argNames[i];
				}

				break;
			}

			case PLPGSQL_DTYPE_RECFIELD:
			case PLPGSQL_DTYPE_ARRAYELEM:
			case PLPGSQL_DTYPE_EXPR:
#if (PG_VERSION_NUM <= 80400)
			case PLPGSQL_DTYPE_TRIGARG:
#endif
			{
				break;
			}
		}

		if( datumName == NULL )
			continue;

		if( strcmp( var_name, datumName ) == 0 )
		{
			if( lineNo == -1 || lineNo == datumLineno )
			{	
				if( index )
					*index = i;

				return( frame->datums[i] );
			}
		}
	}

	return( NULL );
}

/*
 * ---------------------------------------------------------------------
 * dbg_printvar()
 *
 *	This function will print (that is, send to the debugger client) the 
 *  type and value of the given variable.
 */

static void print_var( const PLpgSQL_execstate * frame, const char * var_name, int lineno, const PLpgSQL_var * tgt )
{
    char	     	 * extval;
    HeapTuple	       typeTup;
    Form_pg_type       typeStruct;
    FmgrInfo	       finfo_output;
    dbg_ctx 		 * dbg_info = (dbg_ctx *)frame->plugin_info;

    if( tgt->isnull )
    {
		if( dbg_info->symbols[tgt->dno].duplicate_name )
			dbg_send( per_session_ctx.client_w, "v:%s(%d):NULL\n", var_name, lineno );
		else
			dbg_send( per_session_ctx.client_w, "v:%s:NULL\n", var_name );
		return;
    }

    /* Find the output function for this data type */

    typeTup = SearchSysCache( TYPEOID, ObjectIdGetDatum( tgt->datatype->typoid ), 0, 0, 0 );

    if( !HeapTupleIsValid( typeTup ))
    {
		dbg_send( per_session_ctx.client_w, "v:%s(%d):***can't find type\n", var_name, lineno );
		return;
    }
    
    typeStruct = (Form_pg_type)GETSTRUCT( typeTup );

	/* Now invoke the output function to convert the variable into a null-terminated string */

    fmgr_info( typeStruct->typoutput, &finfo_output );

    extval = DatumGetCString( FunctionCall3( &finfo_output, tgt->value, ObjectIdGetDatum(typeStruct->typelem), Int32GetDatum(-1)));

	/* Send the name:value to the debugger client */

	if( dbg_info->symbols[tgt->dno].duplicate_name )
		dbg_send( per_session_ctx.client_w, "v:%s(%d):%s\n", var_name, lineno, extval );
	else
		dbg_send( per_session_ctx.client_w, "v:%s:%s\n", var_name, extval );

    pfree( extval );
    ReleaseSysCache( typeTup );
}

static void print_row( const PLpgSQL_execstate * frame, const char * var_name, int lineno, const PLpgSQL_row * tgt )
{


}

static void print_rec( const PLpgSQL_execstate * frame, const char * var_name, int lineno, const PLpgSQL_rec * tgt )
{
	int		attNo;
	
	if (tgt->tupdesc == NULL)
		return;

	for( attNo = 0; attNo < tgt->tupdesc->natts; ++attNo )
	{
		char * extval = SPI_getvalue( tgt->tup, tgt->tupdesc, attNo + 1 );

		dbg_send( per_session_ctx.client_w, "v:%s.%s:%s\n", var_name, NameStr( tgt->tupdesc->attrs[attNo]->attname ), extval ? extval : "NULL" );

		if( extval )
			pfree( extval );
	}
}

static void print_recfield( const PLpgSQL_execstate * frame, const char * var_name, int lineno, const PLpgSQL_recfield * tgt )
{


}

static void dbg_printvar( PLpgSQL_execstate * estate, const char * var_name, int lineno )
{

	PLpgSQL_variable * generic = NULL;

	/* Try to find the given variable */

    if(( generic = (PLpgSQL_variable*) find_var_by_name( estate, var_name, lineno, NULL )) == NULL )
    {
		dbg_send( per_session_ctx.client_w, "v:%s(%d):Unknown variable (or not in scope)\n", var_name, lineno );
		return;
    }

	switch( generic->dtype )
	{
		case PLPGSQL_DTYPE_VAR:
			print_var( estate, var_name, lineno, (PLpgSQL_var *) generic );
			break;

		case PLPGSQL_DTYPE_ROW:
			print_row( estate, var_name, lineno, (PLpgSQL_row *) generic );
			break;

		case PLPGSQL_DTYPE_REC:
			print_rec( estate, var_name, lineno, (PLpgSQL_rec *) generic );
			break;

		case PLPGSQL_DTYPE_RECFIELD:
			print_recfield( estate, var_name, lineno, (PLpgSQL_recfield *) generic );
			break;

	}
}

/*
 * ---------------------------------------------------------------------
 * mark_duplicate_names()
 *
 *	In a PL/pgSQL function/procedure you can declare many variables with
 *  the same name as long as the name is unique within a scope.  The PL
 *	compiler co-mingles all variables into a single symbol table without
 *  indicating (at run-time) when a variable comes into scope.  
 *
 *  When we display a variable to the user, we want to show an undecorated
 *  name unless the given variable has duplicate declarations (in nested 
 *  scopes).  If we detect that a variable has duplicate declarations, we
 *	decorate the name with the line number at which each instance is 
 *  declared.  This function detects duplicate names and marks duplicates
 *  in our private symbol table.
 */

static void mark_duplicate_names( const PLpgSQL_execstate * estate, int var_no )
{
    dbg_ctx * dbg_info = (dbg_ctx *)estate->plugin_info;

	if( dbg_info->symbols[var_no].duplicate_name )
	{
		/* already detected as a duplicate name - just go home */
		return;
	}

	/*
	 * FIXME: Handle other dtypes here too - for now, we just assume
	 *		  that all other types have duplicate names
	 */

	if( estate->datums[var_no]->dtype != PLPGSQL_DTYPE_VAR )
	{
		dbg_info->symbols[var_no].duplicate_name = TRUE;
		return;
	}
	else
	{
		PLpgSQL_var * var	   = (PLpgSQL_var *)estate->datums[var_no];
		char        * var_name = var->refname;
		int			  i;

		for( i = 0; i < estate->ndatums; ++i )
		{
			if( i != var_no )
			{
				if( estate->datums[i]->dtype != PLPGSQL_DTYPE_VAR )
					continue;
				
				var = (PLpgSQL_var *)estate->datums[i];

				if( strcmp( var_name, var->refname ) == 0 )
				{
					dbg_info->symbols[var_no].duplicate_name  = TRUE;
					dbg_info->symbols[i].duplicate_name  = TRUE;
				}
			}
		}
	}
}

/*
 * ---------------------------------------------------------------------
 * completeFrame()
 *
 *	This function ensures that the given execution frame contains
 *	all of the information we need in order to debug it.  In particular,
 *	we create an array that extends the frame->datums[] array.  
 *	We need to know which variables should be visible to the 
 *	debugger client (we hide some of them by convention) and 
 *	we need to figure out which names are unique and which 
 *	are duplicates.
 */

static void completeFrame( PLpgSQL_execstate * frame )
{
    dbg_ctx 		 * dbg_info = (dbg_ctx *)frame->plugin_info;
    PLpgSQL_function * func     = dbg_info->func;
    int		           i;

	if( dbg_info->symbols == NULL )
	{
		dbg_info->symbols = (var_value *) palloc( sizeof( var_value ) * func->ndatums );

		for( i = 0; i < func->ndatums; ++i )
		{
			dbg_info->symbols[i].isnull = TRUE;

			/*
			 * Note: in SPL, we hide a few variables from the debugger since
			 *       they are internally generated (that is, not declared by 
			 *		 the user).  Decide whether this particular variable should
			 *       be visible to the debugger client.
			 */

			dbg_info->symbols[i].visible 		= is_datum_visible( frame->datums[i] );
			dbg_info->symbols[i].duplicate_name = FALSE;
		}

		for( i = 0; i < func->ndatums; ++i )
			mark_duplicate_names( frame, i );

		dbg_info->argNames = fetchArgNames( func, &dbg_info->argNameCount );
	}
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

static bool attach_to_proxy( Breakpoint * breakpoint )
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
	int	 				sockfd       = socket( AF_INET, SOCK_STREAM, 0 );
	struct sockaddr_in 	srv_addr     = {0};
	struct sockaddr_in  cli_addr     = {0};
	socklen_t			srv_addr_len = sizeof( srv_addr );
	socklen_t			cli_addr_len = sizeof(cli_addr);
	int	 				client_sock;
	int					reuse_addr_flag = 1;
#ifdef WIN32
	WORD                wVersionRequested;
	WSADATA             wsaData;
	int                 err;
	u_long              blockingMode = 0;
#endif

	/* Ask the TCP/IP stack for an unused port */
	srv_addr.sin_family      = AF_INET;
	srv_addr.sin_port        = htons( 0 );
	srv_addr.sin_addr.s_addr = htonl( INADDR_ANY );

#ifdef WIN32

	wVersionRequested = MAKEWORD( 2, 2 );
 
	err = WSAStartup( wVersionRequested, &wsaData );
	if ( err != 0 )
	{
		/* Tell the user that we could not find a usable 
		 * WinSock DLL.                                  
		 */
		return 0;
	}

	/* Confirm that the WinSock DLL supports 2.2.
	 * Note that if the DLL supports versions greater
	 * than 2.2 in addition to 2.2, it will still return
	 * 2.2 in wVersion since that is the version we
	 * requested.
	 */

	if ( LOBYTE( wsaData.wVersion ) != 2 ||HIBYTE( wsaData.wVersion ) != 2 )
	{
		/* Tell the user that we could not find a usable
		 * WinSock DLL.
		 */
		WSACleanup( );
		return 0;
	}
#endif

	setsockopt( sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_addr_flag, sizeof( reuse_addr_flag ));

	/* Bind a listener socket to that port */
	if( bind( sockfd, (struct sockaddr *)&srv_addr, sizeof( srv_addr )) < 0 )
	{
		elog( COMMERROR, "pl_debugger - can't bind server port, errno %d", errno );
		return( FALSE );
	}

	/* Get the port number selected by the TCP/IP stack */
	getsockname( sockfd, (struct sockaddr *)&srv_addr, &srv_addr_len );

	/* Get ready to wait for a client */
	listen( sockfd, 2 );
		
#ifdef WIN32
	ioctlsocket( sockfd, FIONBIO,  &blockingMode );
#endif

	/* Notify the client application that a debugger is waiting on this port. */
	elog( NOTICE, "PLDBGBREAK:%d", ntohs( srv_addr.sin_port ));

	while( TRUE )
	{
		uint32	proxyPID;
		PGPROC *proxyOff;
		PGPROC *proxyProc;
		char   *proxyProtoVersion;
			
		/* and wait for the debugger client to attach to us */
		if(( client_sock = accept( sockfd, (struct sockaddr *)&cli_addr, &cli_addr_len )) < 0 )
		{
			per_session_ctx.client_w = per_session_ctx.client_r = 0;
			per_session_ctx.client_port = 0;
			return( FALSE );
		}
		else
		{
#ifdef WIN32
			u_long blockingMode1 = 0;

			ioctlsocket( client_sock, FIONBIO,  &blockingMode1 );
#endif
			
			per_session_ctx.client_w = client_sock;
			per_session_ctx.client_r = client_sock;
			per_session_ctx.client_port = 0;
		}

		/* Now authenticate the proxy */
		proxyPID = readUInt32( client_sock );
		readn( client_sock, &proxyOff, sizeof( proxyOff ));
		proxyProc = BackendPidGetProc(proxyPID);
		
		if (proxyProc == NULL || proxyProc != proxyOff)
		{
			/* This doesn't look like a valid proxy - he didn't send us the right info */
			ereport(LOG, (ERRCODE_CONNECTION_FAILURE, 
						  errmsg( "invalid debugger connection credentials")));
			dbg_send( per_session_ctx.client_w, "%s", "f" );
#ifdef WIN32
			closesocket( client_sock );
#else
			close( client_sock );
#endif
			per_session_ctx.client_w = per_session_ctx.client_r = 0;
			per_session_ctx.client_port = 0;
			continue;
		}

			
		/* 
		 * This looks like a valid proxy, let's use this connection
		 *
		 * FIXME: we may want to ensure that proxyProc->roleId corresponds
		 *		  to a superuser too
		 */
		dbg_send( per_session_ctx.client_w, "%s", "t" );
		
		/*
		 * The proxy now sends it's protocol version and we
		 * reply with ours
		 */
		proxyProtoVersion = dbg_read_str( per_session_ctx.client_w );
		pfree(proxyProtoVersion);
		dbg_send( per_session_ctx.client_w, "%s", TARGET_PROTO_VERSION );
		
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
	struct 	sockaddr_in  proxyAddress = {0};
	char               * proxyProtoVersion;

	if(( proxySocket = socket( AF_INET, SOCK_STREAM, 0 )) < 0 )
	{
		ereport( COMMERROR, (errcode(ERRCODE_CONNECTION_FAILURE), errmsg( "debugger server can't create socket, errno %d", errno )));
		return( FALSE );
	}

	proxyAddress.sin_family 	  = AF_INET;
	proxyAddress.sin_addr.s_addr = resolveHostName( "127.0.0.1" );
	proxyAddress.sin_port        = htons( breakpoint->data.proxyPort );
	
	if( connect( proxySocket, (struct sockaddr *)&proxyAddress, sizeof( proxyAddress )) < 0 )
	{
		ereport( DEBUG1, (errcode(ERRCODE_CONNECTION_FAILURE), errmsg( "debugger could not connect to debug proxy" )));
		return( FALSE );
	}

	sendUInt32( proxySocket, MyProc->pid );
	writen( proxySocket, &MyProc, sizeof( MyProc ));

	if( !getBool( proxySocket ))
	{
		ereport( COMMERROR, (errcode(ERRCODE_CONNECTION_FAILURE), errmsg( "debugger proxy refused authentication" )));
	}

	/*
	 * Now exchange version information with the target - for now,
	 * we don't actually do anything with the version information,
	 * but as soon as we make a change to the protocol, we'll need
	 * to know the right patois.
	 */

	sendString( proxySocket, TARGET_PROTO_VERSION );

	proxyProtoVersion = getNString( proxySocket );
	
	pfree( proxyProtoVersion );

	per_session_ctx.client_w = proxySocket;
	per_session_ctx.client_r = proxySocket;
	per_session_ctx.client_port = breakpoint->data.proxyPort;

	BreakpointBusySession( breakpoint->data.proxyPid );

	return( TRUE );
}

/* ------------------------------------------------------------------
 * fetchArgNames()
 * 
 *   This function returns the name of each argument for the given 
 *   function or procedure. If the function/procedure does not have 
 *	 named arguments, this function returns NULL
 *
 *	 The argument names are returned as an array of string pointers
 */

static char ** fetchArgNames( PLpgSQL_function * func, int * nameCount )
{
	HeapTuple	tup;
	Datum		argnamesDatum;
	bool		isNull;
	Datum	   *elems;
	bool	   *nulls;
	char	  **result;
	int			i;

	if( func->fn_nargs == 0 )
		return( NULL );

	tup = SearchSysCache( PROCOID, ObjectIdGetDatum( func->fn_oid ), 0, 0, 0 );

	if( !HeapTupleIsValid( tup ))
		elog( ERROR, "edbspl: cache lookup for proc %u failed", func->fn_oid );

	argnamesDatum = SysCacheGetAttr( PROCOID, tup, Anum_pg_proc_proargnames, &isNull );

	if( isNull )
	{
		ReleaseSysCache( tup );
		return( NULL );
	}

	deconstruct_array( DatumGetArrayTypeP( argnamesDatum ), TEXTOID, -1, false, 'i', &elems, &nulls, nameCount );

	result = (char **) palloc( sizeof(char *) * (*nameCount));

	for( i = 0; i < (*nameCount); i++ )
		result[i] = DatumGetCString( DirectFunctionCall1( textout, elems[i] ));

	ReleaseSysCache( tup );

	return( result );
}

static char * get_text_val( PLpgSQL_var * var, char ** name, char ** type )
{
	HeapTuple	       typeTup;
    Form_pg_type       typeStruct;
    FmgrInfo	       finfo_output;
	char            *  text_value = NULL;

    /* Find the output function for this data type */
    typeTup = SearchSysCache( TYPEOID, ObjectIdGetDatum( var->datatype->typoid ), 0, 0, 0 );

    if( !HeapTupleIsValid( typeTup ))
		return( NULL );

    typeStruct = (Form_pg_type)GETSTRUCT( typeTup );

	/* Now invoke the output function to convert the variable into a null-terminated string */
    fmgr_info( typeStruct->typoutput, &finfo_output );

    text_value = DatumGetCString( FunctionCall3( &finfo_output, var->value, ObjectIdGetDatum(typeStruct->typelem), Int32GetDatum(-1)));

	ReleaseSysCache( typeTup );

	if( name )
		*name = var->refname;

	if( type )
		*type = var->datatype->typname;

	return( text_value );
}

/* ------------------------------------------------------------------
 * send_plpgsql_frame()
 * 
 *   This function sends information about a single stack frame
 *   to the debugger client.  This function is called by send_stack()
 *	 whenever send_stack() finds a PL/pgSQL call in the stack (remember,
 *	 the call stack may contain stack frames for functions written in 
 *	 other languages like PL/Tcl).
 */

static void send_plpgsql_frame( PLpgSQL_execstate * estate )
{

#if (PG_VERSION_NUM >= 80500)
	PLpgSQL_function  * func     = estate->func;
#else
	PLpgSQL_function  * func     = estate->err_func;
#endif
	PLpgSQL_stmt	  * stmt 	 = estate->err_stmt;
	int					argNameCount;
	char             ** argNames = fetchArgNames( func, &argNameCount );
	StringInfo		    result   = makeStringInfo();
	char              * delimiter = "";
	int				    arg;

	/*
	 * Send the name, function OID, and line number for this frame
	 */

	appendStringInfo( result, "%s:%d:%d:",
#if (PG_VERSION_NUM >= 90200)
					  func->fn_signature,
#else
					  func->fn_name,
#endif
					  func->fn_oid,
					  stmt->lineno );

	/*
	 * Now assemble a string that shows the argument names and value for this frame
	 */

	for( arg = 0; arg < func->fn_nargs; ++arg )
	{
		int					index   = func->fn_argvarnos[arg];
		PLpgSQL_datum		*argDatum = (PLpgSQL_datum *)estate->datums[index];
		char 				*value;

		/* value should be an empty string if argDatum is null*/
		if( datumIsNull( argDatum ))
			value = pstrdup( "" );
		else
			value = get_text_val((PLpgSQL_var*)argDatum, NULL, NULL );
		
		if( argNames && argNames[arg] && argNames[arg][0] )
			appendStringInfo( result,  "%s%s=%s", delimiter, argNames[arg], value );
		else
			appendStringInfo( result,  "%s$%d=%s", delimiter, arg+1, value );

		pfree( value );
			
		delimiter = ", ";
	}

	dbg_send( per_session_ctx.client_w, "%s", result->data );
}

/* ------------------------------------------------------------------
 * send_stack()
 * 
 *   This function sends the call stack to the debugger client.  For
 *	 each PL/pgSQL stack frame that we find, we send the function name,
 *	 argument names and values, and the current line number (within 
 *	 that particular invocation).
 */

static void send_stack( dbg_ctx * dbg_info )
{
	ErrorContextCallback * entry;

	for( entry = error_context_stack; entry; entry = entry->previous )
	{
		/*
		 * ignore frames for other PL languages
		 */

		if( entry->callback == dbg_info->error_callback )
		{
			send_plpgsql_frame((PLpgSQL_execstate *)( entry->arg ));
		}
	}

	dbg_send( per_session_ctx.client_w, "%s", "" );	/* empty string indicates end of list */
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

static void setBreakpoint( const PLpgSQL_execstate * frame, char * command )
{
	/* 
	 *  Format is 'b funcOID:lineNumber'
	 */
	int			  lineNo;
	Oid			  funcOID;

	if( parseBreakpoint( &funcOID, &lineNo, command + 2 ))
	{
		if( addLocalBreakpoint( funcOID, lineNo ))
			dbg_send( per_session_ctx.client_w, "%s", "t" );
		else
			dbg_send( per_session_ctx.client_w, "%s", "f" );
	}
	else
	{
		dbg_send( per_session_ctx.client_w, "%s", "f" );
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

static void clearBreakpoint( PLpgSQL_execstate * frame, char * command )
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
			dbg_send( per_session_ctx.client_w, "t" );
		else
			dbg_send( per_session_ctx.client_w, "f" );
	}
	else
	{
		dbg_send( per_session_ctx.client_w, "f" ); 
	}
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

static void send_breakpoints( PLpgSQL_execstate * frame )
{
	dbg_ctx 		* dbg_info = (dbg_ctx *)frame->plugin_info;
	Breakpoint      * breakpoint;
	Oid			 	  funcOid  = dbg_info->func->fn_oid;
	HASH_SEQ_STATUS	  scan;

	BreakpointGetList( BP_GLOBAL, &scan );

	while(( breakpoint = (Breakpoint *) hash_seq_search( &scan )) != NULL )
	{
		if(( breakpoint->key.targetPid == -1 ) || ( breakpoint->key.targetPid == MyProc->pid ))
			if( breakpoint->key.databaseId == MyProc->databaseId )
				if( breakpoint->key.functionId == funcOid )
					dbg_send( per_session_ctx.client_w, "%d:%d:%s", funcOid, breakpoint->key.lineNumber, "" );
	}

	BreakpointReleaseList( BP_GLOBAL );

	BreakpointGetList( BP_LOCAL, &scan );

	while(( breakpoint = (Breakpoint *) hash_seq_search( &scan )) != NULL )
	{
		if(( breakpoint->key.targetPid == -1 ) || ( breakpoint->key.targetPid == MyProc->pid ))
			if( breakpoint->key.databaseId == MyProc->databaseId )
				if( breakpoint->key.functionId == funcOid )
					dbg_send( per_session_ctx.client_w, "%d:%d:%s", funcOid, breakpoint->key.lineNumber, "" );
	}

	BreakpointReleaseList( BP_LOCAL );

	dbg_send( per_session_ctx.client_w, "%s", "" );	/* empty string indicates end of list */

}

/*
 * ---------------------------------------------------------------------
 * send_vars()
 *	
 *	This function sends a list variables (names, types, values...)
 *	to the proxy process.  We send information about the variables
 *	defined in the given frame (local variables) and parameter values.
 */

static bool varIsArgument( const PLpgSQL_execstate * frame, int varNo )
{
	dbg_ctx * dbg_info = (dbg_ctx *)frame->plugin_info;

	if( varNo < dbg_info->func->fn_nargs )
		return( TRUE );
	else
		return( FALSE );
}

static void send_vars( PLpgSQL_execstate * frame )
{
	int       i;
	dbg_ctx * dbg_info = (dbg_ctx *)frame->plugin_info;

	for( i = 0; i < frame->ndatums; i++ )
	{
		if( is_var_visible( frame, i ))
		{
			switch( frame->datums[i]->dtype )
			{
				case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var * var = (PLpgSQL_var *) frame->datums[i];
					char        * val;
					char		* name = var->refname;
					bool		  isArg = varIsArgument( frame, i );

					if( datumIsNull((PLpgSQL_datum *)var ))
						val = "NULL";
					else
						val = get_text_val( var, NULL, NULL );

					if( i < dbg_info->argNameCount )
					{
						if( dbg_info->argNames && dbg_info->argNames[i] && dbg_info->argNames[i][0] )
						{
							name  = dbg_info->argNames[i];
							isArg = TRUE;
						}
					}

					dbg_send( per_session_ctx.client_w, "%s:%c:%d:%c:%c:%c:%d:%s",
							  name, 
							  isArg ? 'A' : 'L',
							  var->lineno,  
							  dbg_info->symbols[i].duplicate_name ? 'f' : 't',
							  var->isconst ? 't':'f', 
							  var->notnull ? 't':'f', 
							  var->datatype ? var->datatype->typoid : InvalidOid,
							  val );
				  
					break;
				}
#if 0
			FIXME: implement other types

				case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec * rec = (PLpgSQL_rec *) frame->datums[i];
					int		      att;
					char        * typeName;

					if (rec->tupdesc != NULL)
					{
						for( att = 0; att < rec->tupdesc->natts; ++att )
						{
							typeName = SPI_gettype( rec->tupdesc, att + 1 );
	
							dbg_send( per_session_ctx.client_w, "o:%s.%s:%d:%d:%d:%d:%s\n",
									  rec->refname, NameStr( rec->tupdesc->attrs[att]->attname ), 
									  0, rec->lineno, 0, rec->tupdesc->attrs[att]->attnotnull, typeName ? typeName : "" );
	
							if( typeName )
								pfree( typeName );
						}
					}
					break;
				}
#endif
			}
		}
	}

#if INCLUDE_PACKAGE_SUPPORT
	/* If this frame represents a package function/procedure, send the package variables too */
	if( dbg_info->package != NULL )
	{
		PLpgSQL_package * package = dbg_info->package;
		int				  varIndex;

		for( varIndex = 0; varIndex < package->ndatums; ++varIndex )
		{
			PLpgSQL_datum * datum = package->datums[varIndex];

			switch( datum->dtype )
			{
				case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var * var = (PLpgSQL_var *) datum;
					char        * val;
					char		* name = var->refname;

					if( datumIsNull((PLpgSQL_datum *)var ))
						val = "NULL";
					else
						val = get_text_val( var, NULL, NULL );

					dbg_send( per_session_ctx.client_w, "%s:%c:%d:%c:%c:%c:%d:%s",
							  name, 
							  'P',				/* variable class - P means package var */
							  var->lineno,  
							  'f',				/* duplicate name?						*/
							  var->isconst ? 't':'f', 
							  var->notnull ? 't':'f', 
							  var->datatype ? var->datatype->typoid : InvalidOid,
							  val );
				  
					break;
				}
			}
		}
	}
#endif

	dbg_send( per_session_ctx.client_w, "%s", "" );	/* empty string indicates end of list */
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

static PLpgSQL_execstate * select_frame( dbg_ctx * dbg_info, PLpgSQL_execstate * frameZero, int frameNo )
{
	ErrorContextCallback * entry;

	for( entry = error_context_stack; entry; entry = entry->previous )
	{
		if( entry->callback == dbg_info->error_callback )
		{
			if( frameNo-- == 0 )
			{
				PLpgSQL_execstate *estate = entry->arg;
				if (estate->plugin_info == NULL)
#if (PG_VERSION_NUM >= 80500)
					initialize_plugin_info(estate, estate->func);
#else
					initialize_plugin_info(estate, estate->err_func);
#endif
				return( entry->arg );
			}
		}
	}

	return( frameZero );
}

/*
 * ---------------------------------------------------------------------
 * dbg_startup()
 *
 *	This function is called the PL/pgSQL executor each time a function is
 *	invoked.  plpgsql_dbg_startup() returns quickly if user has defined no break-
 *  points.  
 *
 *	If the user has defined a breakpoint in this function, we connect to 
 *  the debugger client, allocate a per-invocation debugger context structure
 *  (a dbg_ctx), and record the address of that context structure in the
 *  pl_exec execution state (estate->plugin_info).
 *
 */

static bool breakAtThisLine( Breakpoint ** dst, eBreakpointScope * scope, Oid funcOid, int lineNumber )
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

static bool breakpointsForFunction( Oid funcOid )
{
	if( BreakpointOnId( BP_LOCAL, funcOid ) || BreakpointOnId( BP_GLOBAL, funcOid ))
		return( TRUE );
	else
		return( FALSE );

}

static void dbg_startup( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	if( func == NULL )
	{
		/* 
		 * In general, this should never happen, but it seems to in the 
		 * case of package constructors
		 */
		estate->plugin_info = NULL;
		return;
	}

	if( !breakpointsForFunction( func->fn_oid ) && !per_session_ctx.step_into_next_func)
	{
		estate->plugin_info = NULL;
		return;
	}
	initialize_plugin_info(estate, func);
}

static void initialize_plugin_info( PLpgSQL_execstate * estate,
									PLpgSQL_function * func )
{
    dbg_ctx * dbg_info;

	/* Allocate a context structure and record the address in the estate */
    estate->plugin_info = dbg_info = (dbg_ctx *) palloc( sizeof( dbg_ctx ));

	/* 
	 * As soon as we hit the first statement, we'll allocate space for each
	 * local variable. For now, we set symbols to NULL so we know to report all
	 * variables the next time we stop...
	 */
	dbg_info->symbols  		 = NULL;
	dbg_info->stepping 		 = FALSE;
    dbg_info->func     		 = func;

	/*
	 * The PL interpreter filled in two member of our plugin_funcs
	 * structure for us - we compare error_callback to the callback
	 * in the error_context_stack to make sure that we only deal with
	 * PL/pgSQL (or SPL) stack frames (hokey, but it works).  We use
	 * assign_expr when we need to deposit a value in variable.
	 */
	dbg_info->error_callback = plugin_funcs.error_callback;
	dbg_info->assign_expr    = plugin_funcs.assign_expr;
}

/*
 * ---------------------------------------------------------------------
 * do_deposit()
 *
 *  This function handles the 'deposit' feature - that is, this function
 *  sets a given PL variable to a new value, supplied by the client.
 *
 *	do_deposit() is called when you type a new value into a variable in 
 *  the local-variables window.
 *
 *  NOTE: For the convenience of the user, we first assume that the 
 *		  provided value is an expression.  If it doesn't evaluate,
 *		  we convert the value into a literal by surrounding it with
 *		  single quotes.  That may be surprising if you happen to make
 *		  a typo, but it will "do the right thing" in most cases.
 */

static void do_deposit( PLpgSQL_execstate * frame, const char * command )
{
	dbg_ctx       *dbg_info   = frame->plugin_info;
	PLpgSQL_datum *target     = NULL;
	const char    *var_name   = command + 2;
	char      	  *value      = strchr( var_name, '=' ); /* FIXME: handle quoted identifiers here */
	char      	  *lineno     = strchr( var_name, '.' ); /* FIXME: handle quoted identifiers here */
	char      	  *select;
    bool		   retry;
	PLpgSQL_expr  *expr;
	MemoryContext  curContext = CurrentMemoryContext;
	ResourceOwner  curOwner   = CurrentResourceOwner;


	/* command = d:var.line=expr */

	if( value == NULL || lineno == NULL )
	{
		dbg_send( per_session_ctx.client_w, "%s", "f" );
		return;
	}
	
	*value = '\0';
	value++;	/* Move past the '=' sign */

	*lineno = '\0';
	lineno++; 	/* Move past the '.' */

    if(( target = find_datum_by_name( frame, var_name, ( strlen( lineno ) == 0 ? -1 : atoi( lineno )), NULL )) == NULL )
	{
		dbg_send( per_session_ctx.client_w, "%s", "f" );
		return;
	}

	/*
	 * Now build a SELECT statement that returns the requested value
	 *
	 * NOTE: we allocate 2 extra bytes for quoting just in case we
	 *       need to later (see the retry logic below)
	 */

	select = palloc( strlen( "SELECT " ) + strlen( value ) + 2 + 1 );

	sprintf( select, "SELECT %s", value );

    /*
	 * Note: we must create a dynamically allocated PLpgSQL_expr here - we 
	 *       can't create one on the stack because exec_assign_expr()
	 *       links this expression into a list (active_simple_exprs) and
	 *       this expression must survive until the end of the current 
	 *	     transaction so we don't free it out from under spl_plpgsql_xact_cb()
	 */

	expr = (PLpgSQL_expr *) palloc0( sizeof( *expr ));

	expr->dtype       	   = PLPGSQL_DTYPE_EXPR;
	expr->dno              = -1;
	expr->query            = select;
	expr->plan   	       = NULL;
#if (PG_VERSION_NUM <= 80400)
	expr->plan_argtypes    = NULL;
	expr->nparams          = 0;
#endif
	expr->expr_simple_expr = NULL;

	BeginInternalSubTransaction( NULL );

	MemoryContextSwitchTo( curContext );

	PG_TRY();
	{
		if( target )
			dbg_info->assign_expr( frame, target, expr );

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo( curContext );
		CurrentResourceOwner = curOwner;

		SPI_restore_connection();		

		dbg_send( per_session_ctx.client_w, "%s", "t" );

		retry = FALSE;	/* That worked, don't try again */

	}
	PG_CATCH();
	{	
		ErrorData * edata;

		MemoryContextSwitchTo( curContext );

		edata = CopyErrorData();

		FlushErrorState();

		/* Abort the inner transaction */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo( curContext );
		CurrentResourceOwner = curOwner;

		SPI_restore_connection();

		retry = TRUE;	/* That failed - try again as a literal */
	}
	PG_END_TRY();

    /*
	 * If the given value is not a valid expression, try converting
	 * the value into a literal by sinqle-quoting it.
	 */

	if( retry )
	{
		sprintf( select, "SELECT '%s'", value );

		expr->dtype       	  = PLPGSQL_DTYPE_EXPR;
		expr->dno              = -1;
		expr->query            = select;
		expr->plan   	      = NULL;
		expr->expr_simple_expr = NULL;
#if (PG_VERSION_NUM <= 80400)
		expr->plan_argtypes    = NULL;
		expr->nparams          = 0;
#endif


		BeginInternalSubTransaction( NULL );

		MemoryContextSwitchTo( curContext );

		PG_TRY();
		{
			if( target )
				dbg_info->assign_expr( frame, target, expr );

			/* Commit the inner transaction, return to outer xact context */
			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo( curContext );
			CurrentResourceOwner = curOwner;

			SPI_restore_connection();		

			dbg_send( per_session_ctx.client_w, "%s", "t" );
		}
		PG_CATCH();
		{	
			ErrorData * edata;

			MemoryContextSwitchTo( curContext );

			edata = CopyErrorData();

			FlushErrorState();

			/* Abort the inner transaction */
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo( curContext );
			CurrentResourceOwner = curOwner;

			SPI_restore_connection();

			dbg_send( per_session_ctx.client_w, "%s", "f" );
		}
		PG_END_TRY();
	}

	pfree( select );
}

/*
 * ---------------------------------------------------------------------
 * is_datum_visible()
 *
 *	This function determines whether the given datum is 'visible' to the 
 *  debugger client.  We want to hide a few undocumented/internally 
 *  generated variables from the user - this is the function that hides
 *  them.  We set a flag in the symbols entry for this datum
 *  to indicate whether this variable is hidden or visible - that way,
 *  only have to do the expensive stuff once per invocation.
 */

static bool is_datum_visible( PLpgSQL_datum * datum )
{
	static const char * hidden_variables[] = 
	{
		"found",
		"rowcount",
		"sqlcode",
		"sqlerrm",
		"_found",
		"_rowcount",
	};

	/*
	 * All of the hidden variables are scalars at the moment so 
	 * assume that anything else is visible regardless of name
	 */
	
	if( datum->dtype != PLPGSQL_DTYPE_VAR )
		return( TRUE );
	else
	{
		PLpgSQL_var * var = (PLpgSQL_var *)datum;
		int			  i;

		for( i = 0; i < sizeof( hidden_variables ) / sizeof( hidden_variables[0] ); ++i )
		{
			if( strcmp( var->refname, hidden_variables[i] ) == 0 )
			{
				/*
				 * We found this variable in our list of hidden names -
				 * this variable is *not* visible
				 */

				return( FALSE );
			}
		}

		/*
		 * The SPL pre-processor generates a few variable names for 
		 * DMBS.PUTLINE statements - we want to hide those variables too.
		 * The generated variables are of the form 'txtnnn...' where 
		 * 'nnn...' is a sequence of one or more digits.
		 */

		if( strncmp( var->refname, "txt", 3 ) == 0 )
		{
			int	   i;

			/*
			 * Starts with 'txt' - see if the rest of the string is composed
			 * entirely of digits
			 */

			for( i = 3; var->refname[i] != '\0'; ++i )
			{
				if( var->refname[i] < '0' || var->refname[i] > '9' )
					return( TRUE );
			}

			return( FALSE );
		}

		return( TRUE );
	}
}

/*
 * ---------------------------------------------------------------------
 * is_var_visible()
 *
 *	This function determines whether the given variable is 'visible' to the 
 *  debugger client. We hide some variables from the user (see the 
 *  is_datum_visible() function for more info).  This function is quick - 
 *  we do the slow work in is_datum_visible() and simply check the results
 *  here.
 */

static bool is_var_visible( PLpgSQL_execstate * frame, int var_no )
{
    dbg_ctx * dbg_info = (dbg_ctx *)frame->plugin_info;

	if (dbg_info->symbols == NULL)
		completeFrame(frame);

	return( dbg_info->symbols[var_no].visible );
}

/*
 * ---------------------------------------------------------------------
 * send_cur_line()
 *
 *	This function sends the current position to the debugger client. We
 *	send the function's OID, xmin, cmin, and the current line number 
 *  (we're telling the client which line of code we're about to execute).
 */

static void send_cur_line( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt )
{
    dbg_ctx 		 * dbg_info = (dbg_ctx *)estate->plugin_info;
	PLpgSQL_function * func     = dbg_info->func;

	dbg_send( per_session_ctx.client_w,
			  "%d:%d:%s",
			  func->fn_oid,
			  stmt->lineno+1,
#if (PG_VERSION_NUM >= 90200)
			  func->fn_signature
#else
			  func->fn_name
#endif
		);
}

/*
 * ---------------------------------------------------------------------
 * isFirstStmt()
 *
 *	Returns true if the given statement is the first statement in the 
 *  given function.
 */

static bool isFirstStmt( PLpgSQL_stmt * stmt, PLpgSQL_function * func )
{
	if( stmt == linitial( func->action->body ))
		return( TRUE );
	else
		return( FALSE );
}
/*
 * ---------------------------------------------------------------------
 * dbg_newstmt()
 *
 *	The PL/pgSQL executor calls plpgsql_dbg_newstmt() just before executing each
 *	statement.  
 *
 *	This function is the heart of the debugger.  If you're single-stepping,
 *	or you hit a breakpoint, plpgsql_dbg_newstmt() sends a message to the debugger
 *  client indicating the current line and then waits for a command from 
 *	the user.
 *
 *	NOTE: it is very important that this function should impose negligible 
 *	      overhead when a debugger client is *not* attached.  In other words
 *		  if you're running PL/pgSQL code without a debugger, you notice no
 *		  performance penalty.
 */

static void dbg_newstmt( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt )
{
	PLpgSQL_execstate * frame = estate;

    /*
	 * If there's no debugger attached, go home as quickly as possible.
	 */
	if( frame->plugin_info == NULL )
		return;
	else
	{
		dbg_ctx 		  * dbg_info = (dbg_ctx *)frame->plugin_info;
		Breakpoint		  * breakpoint = NULL;
		eBreakpointScope	breakpointScope = 0;

		/*
		 * The PL compiler marks certain statements as 'invisible' to the
		 * debugger. In particular, the compiler may generate statements
		 * that do not appear in the source code. Such a statement is
		 * marked with a line number of -1: if we're looking at an invisible
		 * statement, just return to the caller.
		 */

		if( stmt->lineno == -1 )
			return;

		/*
		 * Now set up an error handler context so we can intercept any
		 * networking errors (errors communicating with the proxy).
		 */

		if( sigsetjmp( client_lost.m_savepoint, 1 ) != 0 )
		{
			/*
			 *  The connection to the debugger client has slammed shut - 
			 *	just pretend like there's no debugger attached and return
			 *
			 *	NOTE: we no longer have a connection to the debugger proxy -
			 *		  that means that we cannot interact with the proxy, we
			 *		  can't wait for another command, nothing.  We let the
			 *		  executor continue execution - anything else will hang
			 *		  this backend, waiting for a debugger command that will
			 *		  never arrive.
			 *
			 *		  If, however, we hit a breakpoint again, we'll stop and
			 *		  wait for another debugger proxy to connect to us.  If
			 *		  that's not the behavior you're looking for, you can 
			 *		  drop the breakpoint, or call free_function_breakpoints()
			 *		  here to get rid of all breakpoints in this backend.
			 */
			per_session_ctx.client_w = 0; 		/* No client connection */
			dbg_info->stepping 		 = FALSE; 	/* No longer stepping   */
		}

		if(( dbg_info->stepping ) || breakAtThisLine( &breakpoint, &breakpointScope, dbg_info->func->fn_oid, isFirstStmt( stmt, dbg_info->func ) ? -1 : stmt->lineno ))
			dbg_info->stepping = TRUE;
		else
			return;

		per_session_ctx.step_into_next_func = FALSE;

		/* We found a breakpoint for this function (or we're stepping into) */
		/* Make contact with the debugger client */

		if( !attach_to_proxy( breakpoint ))
		{
			/* 
			 * Can't attach to the proxy, maybe we found a stale breakpoint?
			 * That can happen if you set a global breakpoint on a function,
			 * invoke that function from a client application, debug the target
			 * kill the debugger client, and then re-invoke the function from
			 * the same client application - we will find the stale global
			 * breakpoint on the second invocation.
			 *
			 * We want to remove that breakpoint so that we don't keep trying
			 * to attach to a phantom proxy process (that's rather expensive 
			 * on Win32 or when the proxy is on another host).
			 */
			if( breakpoint )
				BreakpointDelete( breakpointScope, &(breakpoint->key));

			/*
			 * In any case, if we don't have a proxy to work with, we can't 
			 * do any debugging so give up.
			 */
			pfree( frame->plugin_info );
			frame->plugin_info       = NULL; /* No debugger context  */
			per_session_ctx.client_w = 0; /* No client connection */

			return;
		}

		if( stmt->cmd_type == PLPGSQL_STMT_BLOCK )
			return;

		/*
		 * The PL/pgSQL compiler inserts an automatic RETURN stat1ement at the
		 * end of each function (unless the last statement in the function is
		 * already a RETURN). If we run into that statement, we don't really
		 * want to wait for the user to STEP across it. Remember, the user won't
		 * see the RETURN statement in the source-code listing for his function.
		 *
		 * Fortunately, the automatic RETURN statement has a line-number of 0 
		 * so it's easy to spot.
		 */
		if( stmt->lineno == 0 )
			return;

		/*
		 * If we're in step mode, tell the debugger client, read a command from the client and 
		 * execute the command
		 */

		if( dbg_info->stepping )
		{
			bool	need_more     = TRUE;
			char   *command;

			/*
			 * Make sure that we have all of the debug info that we need in this stack frame
			 */
			completeFrame( frame );

			/* 
			 * We're in single-step mode (or at a breakpoint) 
			 * send the current line number to the debugger client and report any
			 * variable modifications
			 */

			send_cur_line( frame, stmt );			/* Report the current location 				 */

			/* 
			 *Loop through the following chunk of code until we get a command
			 * from the user that would let us execute this PL/pgSQL statement.
			 */
			while( need_more )
			{
				/* Wait for a command from the debugger client */
				command = dbg_read_str( per_session_ctx.client_r );

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
						dbg_info->stepping = 0;
						need_more = FALSE;
						break;
					}

					case PLDBG_SET_BREAKPOINT:
					{
						setBreakpoint( frame, command );
						break;
					}
				
					case PLDBG_CLEAR_BREAKPOINT:
					{
						clearBreakpoint( frame, command );
						break;
					}

					case PLDBG_PRINT_VAR:
					{
						/*
						 * Print value of given variable 
						 */

						dbg_printvar( frame, &command[2], -1 );
						break;
					}

					case PLDBG_LIST_BREAKPOINTS:
					{
						send_breakpoints( frame );
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
						dbg_send_src( frame, command );
						break;
					}

					case PLDBG_PRINT_STACK:
					{
						send_stack( dbg_info );
						break;
					}

					case PLDBG_SELECT_FRAME:
					{
						frame = select_frame( dbg_info, estate, atoi( &command[2] ));
						send_cur_line( frame, frame->err_stmt );	/* Report the current location 				 */
						break;

					}

					case PLDBG_DEPOSIT:
					{
						/* 
						 * Deposit a new value into the given variable
						 */
						do_deposit( frame, command );
						break;
					}

					case PLDBG_INFO_VARS:
					{
						/*
						 * Send list of variables (and their values)
						 */
						send_vars( frame );
						break;
					}
					
					case PLDBG_RESTART:
					case PLDBG_STOP:
					{
						/* stop the debugging session */
						dbg_send( per_session_ctx.client_w, "%s", "t" );

						ereport(ERROR,
								(errcode(ERRCODE_QUERY_CANCELED),
								 errmsg("canceling statement due to user request")));
						break;
					}
					
					default:
						elog(WARNING, "Unrecognized message %c", command[0]);
				}
				pfree(command);
			}
		}
	}
	
	return;
}

/* ---------------------------------------------------------------------
 *	datumIsNull()
 *	
 *	determine whether datum is NULL or not.
 *	TODO: consider datatypes other than PLPGSQL_DTYPE_VAR as well
 */
static bool datumIsNull(PLpgSQL_datum *datum)
{
	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
		{
			PLpgSQL_var *var = (PLpgSQL_var *) datum;
			
			if (var->isnull)
				return true;
		}
		break;
	
		/* other data types are not currently handled, we just return true */	
		case PLPGSQL_DTYPE_REC:
		case PLPGSQL_DTYPE_ROW:
			return true;
			
		default:
			return true;
	}
	
	return false;
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
#if INCLUDE_PACKAGE_SUPPORT
	Oid		   	packageId;		/* Not used, but included to match BreakpointKey so casts work as expected */
#endif
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
static void initGlobalBreakpoints(int size);
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

	initGlobalBreakpoints(globalBreakpointCount);

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

static void
initGlobalBreakpoints(int tableEntries)
{
	bool   	  		found;
	LWLockId	   *lockId;

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
