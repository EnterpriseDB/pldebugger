/**********************************************************************
 * dbginfo.c - support functions for PL debugger client applications
 *
 *  Copyright 2006 - EnterpriseDB, Inc.  All Rights Reserved
 *
 **********************************************************************/

#include "postgres.h"
#include "funcapi.h"								/* For ReturnSetInfo			*/
#include "utils/builtins.h"							/* For textout, namein, textin  */
#include "utils/fmgroids.h"							/* For F_NAMEEQ					*/
#include "utils/array.h"							/* For DatumGetArrayTypePCopy()	*/
#include "utils/catcache.h"
#include "utils/syscache.h"							/* For CatCList					*/
#include "catalog/pg_proc.h"						/* For Form_pg_proc				*/
#include "catalog/pg_namespace.h"					/* For Form_pg_namespace		*/
#include "catalog/namespace.h"						/* For fetch_search_path()		*/
 
#include "catalog/pg_trigger.h"
#include "catalog/indexing.h"						/* For TriggerRelidNameIndexId	*/
#include "parser/parse_type.h"						/* For parseTypeString()		*/
#include "access/heapam.h"							/* For heap_form_tuple()		*/
#include "access/genam.h"

#include "dbginfo_edb.h"							/* For targetInfo				*/

/**********************************************************************
 * Macro definitions
 **********************************************************************/

#define	INCLUDE_PACKAGE_ENHANCEMENTS	FALSE

#define GET_STR( textp ) 		DatumGetCString( DirectFunctionCall1( textout, PointerGetDatum( textp )))

/**********************************************************************
 * Exported function declarations
 **********************************************************************/

PG_FUNCTION_INFO_V1( pldbg_get_target_info );		
Datum pldbg_get_target_info( PG_FUNCTION_ARGS );

/*******************************************************************************
 * Static function declarations
 ******************************************************************************/

static bool 		  getTargetDef( targetInfo * info );
static bool 		  getPkgTarget( targetInfo * info );
static bool 		  getGlobalTarget( targetInfo * info );
static bool 		  getTargetFromOid( targetInfo * info );
static Oid 			  getTriggerFuncOid( const char * triggerName );
static void 		  parseNameAndArgTypes( const char *string, const char *caller, bool allowNone, List **names, int *nargs, Oid *argtypes );
static int 			  countCandidatesInSchema( CatCList * candidates, Oid schema, int * hit );
static void 		  completeProcTarget( targetInfo * info, HeapTuple proctup );
static char 		* makeFullName( Oid schemaOID, Oid packageOID, char * targetName );
static void 		  parseQualifiedName( targetInfo * info );
static char 		* getPackageName( Oid packageOid );
static bool 		  getPackageTargetFromOid( targetInfo * info );
static TupleDesc	  getResultTupleDesc( FunctionCallInfo fcinfo );
static CatCList 	* getProcCandidates( targetInfo * info );

/********************************************************************************
 *  pldbg_get_target_info( sigNameOrOid TEXT, isFunc bool ) RETURNS targetInfo
 *
 *	This function retrieves a whole truckload of information about a function or procedure.
 *
 *	You can call this function to find, among other things, the OID of the function/procedure
 *	and the OID of the package in which the function/procedure is defined.
 *
 *	This function expects two argument: sigNameOrOid, and isFunc.
 *
 *	The first argument may be any of the following:
 *		An OID	('33223')
 *
 *		A package OID and a function OID ( '22132:33223' )
 *	 		if the packageOID and the functionOID are the same, we return info about
 *			the package initializer
 *
 *		A function or procedure name
 *		A function or procedure signature (name plus argument types)
 *	   	A package function or package procedure name
 *	   	A package function or package procedure signature (name plus argument types)
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
 *		the package OID (or InvalidOid)
 *		the schema OID
 *		the argument types
 *		the targetName (not schema-qualified or package-qualified)
 *		the argument modes (in, out, or both)
 *		the argument names
 *		the OID of the pg_language row for the target (typically edb-spl or plpgsql)
 *		the fully-qualified name of the target
 *		a boolean indicating whether the target is a function(true) or a procedure(false)
 */

