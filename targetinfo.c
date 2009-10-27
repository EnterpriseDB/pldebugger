/*
 * targetinfo.c -
 *
 *  This file defines a single exported function, pldbg_get_target_info(),
 *  which returns whole boatload of information about a function or trigger.
 *  Specifically, pldbg_get_target_info() returns a tuple of type targetinfo
 *  (see pldbgapi.sql).  
 *
 *  You can call pldbg_get_target_info() with a function/trigger name, the 
 *  OID of the pg_proc (or pg_trigger) tuple of the desired function/trigger, 
 *  or the signature (such as 'my_factorial(int)') of the desired function.
 *
 *  If the target name is not fully qualified, this function performs a search-path
 *  search for the requested name.  If the target name is not a signature, this 
 *  function will throw an error on any ambiguity.
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
#include "utils/fmgroids.h"							/* For F_NAMEEQ					*/
#include "utils/array.h"							/* For DatumGetArrayTypePCopy()	*/
#include "storage/ipc.h"							/* For on_proc_exit()  			*/
#include "storage/proc.h"							/* For MyProc		   			*/
#include "libpq/libpq-be.h"							/* For Port						*/
#include "miscadmin.h"								/* For MyProcPort				*/
#include  "utils/catcache.h"
#include  "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_namespace.h"
#include "catalog/namespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/indexing.h"						/* For TriggerRelidNameIndexId	*/
#include "globalbp.h"
#include "parser/parse_type.h"						/* For parseTypeString()		*/
#include "access/heapam.h"							/* For heap_form_tuple()		*/
#include "access/genam.h"
#include "access/hash.h"							/* For dynahash stuff			*/
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
	char	targetType;				/* f(unction), p(rocedure), o(id), or t(rigger)						*/

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

	/* Working context */
	List   *   names;			/* Parsed-out name components (schema, target)							*/
	char  	 * schemaName;		/* Schema name															*/
	char  	 * funcName;		/* Function/procedure name												*/
	CatCList * catlist;			/* SysCacheList to release when finished								*/
} targetInfo;

static bool 		  	 getTargetDef( targetInfo * info );
static bool 		  	 getGlobalTarget( targetInfo * info );
static bool 		  	 getTargetFromOid( targetInfo * info );
static Oid 			  	 getTriggerFuncOid( const char * triggerName );
static void 		  	 parseNameAndArgTypes( const char *string, const char *caller, bool allowNone, List **names, int *nargs, Oid *argtypes );
static Form_pg_namespace getSchemaForm( Oid schemaOid, HeapTuple * tuple );
static Form_pg_proc      getProcFormByOid( Oid procOid, HeapTuple * tuple );
static void 		     completeProcTarget( targetInfo * info, HeapTuple proctup );
static char 		   * makeFullName( Oid schemaOID, char * targetName );
static TupleDesc	  	 getResultTupleDesc( FunctionCallInfo fcinfo );
static List            * parseNames(const char * string);

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

	/*
	 * Since we have to search through a lot of different tables to satisfy all of the 
	 * different search types (schema qualified, schema unqualified, global, ...) 
	 * we create a search-context structure (a structure of type targetInfo) and just pass
	 * that around instead of managing a huge argument list.
	 */
	info.rawName    = GET_STR( PG_GETARG_TEXT_P( 0 ));
	info.targetType = PG_GETARG_CHAR( 1 );

	/*
	 * getTargetDef() figures out what kind of search to embark upon - let it
	 * take care of the tough stuff.
	 */
	if( !getTargetDef( &info ))
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

/******************************************************************************
 * getTargetDef()
 *
 *	This is a helper function for pldbg_get_target_info() - it figures out what
 *	kind of search is needed, based on the string given by the caller. If the
 *	caller gave us an OID (or a pair of OID's), we let getTargetFromOid()
 *	retrieve the required information. If the caller gave us a name, we simply
 *  call getGlobalTarget().
 *
 *	If any of the helper functions (that we call) perform a syscache search,
 *	we take care of releasing the resources here.
 */

