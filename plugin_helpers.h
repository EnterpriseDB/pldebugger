/*-------------------------------------------------------------------------
 *
 * plugin_helpers.h
 *
 *	  Helper function prototypes for PL/pgSQL instrumentation plugins
 *
 *	Copyright 2006,2007 - EnterpriseDB, Inc.  
 *
 * IDENTIFICATION
 *	  $EnterpriseDB: edb-postgres/contrib/debugger/plugin_helpers.h,v 1.0 2005/12/15 02:49:32 kad Exp $
 *
 *-------------------------------------------------------------------------
 */

static char  * copyLine( const char * src, size_t len );							/* Create a null-terminated copy of the given string 	*/
static int     scanSource( const char * dst[], const char * src );					/* Count (and optionally split) lines in given string	*/
static void    xmlEncode( FILE * dst, const char * str, size_t len );				/* Translate reserved characters and write XML string	*/
static char  * findSource( Oid oid, Oid pkgId, HeapTuple * tup, char ** funcName );	/* Find the source code for a function					*/
static Oid 	   funcGetOid( PLpgSQL_function * func );								/* Given a function descriptor, return the pg_proc OID	*/
static Oid 	   funcGetPkgOid( PLpgSQL_function * func );							/* Given a function descriptor, return the package OID	*/