Datum pldbg_get_target_info( PG_FUNCTION_ARGS )
{
	TupleDesc	tupleDesc  = getResultTupleDesc( fcinfo );
	targetInfo	info 	   = {0};
	Datum		values[14] = {0};
	bool		nulls[14]  = {0};
	HeapTuple	result;

	/*
	 * Since we have to search through a lot of different tables to satisfy all of the 
	 * different search types (schema qualified, schema unqualified, package 
	 * qualified, global, ...) we create a search-context structure (a structure
	 * of type targetInfo) and just pass that around instead of managing a 
	 * huge argument list.
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
	values[1]  = ObjectIdGetDatum( info.packageOid );						
	values[2]  = ObjectIdGetDatum( info.schemaOid );
	values[3]  = Int32GetDatum((int32)info.nargs );
	values[4]  = Int32GetDatum((int32)info.requiredArgs );
	values[5]  = PointerGetDatum( buildoidvector( info.argTypes, info.nargs ));
	values[6]  = DirectFunctionCall1( namein, PointerGetDatum( info.targetName ));
	values[7]  = info.argModes;	
	values[8]  = info.argNames;
	values[9]  = ObjectIdGetDatum( info.langOid );
	values[10] = BoolGetDatum( info.isFunc );
	values[11] = DirectFunctionCall1( textin, PointerGetDatum( info.fqName ));
	values[12] = BoolGetDatum( info.returnsSet );
	values[13] = ObjectIdGetDatum( info.returnType );

	nulls[0]  = FALSE;							/* targetOID 	*/
	nulls[1]  = FALSE;							/* packageOID 	*/
	nulls[2]  = FALSE;							/* schemaOID 	*/
	nulls[3]  = FALSE;							/* nargs		*/
	nulls[4]  = FALSE;							/* requiredArgs */
	nulls[5]  = FALSE;							/* argTypes		*/
	nulls[6]  = FALSE;							/* targetName	*/
	nulls[7]  = info.argModes ? FALSE : TRUE;	/* argModes	*/
	nulls[8]  = info.argNames ? FALSE : TRUE;	/* argNames 	*/
	nulls[9]  = FALSE;							/* targetLang	*/
	nulls[10] = FALSE;							/* isFunc		*/
	nulls[11] = FALSE;							/* fqName		*/
	nulls[12] = FALSE;							/* returnsSet	*/
	nulls[13] = FALSE;							/* returnType 	*/

	result = heap_form_tuple( tupleDesc, values, nulls );

	PG_RETURN_DATUM( HeapTupleGetDatum( result ));	   
}


/******************************************************************************
 * getTargetDef()
 *
 *	This is a helper function for pldbg_get_target_info() - it figures out what
 *	kind of search is needed, based on the string given by the caller. If the
 *	caller gave us an OID (or a pair of OID's), we let getTargetFromOid()
 *	retrieve the required information. If the caller gave us a name, we parse 
 *	it out to determine whether the string includes a package name - if so, 
 *	call getPkgTarget(), if not, we call getGlobalTarget().
 *
 *	If any of the helper functions (that we call) perform a syscache search,
 *	we take care of releasing the resources here.
 */