static bool getTargetDef( targetInfo * info )
{
	bool result;

	if( info->targetType == 'o' )
	{
		info->targetOid = atol( info->rawName );

		result = getTargetFromOid( info );
	}
	else if( info->targetType == 't' )
	{
		/*
		 * The user gave us a trigger name.  Get the pg_trigger tuple and then 
		 * treat this is a an 'oid' target
		 */
		
		if(( info->targetOid = getTriggerFuncOid( info->rawName )) == InvalidOid )
			elog( ERROR, "unknown trigger name(%s)", info->rawName );

		result = getTargetFromOid( info );
	}
	else
	{
		/*
		 * The user gave us a function - it may be schema-qualified.  Let
		 * parseNameAndArgTypes() figure out which name components are 
		 * present
		 */
		
		result = getGlobalTarget( info );

		if( info->catlist )
			ReleaseSysCacheList( info->catlist );
	}
		
	return( result );
}

static Oid getTriggerFuncOid( const char * triggerName )
{
	Oid			result = InvalidOid;
	Relation    tgrel  = heap_open( TriggerRelationId, AccessShareLock );
	ScanKeyData skey;
	SysScanDesc tgscan;

	ScanKeyInit( &skey, Anum_pg_trigger_tgname, BTEqualStrategyNumber, F_NAMEEQ, CStringGetDatum( triggerName ));

	tgscan = systable_beginscan( tgrel, TriggerRelidNameIndexId, false, SnapshotNow, 1, &skey );

	while( true )
	{
		HeapTuple tup = systable_getnext( tgscan );
	
		if( HeapTupleIsValid( tup ))
		{
			Form_pg_trigger	trigger = (Form_pg_trigger) GETSTRUCT( tup );

			result = trigger->tgfoid;
		}
		else
			break;
	}

	systable_endscan(tgscan);
	heap_close(tgrel, AccessShareLock);

	return( result );
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
	char			  * result       = NULL;
	size_t				len          = strlen( targetName )+2+1; /* Add room for quotes and null terminator */

	/* If we found a schema, make room for the quoted name (and the delimiter) */
	if( HeapTupleIsValid( schemaTuple ))
		len += strlen( NameStr( schemaForm->nspname ))+2+1; 

	/* Now allocate enough space to hold the fully-qualified, possibly quoted name */
	result = (char *) palloc( len );
	result[0] = '\0';

	/* And start putting the whole thing together - schema name first */
	if( HeapTupleIsValid( schemaTuple ))
	{
		strcat( result, quote_identifier( NameStr( schemaForm->nspname )));
		strcat( result, "." );

		ReleaseSysCache( schemaTuple );
	}

	/* 
	 * Finally the target name  - we end up with:
	 *	schema.target
	 */
	strcat( result, quote_identifier( targetName ));

	return( result );
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
 *	This function is called by getTargetDef() when the user gives a string that
 *  contains only digits and colons - in that case, we assume that the user
 *	gave us a single OID.
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
 * getProcOidBySig()
 *
 *	This helper function is called (indirectly) by pldbg_get_target_info() to 
 *	retrieve the OID of the pg_proc tuple that corresponds to the given 
 *  signature.  This is a slightly modified version of regprocedurein().
 */

static Oid getProcOidBySig( const char * pro_name_or_oid, char targetType )
{
	List	   		  *names;
	int				   nargs;
	Oid				   argtypes[FUNC_MAX_ARGS];
	FuncCandidateList  clist;
	
	/* '-' ? */
	if (strcmp(pro_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (pro_name_or_oid[0] >= '0' &&
		pro_name_or_oid[0] <= '9' &&
		strspn(pro_name_or_oid, "0123456789") == strlen(pro_name_or_oid))
	{
		return( DatumGetObjectId(DirectFunctionCall1(oidin, CStringGetDatum(pro_name_or_oid))));
	}

	/*
	 * Else it's a name and arguments.  Parse the name and arguments, look up
	 * potential matches in the current namespace search list, and scan to see
	 * which one exactly matches the given argument types.	(There will not be
	 * more than one match.)
	 *
	 * XXX at present, this code will not work in bootstrap mode, hence this
	 * datatype cannot be used for any system column that needs to receive
	 * data during bootstrap.
	 */
	parseNameAndArgTypes(pro_name_or_oid, "regprocedurein", false,
						 &names, &nargs, argtypes);

#if (PG_VERSION_NUM >= 80500)
	clist = FuncnameGetCandidates(names, nargs, false, false, false);
#else
	clist = FuncnameGetCandidates(names, nargs, false, false);
#endif

	for (; clist; clist = clist->next)
	{
		if (memcmp(clist->args, argtypes, nargs * sizeof(Oid)) == 0)
			break;
	}

	if (clist == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function \"%s\" does not exist", pro_name_or_oid)));

	return( clist->oid );
}

/*******************************************************************************
 * getProcOidByName()
 *
 *	This helper function is called (indirectly) by pldbg_get_target_info() to 
 *	retrieve the OID of the pg_proc tuple that corresponds to the given 
 *  name.  This is a slightly modified version of regprocin().
 */

static Oid getProcOidByName(const char * pro_name_or_oid, char targetType )
{
	List	   		  *names;
	FuncCandidateList  clist;

	/* '-' ? */
	if (strcmp(pro_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (pro_name_or_oid[0] >= '0' &&
		pro_name_or_oid[0] <= '9' &&
		strspn(pro_name_or_oid, "0123456789") == strlen(pro_name_or_oid))
	{
		return( DatumGetObjectId(DirectFunctionCall1(oidin, CStringGetDatum(pro_name_or_oid))));
	}

	/* Else it's a name, possibly schema-qualified 
	 *
	 * Parse the name into components and see if it matches any
	 * pg_proc entries in the current search path.
	 */
	names = parseNames(pro_name_or_oid);
#if (PG_VERSION_NUM >= 80500)
	clist = FuncnameGetCandidates(names, -1, false, false, false);
#else
	clist = FuncnameGetCandidates(names, -1, false, false);
#endif

	if (clist == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function \"%s\" does not exist", pro_name_or_oid)));
	else if (clist->next != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
				 errmsg("more than one function named \"%s\"",
						pro_name_or_oid)));

	return( clist->oid );
}

