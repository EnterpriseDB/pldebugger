/*-------------------------------------------------------------------------
 *
 * plugin_helpers.c
 *
 *	  Helper functions for PL/pgSQL instrumentation plugins
 *
 *	Copyright 2006,2007 - EnterpriseDB, Inc.  
 *
 * IDENTIFICATION
 *	  $EnterpriseDB: edb-postgres/contrib/debugger/plugin_helpers.c,v 1.0 2005/12/15 02:49:32 kad Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include "postgres.h"
#include "utils/palloc.h"
#include "plpgsql.h"
#include "plugin_helpers.h"

/**********************************************************************
 * Static function prototypes
 **********************************************************************/

static char * findGlobalSource( Oid oid, HeapTuple * tup, char ** funcName );

/**********************************************************************
 * Exported Function definitions
 **********************************************************************/

/* -------------------------------------------------------------------
 * copyLine()
 *
 *	This function creates a null-terminated copy of the given string
 *	(presumably a line of source code).
 * -------------------------------------------------------------------
 */

char * copyLine( const char * src, size_t len )
{
	char * result = palloc(len+1);

	memcpy( result, src, len );
	result[len] = '\0';

	return( result );
}

/* -------------------------------------------------------------------
 * scanSource()
 *
 *	This function scans through the source code for a given function
 *	and counts the number of lines of code present in the string.  If
 *	the caller provides an array of char pointers (dst != NULL), we 
 *  copy each line of code into that array.
 *
 *  You would typically call this function twice:  the first time, you
 *  pass dst = NULL and scanSource() returns the number of lines of 
 *  code found in the string.  Once you know how many lines are present,
 *  you can allocate an array of char pointers and call scanSource()
 *  again - this time around, scanSource() will (non-destructively) split
 *  the source code into that array of char pointers.
 * -------------------------------------------------------------------
 */

int scanSource( const char * dst[], const char * src )
{
	int			 lineCount = 0;
	const char * nl;
	
	while(( nl = strchr( src, '\n' )) != NULL )
	{
		/* src points to start of line, nl points to end of line */

		if( dst )
			dst[lineCount] = copyLine( src, nl - src );

		lineCount++;

		src = nl + 1;
	}

	return( lineCount );
}

/* -------------------------------------------------------------------
 * xmlEncode()
 *
 *  Given a string (str & len) that may contain reserved characters, 
 *  xmlEncode() will translate those characters into legal XML entities
 *  and write the results to dst.
 * -------------------------------------------------------------------
 */

void xmlEncode( FILE * dst, const char * str, size_t len )
{
	int		i;

	for( i = 0; i < len; ++i )
	{
		/* 
		 *Translate any reserved characters into their 
		 * corresponding entity encodings
		 */

		switch( str[i] )
		{
			case '<':	 fprintf( dst, "&lt;" ); break;
			case '>':	 fprintf( dst, "&gt;" ); break;
			case '"':	 fprintf( dst, "&quot;" ); break;
			case '\'':	 fprintf( dst, "&apos;" ); break;
			case '&':	 fprintf( dst, "&amp;" ); break;
			case '\x09': fprintf( dst, "&#x9;" ); break;
			case '\x0A': fprintf( dst, "&#xA;" ); break;
			case '\x0D': fprintf( dst, "&#xD;" ); break;

			default: fputc( str[i], dst ); break;
      
		}
	}
}

/* -------------------------------------------------------------------
 * findSource()
 *
 *	This function locates and returns a pointer to a null-terminated string
 *	that contains the source code for the given function (identified by its
 *	OID and possibly package OID).
 *
 *	In addition to returning a pointer to the requested source code, this
 *	function sets *tup to point to a HeapTuple (that you must release when 
 *	you are finished with it) and sets *funcName to point to the name of 
 *	the given function.
 * -------------------------------------------------------------------
 */

char * findSource( Oid oid, Oid pkgId, HeapTuple * tup, char ** funcName )
{
	return( findGlobalSource( oid, tup, funcName ));
}

/* -------------------------------------------------------------------
 * funcGetOid()
 *
 *  Given a pointer to a PLpgSQL_function structure, funcGetOid() 
 *  returns the OID of the pg_proc tuple that defines that function.
 * -------------------------------------------------------------------
 */

Oid funcGetOid( PLpgSQL_function * func )
{
	return( func->fn_oid );
}

/* -------------------------------------------------------------------
 * funcGetPkgOid()
 *
 *  Given a pointer to a PLpgSQL_function structure, funcGetPkgOid() 
 *  returns the OID of the edb_package tuple that defines the function.
 * -------------------------------------------------------------------
 */

Oid funcGetPkgOid( PLpgSQL_function * func )
{
	return( InvalidOid );
}

/**********************************************************************
 * Static function definitions
 **********************************************************************/

/* ---------------------------------------------------------------------
 * findGlobalSource()
 *
 *	This function locates and returns a pointer to a null-terminated string
 *	that contains the source code for a function or procedure that is *not*
 *  defined within a package (thus the name 'global').  Calls findGlobalSource()
 *  with the OID of the function/procedure and it returns a pointer to the 
 *  source code for that function/procedure.
 *
 *	This function is called by findSource().
 *
 *	In addition to returning a pointer to the requested source code, this
 *	function sets *tup to point to a HeapTuple (that you must release when 
 *	you are finished with it) and sets *funcName to point to the name of 
 *	the given function.
 * -------------------------------------------------------------------
 */

static char * findGlobalSource( Oid oid, HeapTuple * tup, char ** funcName )
{
	bool	isNull;

	*tup = SearchSysCache( PROCOID, ObjectIdGetDatum( oid ), 0, 0, 0 );

	if(!HeapTupleIsValid( *tup ))
		elog( ERROR, "edbspl: cache lookup for proc %u failed", oid );

	*funcName = NameStr(((Form_pg_proc) GETSTRUCT( *tup ))->proname );

	return( DatumGetCString( DirectFunctionCall1( textout, SysCacheGetAttr( PROCOID, *tup, Anum_pg_proc_prosrc, &isNull ))));

}