static bool getTargetDef( targetInfo * info )
{
	bool result;

	if( info->targetType == 'o' )
	{
		char	* rawName   = pstrdup( info->rawName );
		char	* delimiter = strchr( rawName, ':' );
		/* 
		 * The user gave us a target OID in one of the following forms:
		 *		packageOID:functionOID
		 *		-:functionOID
		 *		functionOID
		 * Figure out which pieces we have
		 */
	
		if( delimiter == NULL )
		{
			/* Just a functionOID */
			info->targetOid  = atol( rawName );
			info->packageOid = InvalidOid;
		}
		else if( rawName[0] == '-' )
		{
			/* Got '-:functionOID'	*/
			info->targetOid  = atol( delimiter + 1 );
			info->packageOid = InvalidOid;
		}
		else
		{
			/* Got 'packageOID:functionOID'	*/
			info->targetOid  = atol( delimiter + 1 );
			info->packageOid = atol( rawName );
		}

		result = getTargetFromOid( info );
	}
	else if( info->targetType == 't' )
	{
		/*
		 * The user have us a trigger name.  Get the pg_trigger tuple and then 
		 * treat this is as an 'oid' target
		 */
		
		if(( info->targetOid = getTriggerFuncOid( info->rawName )) == InvalidOid )
			elog( ERROR, "unknown trigger name(%s)", info->rawName );

		info->packageOid = InvalidOid;

		result = getTargetFromOid( info );
	}
	else
	{
		/*
		 * The user gave us a function or procedure name - it may be 
		 * schema-qualified and it may include a package name.  Let
		 * parseNameAndArgTypes() figure out which name components
		 * are present
		 */
		
		if( info->targetType == 'f' )
			info->isFunc = TRUE; 
		else
			info->isFunc = FALSE;
			 
		parseNameAndArgTypes( info->rawName, "getTargetDef", false, &info->names, &info->nargs, info->argTypes );

		parseQualifiedName( info );

		if( OidIsValid( info->packageOid ))
			result = getPkgTarget( info );
		else
			result = getGlobalTarget( info );

		if( info->catlist )
			ReleaseSysCacheList( info->catlist );
	}
		
	return( result );
}

/******************************************************************************
 * getTriggerFuncOid()
 *
 *	Given the name of a trigger, this function returns the OID of the function
 *	that implements that trigger (or InvalidOid if the trigger name is invalid)
 */

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

/******************************************************************************
 * sortBySearchPath()
 *
 *	This is a helper function for pldbg_get_target_info() - it sorts a list of
 *	pg_proc tuples by schema.  Actually, we don't change the physical ordering
 *	of the tuples - instead, we build a map instead (and return that map to 
 *  the caller). The map is just an array of HeapTuple pointers that happen to
 *	point to the pg_proc tuples in the right order.
 *
 *	Note: the map may *not* include a pointer for every pg_proc tuple that the 
 *	caller gives us.  We only include those pg_proc tuples defined in a schema
 *	that's in the current search path. If catlist points to a pg_proc tuple that
 *	is *not* defined in a schema in the current search path, we don't store a
 *	pointer to that tuple in the map.
 *
 *	You can find out how many entries are in the resulting map by looking at
 *	the 'matches' argument in the caller.
 *
 *	For example, say that catlist points to the following tuples:
 *		namespace   | signature
 *		------------+--------------------
 *		payables    | addAccount(int, numeric)
 *		receivables | addAccount(int, numeric)
 *		payroll     | addAccount(real)
 *
 *	And that search_path is set to "payables,payroll"
 *
 *	The second tuple (receivables.addAccount(int,numeric)) is not defined
 *	within a schema that's part of the search_path. We won't include that 
 *	tuple in the map that we build and *matches will be set to 2.
 */

static HeapTuple * sortBySearchPath( CatCList * catlist, int * matches )
{
	HeapTuple * map = palloc( catlist->n_members * sizeof( HeapTuple ));
	List	  * searchPath = fetch_search_path( TRUE );
	ListCell  * nsp;
	int			i;

	*matches = 0;

	/*
	 * Because we add tuples to the map in schema-name order, we're
	 * effectively doing a sort at the same time we're picking out 
	 * the hits
	 */

	/* For each schema in the search path... */
	foreach( nsp, searchPath )
	{
		/* For each pg_proc tuple in *catlist ... */
		for( i = 0; i < catlist->n_members; ++i )
		{
			HeapTuple		 proctup  = &catlist->members[i]->tuple;
			Form_pg_proc 	 procform = (Form_pg_proc) GETSTRUCT( proctup );
			
			if( procform->pronamespace == lfirst_oid( nsp ))
			{
				/* This pg_proc tuple is defined in a schema in the search_path */
				map[(*matches)++] = proctup;
			}
		}
	}
			 
	list_free( searchPath );

	return( map );
}

