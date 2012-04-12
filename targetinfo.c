/*
 * targetinfo.c -
 *
 *  This file defines a single exported function, pldbg_get_target_info(),
 *  which returns whole boatload of information about a function or trigger.
 *  Specifically, pldbg_get_target_info() returns a tuple of type targetinfo
 *  (see pldbgapi.sql).  
 *
 *  You call pldbg_get_target_info() with the OID of the pg_proc tuple of the
 *  desired function/trigger. We used to support lookups using
 *  function/procedure name or trigger oid, but those modes have been removed
 *  as no-one was using them. 'o' is now the only valid 2nd argument.
 *
 * Copyright (c) 2004-2007 EnterpriseDB Corporation. All Rights Reserved.
 *
 * Licensed under the Artistic License, see 
 *		http://www.opensource.org/licenses/artistic-license.php
 * for full details
 */

#include "postgres.h"
#include "funcapi.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/array.h"							/* For DatumGetArrayTypePCopy()	*/
#include  "utils/catcache.h"
#include  "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_namespace.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "access/heapam.h"							/* For heap_form_tuple()		*/
#include "access/genam.h"
#include "utils/tqual.h"

/*
 * Let the PG module loader know that we are compiled against
 * the right version of the PG header files
 */

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/*******************************************************************************
 * Structure targetInfo
 *
 *	When you call the pldbg_get_target_info() function, we build a structure of
 *  type targetInfo and pass that structure to a bunch of helper functions.  We
 *  do that to keep the argument list manageable.
 */

typedef struct
{
	/* Input values */
	char  * rawName;				/* Raw target name given by the user 								*/

	/* Results */
	int		 nargs;				/* Number of arguments expected by the target function/procedure		*/
	Oid	     argTypes[FUNC_MAX_ARGS]; /* Argument types expected by target								*/
	Oid		 targetOid;			/* OID of the target (may point to a pg_proc or edb_pkgelements tuple)	*/
	Oid		 schemaOid;			/* OID of the schema (pg_namespace tuple) in which target is defined	*/
	char   * targetName;		/* Target name															*/
	Datum    argModes;			/* Argument modes (in/out/both)											*/
	Datum 	 argNames;			/* Argument names														*/
	Oid	 	 langOid;			/* OID of pg_language in which target is written (edb-spl or pl/pgsql)	*/
	char   * fqName;			/* Fully-qualified name (schema.package.target or schema.target)		*/
	bool	 returnsSet;		/* TRUE -> target returns SETOF values									*/
	Oid		 returnType;		/* OID of pg_type which corresponds to target return type				*/
} targetInfo;

static bool 		  	 getTargetFromOid( targetInfo * info );
static Form_pg_namespace getSchemaForm( Oid schemaOid, HeapTuple * tuple );
static Form_pg_proc      getProcFormByOid( Oid procOid, HeapTuple * tuple );
static void 		     completeProcTarget( targetInfo * info, HeapTuple proctup );
static char 		   * makeFullName( Oid schemaOID, char * targetName );
static TupleDesc	  	 getResultTupleDesc( FunctionCallInfo fcinfo );

PG_FUNCTION_INFO_V1( pldbg_get_target_info );		/* Get target name, argtypes, pkgOid, ...		*/

Datum pldbg_get_target_info( PG_FUNCTION_ARGS );

#define GET_STR( textp ) 		DatumGetCString( DirectFunctionCall1( textout, PointerGetDatum( textp )))


/********************************************************************************
 *  pldbg_get_target_info( sigNameOrOid TEXT, isFunc bool ) RETURNS targetInfo
 *
 *	This function retrieves a whole truckload of information about a function or procedure.
 *
 *	You can call this function to find, among other things, the OID of the function/procedure
 *
 *	This function expects two argument: sigNameOrOid, and isFunc.
 *
 *	The first argument may be any of the following:
 *		An OID	('33223')
 *
 *		A function or procedure name
 *		A function or procedure signature (name plus argument types)
 *
 *	If you specify a name and you don't mention a specific schema, we search
 *	the schema search path to find a match.
 *
 *	If the name you specify turns out to be ambiguous, we throw an error.  Ambiguity 
 *	is trickier than you might think.  If you specify a schema-qualified name (but not a 
 *	signature) schema (like 'schema.function'), it's only ambiguous if 'function' is 
 *	overloaded within that schema.  However, if you only specify a name (and not a
 *	specific schema), we consider it ambiguous if there are two or more functions with
 *	that name somewhere in the search path.  
 *
 *	Our goal is to find the same function/procedure that the parser/executor would 
 *	find, but we'll let you omit the complete signature as long as the result is not
 *	ambiguous.
 *
 *	If this function does not throw an error, it returns a tuple that contains:
 *		the target OID
 *		the schema OID
 *		the argument types
 *		the targetName (not schema-qualified)
 *		the argument modes (in, out, or both)
 *		the argument names
 *		the OID of the pg_language row for the target (typically edb-spl or plpgsql)
 *		the fully-qualified name of the target
 *		a boolean indicating whether the target is a function(true) or a procedure(false)
 */