/*******************************************************************************
 * getGlobalTarget()
 *
 *	This helper function is called (indirectly) by pldbg_get_target_info() to 
 *	retreive information about a function or procedure that is *not* defined 
 *	within a package.
 *
 *	If the user gave us a specific schema name, we look for a target within
 *  that schema.  Otherwise, we search through the current search_path for 
 *	a match.
 *
 *	If we find a match, we fill in the targetInfo structure with information
 *  about the target that we've identified.
 */

static bool getGlobalTarget( targetInfo * info )
{
	bool	inQuote = false;
	char  * ptr;

	for( ptr = info->rawName; *ptr; ptr++ )
	{
		if( *ptr == '"' )
			inQuote = !inQuote;
		else if( *ptr == '(' && !inQuote )
			break;
	}

	if( *ptr == '\0' )
	{
		/* 
		 * The user gave us function/procedure name without an
		 * argument list (i.e. 'foo').
		 */
		info->targetOid = getProcOidByName( info->rawName, info->targetType );
	}
	else
	{
		/* 
		 * The user gave us a function/procedure name with an 
		 * argument list (i.e. 'foo(int, text)', or 'foo()')
		 */
		info->targetOid = getProcOidBySig( info->rawName, info->targetType );
	}

	return( getTargetFromOid( info ));
}

/*******************************************************************************
 * parseNameAndArgTypes()
 *
 *	This helper function parses the given string and returns a list of names 
 *	(schema and target), a count of the number of arguments present
 *  in the signature, and an array of OID's for the argument types.
 *
 *	You can call this function with a fully-qualified name (schema.target, or
 *  an unqualified name (target).  
 *
 *  You can also call this function with a list of argument types 
 *  (myfoo( int, real )) or without a list of argument types (myfoo).  
 *
 *  If you provide a complete signature (that is, you include argument types),
 *  *nargs is set to the number of arguments and *argtypes is filled in with
 *  the resolved OID of each type.  This function takes care of translating 
 *  typename synonyms for you - so you can call it with: myfoo(int) or 
 *  myfoo(integer).
 *
 *  If you don't provide a complete signature, *nargs is set to -1.
 *
 *	NOTE: this function borrows heavily from the parseNameAndArgTypes()
 *        function in regproc.c. That function will not accept a string
 *		  that does not include a complete signature - we do.
 */