/*******************************************************************************
 * argTypesMatch()
 *
 *	This helper function compares the data type of each argument in a signature.
 *  If the data types in the argument lists are identical, we return TRUE, 
 *	otherwise, we return FALSE.
 *
 *	'left' and 'right' must point to an array of OID's (presumably each OID 
 *  corresponds to a pg_type tuple). We assume that each array contains exactly
 *	'count' entries.
 */

static bool argTypesMatch( Oid * left, Oid * right, int count )
{
	if( memcmp( left, right, count ) == 0 )
		return( TRUE );
	else
		return( FALSE );
}

/*******************************************************************************
 * argTypesDiffer()
 *
 *	This helper function compares the data type of each argument in a signature.
 *  If the data types in the argument lists differ, we return TRUE, otherwise, 
 *  we return FALSE.
 *
 *	'left' and 'right' must point to an array of OID's (presumably each OID 
 *  corresponds to a pg_type tuple). We assume that each array contains exactly
 *	'count' entries.
 */

static bool argTypesDiffer( Oid * left, Oid * right, int count )
{
	if( argTypesMatch( left, right, count ))
		return( FALSE );
	else
		return( TRUE );
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
	if( OidIsValid( schemaOid ))
	{
		*tuple = SearchSysCache( NAMESPACEOID, ObjectIdGetDatum( schemaOid ), 0, 0, 0 );

		if( HeapTupleIsValid( *tuple ))
			return((Form_pg_namespace) GETSTRUCT( *tuple ));
		else
			return( NULL );
	}
	else
	{
		return( NULL );
	}
}

/*******************************************************************************
 * getSchemaName()
 *
 *	This helper function, given the OID of a row in the pg_namespace table, 
 *	returns the name of that namespace.
 */

static char * getSchemaName( Oid schemaOid )
{
	HeapTuple			schemaTuple = NULL;
	Form_pg_namespace	schema      = getSchemaForm( schemaOid, &schemaTuple );

	if( schema == NULL )
		return( NULL );

	return( pstrdup( NameStr( schema->nspname )));
}

/*******************************************************************************
 * makeFullName()
 *
 *	This helper function will construct a fully-qualified name for the given
 *	target. If the target is defined in a package, the fully-qualified name 
 *  will include the name of the package.  The fully-qualified name will always
 *  include the name of the schema in which the target is defined.
 */

static char * makeFullName( Oid schemaOID, Oid packageOID, char * targetName )
{
	char			  * schemaName   = getSchemaName( schemaOID );
	char			  * packageName  = getPackageName( packageOID );
	char			  * result       = NULL;
	size_t				len          = strlen( targetName ) + 1;

	/* If we found a schema, make room for the name (and the delimiter) */
	if( schemaName != NULL )
		len += strlen( schemaName ) + 1;

	/* If we found a package, make room for the name (and the delimiter) */
	if( packageName != NULL )
		len += strlen( packageName ) + 1;

	/* Now allocate enough space to hold the fully-qualified name */
	result = (char *) palloc( len );
	result[0] = '\0';

	/* And start putting the whole thing together - schema name first */
	if( schemaName )
	{
		strcat( result, schemaName );
		strcat( result, "." );
	}

	/* Now the package name */
	if( packageName )
	{
		strcat( result, packageName );
		strcat( result, "." );
	}

	/* 
	 * Finally the target name  - we end up with:
	 *	schema.package.target
	 * or
	 *	schema.target
	 */
	strcat( result, targetName );

	return( result );
}

/*******************************************************************************
 * countCandidatesInSchema()
 *
 *	This helper function returns the number of pg_proc tuples (in the candidates
 *  list) that are defined in the given schema. If the count is non-zero, *hit 
 *  returns the element number of the last match.
 *
 *  We use this function to determine whether a name is unique within the given
 *	schema (to detect ambiguity in the case where the user gives us a function 
 *  name, but not a complete signature).
 */

static int countCandidatesInSchema( CatCList * candidates, Oid schema, int * hit )
{
	int		count = 0;
	int		i;

	for( i = 0; i < candidates->n_members; ++i )
	{
		HeapTuple		 proctup  = &candidates->members[i]->tuple;
		Form_pg_proc 	 procform = (Form_pg_proc) GETSTRUCT( proctup );

		if( procform->pronamespace == schema )
		{
			*hit = i;
			count++;
		}
	}

	return( count );
}