Datum pldbg_get_target_info( PG_FUNCTION_ARGS )
{
	targetInfo	info 	   = {0};
	TupleDesc	tupleDesc  = getResultTupleDesc( fcinfo );
	Datum		values[11] = {0};
	bool		nulls[11]  = {0};
	HeapTuple	result;
	char		targetType;

	/*
	 * Since we have to search through a lot of different tables to satisfy all of the 
	 * different search types (schema qualified, schema unqualified, global, ...) 
	 * we create a search-context structure (a structure of type targetInfo) and just pass
	 * that around instead of managing a huge argument list.
	 */
	info.rawName    = GET_STR( PG_GETARG_TEXT_P( 0 ));

	/* We only support 'o' as target type, meaning look up by oid */
	targetType = PG_GETARG_CHAR( 1 );
	if (targetType != 'c')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid target type"),
				 errhint("Only valid target type is 'c'")));

	/*
	 * Let getTargetFromOid() fill in the information in the struct.
	 */
	info.targetOid = atol( info.rawName );
	if( !getTargetFromOid( &info ))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("function %s does not exist", info.rawName )));

	/*
	 * Now create the return tuple
	 */
	values[0]  = ObjectIdGetDatum( info.targetOid );				
	values[1]  = ObjectIdGetDatum( info.schemaOid );
	values[2]  = Int32GetDatum((int32)info.nargs );
	values[3]  = PointerGetDatum( buildoidvector( info.argTypes, info.nargs ));
	values[4]  = DirectFunctionCall1( namein, PointerGetDatum( info.targetName ));
	values[5]  = info.argModes;	
	values[6]  = info.argNames;
	values[7]  = ObjectIdGetDatum( info.langOid );
	values[8]  = DirectFunctionCall1( textin, PointerGetDatum( info.fqName ));
	values[9]  = BoolGetDatum( info.returnsSet );
	values[10] = ObjectIdGetDatum( info.returnType );

	nulls[0]  = FALSE;							/* targetOID 	*/
	nulls[1]  = FALSE;							/* schemaOID 	*/
	nulls[2]  = FALSE;							/* nargs		*/
	nulls[3]  = FALSE;							/* argTypes		*/
	nulls[4]  = FALSE;							/* targetName	*/
	nulls[5]  = info.argModes ? FALSE : TRUE;	/* argModes	*/
	nulls[6]  = info.argNames ? FALSE : TRUE;	/* argNames 	*/
	nulls[7]  = FALSE;							/* targetLang	*/
	nulls[8]  = FALSE;							/* fqName		*/
	nulls[9]  = FALSE;							/* returnsSet	*/
	nulls[10] = FALSE;							/* returnType 	*/

	result = heap_form_tuple( tupleDesc, values, nulls );

	PG_RETURN_DATUM( HeapTupleGetDatum( result ));	   
}

/*******************************************************************************
 * getProcFormByOid()
 *
 *	This helper function, given the OID of a row in the pg_proc table, 
 *	returns a pointer to that row (or NULL if the row does not exist).
 *
 *	In addition to returning a pointer to the requested row, we return a 
 *	tuple handle and the caller must ReleaseSysCache( *tuple ) when it's 
 *	finished with it.
 */

static Form_pg_proc getProcFormByOid( Oid procOid, HeapTuple * tuple )
{
	*tuple = SearchSysCache( PROCOID, ObjectIdGetDatum( procOid ), 0, 0, 0 );

	if( HeapTupleIsValid( *tuple ))
		return((Form_pg_proc) GETSTRUCT( *tuple ));
	else
		return( NULL );
}

/*******************************************************************************
 * getSchemaForm()
 *
 *	This helper function, given the OID of a row in the pg_namespace table, 
 *	returns a pointer to that row (or NULL if the row does not exist).
 *
 *	In addition to returning a pointer to the requested row, we return a 
 *	tuple handle and the caller must ReleaseSysCache( *tuple ) when it's 
 *	finished with it.
 */