static void parseNameAndArgTypes( const char *string, const char *caller, bool allowNone, List **names, int *nargs, Oid *argtypes )
{
	char	   *rawname;
	char	   *ptr;
	char	   *ptr2;
	char	   *typename;
	bool		in_quote;
	bool		had_comma;
	int			paren_count;
	Oid			typeid;
	int32		typmod;

	/* We need a modifiable copy of the input string. */
	rawname = pstrdup(string);

	/* Scan to find the expected left paren; mustn't be quoted */
	in_quote = false;
	for (ptr = rawname; *ptr; ptr++)
	{
		if (*ptr == '"')
			in_quote = !in_quote;
		else if (*ptr == '(' && !in_quote)
			break;
	}

	if (*ptr == '\0')
	{
		/* This is a function/name only, not a signature. */
		*names = parseNames(rawname);
		*nargs = -1;
		return;
	}

	/* Separate the name and parse it into a list */
	*ptr++ = '\0';
	*names = parseNames(rawname);

	/* Check for the trailing right parenthesis and remove it */
	ptr2 = ptr + strlen(ptr);
	while (--ptr2 > ptr)
	{
		if (!isspace((unsigned char) *ptr2))
			break;
	}
	if (*ptr2 != ')')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("expected a right parenthesis")));

	*ptr2 = '\0';

	/* Separate the remaining string into comma-separated type names */
	*nargs = 0;
	had_comma = false;

	for (;;)
	{
		/* allow leading whitespace */
		while (isspace((unsigned char) *ptr))
			ptr++;
		if (*ptr == '\0')
		{
			/* End of string.  Okay unless we had a comma before. */
			if (had_comma)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("expected a type name")));
			break;
		}
		typename = ptr;
		/* Find end of type name --- end of string or comma */
		/* ... but not a quoted or parenthesized comma */
		in_quote = false;
		paren_count = 0;
		for (; *ptr; ptr++)
		{
			if (*ptr == '"')
				in_quote = !in_quote;
			else if (*ptr == ',' && !in_quote && paren_count == 0)
				break;
			else if (!in_quote)
			{
				switch (*ptr)
				{
					case '(':
					case '[':
						paren_count++;
						break;
					case ')':
					case ']':
						paren_count--;
						break;
				}
			}
		}
		if (in_quote || paren_count != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("improper type name")));

		ptr2 = ptr;
		if (*ptr == ',')
		{
			had_comma = true;
			*ptr++ = '\0';
		}
		else
		{
			had_comma = false;
			Assert(*ptr == '\0');
		}
		/* Lop off trailing whitespace */
		while (--ptr2 >= typename)
		{
			if (!isspace((unsigned char) *ptr2))
				break;
			*ptr2 = '\0';
		}

		if (allowNone && pg_strcasecmp(typename, "none") == 0)
		{
			/* Special case for NONE */
			typeid = InvalidOid;
			typmod = -1;
		}
		else
		{
			/* Use full parser to resolve the type name */
			parseTypeString(typename, &typeid, &typmod);
		}
		if (*nargs >= FUNC_MAX_ARGS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg("too many arguments")));

		argtypes[*nargs] = typeid;
		(*nargs)++;
	}

	pfree(rawname);
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

/*******************************************************************************
 * parseNames()
 *
 *  This functions provides a version-neutral wrapper around the 
 *  stringToQualifiedNameList() function.  The signature for 
 *  stringToQualifiedNameList() changed between 8.2 and 8.3 - the second 
 *  argument was never very valuable anyway.
 */

static List * parseNames( const char * string )
{
#if PG_VERSION_NUM >= 80300
	return stringToQualifiedNameList(string);
#else
	return stringToQualifiedNameList(string, "parseNames");
#endif
}