/*******************************************************************************
 * getTargetFromOid()
 *
 *	This function is called by getTargetDef() when the user gives a string that
 *  contains only digits and colons - in that case, we assume that the user
 *	gave us a single OID (the OID of a global function or procedure) or a colon
 *  separated pair of OID's (the OID of a package and the OID of a function or
 *  procedure within that package).
 *
 *	Given one or two OID's, we fill in the given targetInfo structure with a 
 *	bunch of inforamtion about that target.
 *
 *  NOTE: if the caller gives us two OID's and they happen to be identical, we
 *		  assume that the caller wants information about the initializer for a
 *		  package (and the OID he's given us is the OID of that package).
 */

static bool getTargetFromOid( targetInfo * info ) 
{
	if( OidIsValid( info->packageOid ))
		return( getPackageTargetFromOid( info ));
	else
	{
		/*
		 * The user gave us a single OID - assume that it's the OID of a pg_proc tuple.
		 */

		HeapTuple proctup = SearchSysCache( PROCOID, ObjectIdGetDatum( info->targetOid ), 0, 0, 0 );
		
		if( !HeapTupleIsValid( proctup ))
			elog( ERROR, "cache lookup failed for function %u", info->targetOid );

		completeProcTarget( info, proctup );

		ReleaseSysCache( proctup );

		return( TRUE );
	}

	return( FALSE );
}