static Form_pg_namespace getSchemaForm( Oid schemaOid, HeapTuple * tuple )
{
	*tuple = SearchSysCache( NAMESPACEOID, ObjectIdGetDatum( schemaOid ), 0, 0, 0 );

	if( HeapTupleIsValid( *tuple ))
		return((Form_pg_namespace) GETSTRUCT( *tuple ));
	else
		return( NULL );
}

/*******************************************************************************
 * makeFullName()
 *
 *	This helper function will construct a fully-qualified name for the given
 *	target. The fully-qualified name will always include the name of the schema
 *  in which the target is defined.
 */

static char * makeFullName( Oid schemaOID, char * targetName )
{
	HeapTuple			schemaTuple;
	Form_pg_namespace   schemaForm    = getSchemaForm( schemaOID, &schemaTuple );
	StringInfoData		fullName;

	initStringInfo(&fullName);

	if( HeapTupleIsValid( schemaTuple ))
	{
		appendStringInfo(&fullName, "%s.",
						 quote_identifier( NameStr( schemaForm->nspname )));
		ReleaseSysCache( schemaTuple );
	}

	/* 
	 * Finally the target name  - we end up with:
	 *	schema.target
	 */
	appendStringInfoString(&fullName, quote_identifier( targetName ));

	return fullName.data;
}

/*******************************************************************************
 * completeProcTarget()
 *
 *	Given a pg_proc tuple, this function copies the target definition into
 *	the targetInfo structure.
 */

static void completeProcTarget( targetInfo * info, HeapTuple proctup )
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT( proctup );
	bool		 modesNull;
	bool		 namesNull;
	bool		 allTypesNull;
	Datum		 argModes;
	Datum		 argNames;
	Datum		 argTypes;

	/* 
	 * Grab argModes, argNames, and isFunc from the tuple
	 *
	 *	NOTE: we *must* use SysCacheGetAttr() for these attributes instead of 
	 *	peeking directly into the Form_pg_proc: these attributes follow the 
	 *	first variable-length attribute in the tuple and the Form_pg_proc 
	 *	typedef is meaningless beyond the first variable-length value.
	 */
	
	argModes = SysCacheGetAttr( PROCNAMEARGSNSP, proctup, Anum_pg_proc_proargmodes,	   &modesNull );
	argNames = SysCacheGetAttr( PROCNAMEARGSNSP, proctup, Anum_pg_proc_proargnames,	   &namesNull );
	argTypes = SysCacheGetAttr( PROCNAMEARGSNSP, proctup, Anum_pg_proc_proallargtypes, &allTypesNull );

	info->schemaOid  = procform->pronamespace;
	info->targetOid  = HeapTupleGetOid( proctup );
	info->targetName = pstrdup( NameStr( procform->proname ));
	info->argModes   = modesNull ? PointerGetDatum( NULL ) : (Datum)DatumGetArrayTypePCopy( argModes );
	info->argNames   = namesNull ? PointerGetDatum( NULL ) : (Datum)DatumGetArrayTypePCopy( argNames );
	info->langOid    = procform->prolang;
	info->fqName	 = makeFullName( info->schemaOid, info->targetName );
	info->returnsSet = procform->proretset;
	info->returnType = procform->prorettype;

	if( allTypesNull )
	{
		info->nargs = procform->pronargs;
	}
	else
	{
		ArrayType * argModesArray = DatumGetArrayTypeP( argModes );

		info->nargs = ArrayGetNItems(ARR_NDIM(argModesArray), ARR_DIMS(argModesArray));	
	}

	if( allTypesNull )
	{
		int arg;

		for( arg = 0; arg < info->nargs; ++arg )
			info->argTypes[arg] = procform->proargtypes.values[arg];
	}
	else
	{
		ArrayType * allTypes = DatumGetArrayTypeP( argTypes );
		Oid		  * typeOids = (Oid *)ARR_DATA_PTR( allTypes );
		int			arg;

		for( arg = 0; arg < info->nargs; ++arg )
			info->argTypes[arg] = typeOids[arg];
	}
}

/*******************************************************************************
 * getTargetFromOid()
 *
 *	Given an OID, we fill in the given targetInfo structure with a	bunch of 
 *  information about that target.
 */
static bool getTargetFromOid( targetInfo * info ) 
{
	HeapTuple	 procTuple;
	Form_pg_proc procForm = getProcFormByOid( info->targetOid, &procTuple );

	if( procForm == NULL )
		elog( ERROR, "cache lookup failed for function %u", info->targetOid );

	completeProcTarget( info, procTuple );

	ReleaseSysCache( procTuple );

	return( TRUE );
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