/*******************************************************************************
 * getGlobalTarget()
 *
 *	This helper function is called (indirectly) by pldbg_get_target_info() to 
 *	retrieve information about a function or procedure that is *not* defined 
 *	within a package.
 *
 *  The caller already parsed out the raw name and decided that the user did
 *  not include a package name.
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
	/*
	 * If the user gave us an explicit schema name, we only search within
	 * that schema, otherwise, we search through all of the schema's in 
	 * the current search_path.
	 */

	if( info->schemaName )
	{
		info->schemaOid = LookupExplicitNamespace( info->schemaName );
	}
	else
	{
		info->schemaOid = InvalidOid;
	}

	/* Build a list of all pg_proc entries with the given name (and isFunc) */
	info->catlist = getProcCandidates( info );

	if( info->catlist->n_members == 0 )
	{
		return( FALSE );
	}

	/*
	 * Search for:
	 *
	 *	The only match (argtypes) if the user gave us no argtypes (schema.foo or foo)
	 *	An exact match (namespace + argtypes) if the user gave us a namespace and argtypes ( schema.foo( int ))
	 *  The first match (argtypes) in namespace order if the user gave use argtypes but no namespace ( foo( int ))
	 *
	 *	Note that, unlike FuncnameGetCandidates(), we don't build a list of candidates - we want 
	 *	an exact match on argument types, not a list of coercable functions
	 */

	if( info->nargs == -1 )
	{
		int hit 	= 0;

		/* 
		 * The user gave us a function/procedure name, not a complete
		 * signature (that is, he gave us 'foo' instead of 'foo(int)').
		 *
		 * If we found more than one function/procedure with the given
		 * name, it's an error (because we can't disambiguate the name
		 * without the argument types).
		 */
		
		if( OidIsValid( info->schemaOid ))
		{
			/*
			 * The user gave us a schema-qualified name (not a signature) 
			 * (something like 'sch.foo').  
			 *
			 * The catlist may contain multiple functions with the 
			 * name foo, we have to find the one defined in the given
			 * schema.  If we find more than one in the given schema,
			 * it's an error because we can't disambiguate.
			 */
			
			if( countCandidatesInSchema( info->catlist,  info->schemaOid, &hit ) > 1 )
			{
				ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("%s %s is not unique in the given schema",
							info->isFunc ? "function" : "procedure", 
							info->rawName),
					 errhint("More than one %s named %s is defined in the given schema. "
							 "You must provide a complete signature.", info->isFunc ? "function" : "procedure", info->funcName )));
			}
			
			completeProcTarget( info, &info->catlist->members[hit]->tuple );
			return( TRUE );

		}
		else
		{
			int			matches;
			HeapTuple * map = sortBySearchPath( info->catlist, &matches );

			if( matches == 0 )
			{
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("%s %s does not exist in the search_path", info->isFunc ? "function" : "procedure", info->funcName )));
			}
			else if( matches > 1 )
			{
				ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("%s %s is not unique in search_path",
							info->isFunc ? "function" : "procedure", 
							info->rawName),
					 errhint("More than one %s defined in the search_path. "
							 "Specify a schema or provide a complete signature.", info->isFunc ? "function" : "procedure" )));
			}

			completeProcTarget( info, map[0] );

			pfree( map );

			return( TRUE );
		}
		
	}

	/*
	 * The user gave us a signature
	 */

	if( OidIsValid( info->schemaOid ))
	{
		int	i;

		/*
		 * The user explicitly specified a schema so we won't bother with
		 * a namespace search - just find an exact match based on the 
		 * argument types.
		 */

		for( i = 0; i < info->catlist->n_members; i++ )
		{
			HeapTuple		 proctup  = &info->catlist->members[i]->tuple;
			Form_pg_proc 	 procform = (Form_pg_proc) GETSTRUCT( proctup );

			/* Ignore this proc if it's not in the desired namespace */
			if( procform->pronamespace != info->schemaOid )
				continue;

			/* Ignore this proc if it's argument count differs from the one we're looking for */
			if( procform->pronargs != info->nargs )
				continue;

			/* Ignore this proc if the argument types differ */
			if( argTypesDiffer( procform->proargtypes.values, info->argTypes, info->nargs ))
				continue;

			/* We have an exact match... */
			completeProcTarget( info, proctup );
			return( TRUE );
		}
	}
	else
	{
		/* 
		 * The user gave us a signature but did not qualify the procedure name
		 * with a schema.  That means we have to search for an exact match on 
		 * argument types, but we want to return the first exact match in 
		 * searchpath order.  However, the candidates are not sorted in searchpath
		 * order.
		 *
		 * To make this search a little more understandable, we'll make a map that 
		 * is ordered by searchpath.  sortBySearchPath() will also filter out any
		 * candidates that are not in the searchpath so 'matches' may be less than
		 * info->catlist->n_members.
		 */

		int			matches;
		int			i;
		HeapTuple * map = sortBySearchPath( info->catlist, &matches );

		for( i = 0; i < matches; i++ )
		{
			HeapTuple		 proctup  = map[i];
			Form_pg_proc 	 procform = (Form_pg_proc) GETSTRUCT( proctup );

			/* Ignore this proc if it's argument count differs from the one we're looking for */
			if( procform->pronargs != info->nargs )
				continue;

			/* Ignore this proc if the argument types differ */
			if( argTypesDiffer( procform->proargtypes.values, info->argTypes, info->nargs ))
				continue;

			/* We have an exact match... */
			completeProcTarget( info, proctup );
			return( TRUE );
		}
	}

	return( FALSE );
}

/*******************************************************************************
 * parseNameAndArgTypes()
 *
 *	This helper function parses the given string and returns a list of names 
 *	(schema, package, and target), a count of the number of arguments present
 *  in the signature, and an array of OID's for the argument types.
 *
 *	You can call this function with a fully-qualified name (schema.target or 
 *  schema.package.target), a partially-qualified name (package.target), or 
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
		*names = stringToQualifiedNameList(rawname, caller);
		*nargs = -1;
		return;
	}

	/* Separate the name and parse it into a list */
	*ptr++ = '\0';
	*names = stringToQualifiedNameList(rawname, caller);

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
 * This PL debugger can be configures to support EDB's suite of package
 * enhancements.  The package suite adds support for packages, procedures,
 * and default argument values.  
 *
 * We factor package-specific enhancements into a separate file so that we
 * can easily compile in the presence (or absence) of the package enhancements.
 */

#if INCLUDE_PACKAGE_ENHANCEMENTS
#include "dbginfo_edb.c"
#else
#include "dbginfo_pgsql.c"
#endif
