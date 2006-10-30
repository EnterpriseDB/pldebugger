/**********************************************************************
 * dbginfo_edb.c - EDB-specific support functions for PL debugger 
 *				   client applications
 *
 *	NOTE: This file is #included by dbginfo.c - it is not intended as
 *		  a stand-alone compile unit and will not compile by itself.
 *		  We isolate language-specific code into two different C files
 *		  and include the appropriate code depending on our compile 
 *		  environment. 
 *
 *  Copyright 2006 - EnterpriseDB, Inc.  All Rights Reserved
 *
 **********************************************************************/

#include "catalog/edb_package.h"
#include "catalog/edb_pkgelements.h"
#include "catalog/pg_type.h"

/**********************************************************************
 * Symbol definitions
 **********************************************************************/

#define PACKAGE_LANG			"edbspl"	/* Package element language, by definition	*/

/*******************************************************************************
 * Static function declarations
 ******************************************************************************/

static Oid 			  getLangOid( const char * langName );
static void 		  completePackageTarget( targetInfo * info, HeapTuple pkgElemTup, Oid schema, int cacheID );
static void 		  computeRequiredArgs( targetInfo * info, Datum defValDatum, bool isNull );

/*******************************************************************************
 * getLangOid()
 *
 *	This (rather unsatisfying) helper function returns the OID of the language
 *  tuple that corresponds to the given language name.
 *
 *  It would be nice if we had a constant hanging around for each pg_language,
 *  but we don't.
 */

static Oid getLangOid( const char * langName )
{
	HeapTuple languageTuple = SearchSysCache( LANGNAME, CStringGetDatum( langName ), 0, 0, 0 );
	Oid       result;

	if( !HeapTupleIsValid( languageTuple ))
		elog( ERROR, "cache lookup failed for language %s", langName );

	result = HeapTupleGetOid( languageTuple );	

	ReleaseSysCache( languageTuple );

	return( result );
}

/*******************************************************************************
 * parseQualifiedName()
 *
 *	This function splits a name list (info->names) into schema, package, and 
 *	function (or procedure) name.  It also retrieves the OID of the schema and
 *	package (if appropriate).  
 *
 *	NOTE: we split this code out into a separate function because the PL/pgSQL
 *		  implementation differs from the EDB SPL implementation.
 */

static void parseQualifiedName( targetInfo * info )
{
	info->packageOid = DeconstructQualifiedNamePackage( info->names, &info->schemaOid, &info->schemaName, &info->pkgName, &info->funcName );
}

/*******************************************************************************
 * getPackageForm()
 *
 *	This helper function, given the OID of a row in the edb_package table, 
 *	returns a pointer to that row (or NULL if the row does not exist).
 *
 *	In addition to returning a pointer to the requested row, we return a 
 *	tuple handle and the caller must ReleaseSysCache( *tuple ) when it's 
 *	finished with it.
 */

static Form_edb_package getPackageForm( Oid packageOid, HeapTuple * tuple )
{
	if( OidIsValid( packageOid ))
	{
		*tuple = SearchSysCache( PACKAGEOID, ObjectIdGetDatum( packageOid ), 0, 0, 0 );
		
		if( HeapTupleIsValid( *tuple ))
			return((Form_edb_package) GETSTRUCT( *tuple ));
		else
			return( NULL );
	}

	return( NULL );
}


/*******************************************************************************
 * getPackageName()
 *
 *	This helper function, given the OID of a row in the edb_package table, 
 *	returns the name of that package.
 */

static char * getPackageName( Oid packageOid )
{
	HeapTuple			packageTuple = NULL;
	Form_edb_package	package 	 = getPackageForm( packageOid, &packageTuple );

	if( package == NULL )
		return( NULL );

	return( pstrdup( NameStr( package->pkgname )));
}

/*******************************************************************************
 * completePackageTarget()
 *
 *	Given an edb_pkgelements tuple, this function copies the target definition 
 *	into the targetInfo structure.
 */

static void completePackageTarget( targetInfo * info, HeapTuple pkgElemTup, Oid schema, int cacheID )
{
	Form_pkgelements 		pkgform = (Form_pkgelements) GETSTRUCT( pkgElemTup );
	Datum					argModes;
	Datum					argNames;
	Datum		 			defVals;
	Datum		 			argTypes;
	bool					modesNull;
	bool					namesNull;
	bool		 			defValsNull;
	bool		 			allTypesNull;

	/* NOTE: we *must* use SysCacheGetAttr() to find the argModes and argNames */
	argModes = SysCacheGetAttr( cacheID, pkgElemTup, Anum_edb_pkgelements_argmodes,    &modesNull );
	argNames = SysCacheGetAttr( cacheID, pkgElemTup, Anum_edb_pkgelements_argnames,    &namesNull );
	argTypes = SysCacheGetAttr( cacheID, pkgElemTup, Anum_edb_pkgelements_allargtypes, &allTypesNull );
	defVals  = SysCacheGetAttr( cacheID, pkgElemTup, Anum_edb_pkgelements_argdefvals,  &defValsNull );

	info->targetOid  = HeapTupleGetOid( pkgElemTup );
	info->schemaOid  = schema;
	info->targetName = pstrdup( NameStr( pkgform->eltname ));
	info->argModes   = modesNull ? PointerGetDatum( NULL ) : (Datum)DatumGetArrayTypePCopy( argModes );
	info->argNames   = namesNull ? PointerGetDatum( NULL ) : (Datum)DatumGetArrayTypePCopy( argNames );
	info->langOid    = getLangOid( PACKAGE_LANG );
	info->isFunc     = ( pkgform->eltclass == FUNCTION_ELT ) ? TRUE : FALSE;
	info->fqName	 = makeFullName( info->schemaOid, info->packageOid, info->targetName );
	info->returnsSet = FALSE;	/* FIXME: do we support SRF's in packages? apparently not at the moment */
	info->returnType = pkgform->eltdatatype;

	if( allTypesNull )
	{
		info->nargs = pkgform->nargs;
	}
	else
	{
		info->nargs = ArrayGetNItems(ARR_NDIM(argModes), ARR_DIMS(argModes));
	}

	/*
	 * Compute the number of required arguments 
	 *
	 *	NOTE: we must call this function after info->nargs is set since 
	 *		  computeRequiredArgs() relies on that value
	 */

	computeRequiredArgs( info, defVals, defValsNull );

	if( allTypesNull )
	{
		int arg;

		for( arg = 0; arg < info->nargs; ++arg )
			info->argTypes[arg] = pkgform->argtypes.values[arg];
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
 * getPackageTargetFromOid()
 *
 *	Given a package OID and a target OID, this function completes the targetInfo
 *	structure for the target function/procedure.  If the package OID and target 
 *	OID are identical, we assume that the caller wants us to report information 
 *	about the package initializer.
 */	

static bool getPackageTargetFromOid( targetInfo * info )
{
	if(( info->packageOid == info->targetOid ) && OidIsValid( info->packageOid ))
	{
		HeapTuple		  pkgTup;
		Form_edb_package  pkgForm;

		/* 
		 * The user gave is a package OID and a function OID and they happen to
		 * be identical - that means that the user wants information about the 
		 * initializer for the given package
		 */

		if(( pkgForm = getPackageForm( info->packageOid, &pkgTup )) == NULL )
			elog( ERROR, "cache lookup failed for package %u", info->packageOid );

		info->schemaOid = pkgForm->pkgnamespace;
		info->targetName = pstrdup( NameStr( pkgForm->pkgname ));
		info->nargs      = 0;
		info->argModes   = PointerGetDatum( NULL );
		info->argNames   = PointerGetDatum( NULL );
		info->langOid    = getLangOid( PACKAGE_LANG );
		info->isFunc     = TRUE;	/* FIXME: is an initializer a function or procedure - does it matter??? */
		info->fqName     = makeFullName( info->schemaOid, info->packageOid, NameStr( pkgForm->pkgname ));
		info->returnsSet = FALSE;

		ReleaseSysCache( pkgTup );

		return( TRUE );
	}
	else
	{
		HeapTuple 		  pkgElemTup  = SearchSysCache( PKGELEMENTSOID, ObjectIdGetDatum( info->targetOid ), 0, 0, 0 );
		HeapTuple		  pkgTup;
		Form_pkgelements  pkgElemForm;
		Form_edb_package  pkgForm;

		/* 
		 * The user gave us two OID's, but they differ - assume the first is a package OID
		 * and the second is package element OID
		 */

		if( !HeapTupleIsValid( pkgElemTup ))
			elog( ERROR, "cache lookup failed for function %u", info->targetOid );

		pkgElemForm = (Form_pkgelements) GETSTRUCT( pkgElemTup );
		
		if(( pkgForm = getPackageForm( pkgElemForm->packageoid, &pkgTup )) ==  NULL )
			elog( ERROR, "cache lookup failed for package %u", pkgElemForm->packageoid );

		completePackageTarget( info, pkgElemTup, pkgForm->pkgnamespace, PKGELEMENTSOID );

		ReleaseSysCache( pkgTup );
		ReleaseSysCache( pkgElemTup );

		return( TRUE );
	}
}

/*******************************************************************************
 * getPkgTarget()
 *
 *	This helper function is called (indirectly) by pldbg_get_target_info() to 
 *	retrieve information about a function or procedure that defined within a 
 *  package.
 *
 *  The caller already parsed out the raw name and decided that the user did
 *  include a package name.
 *
 *	This function is much less complex than getGlobalTarget() because we have 
 *  already identified the schema that contains the requested package.
 *
 *	If we find a match, we fill in the targetInfo structure with information
 *  about the target that we've identified.
 */

static bool getPkgTarget( targetInfo * info )
{
	int			  i;

	info->targetOid = InvalidOid;

	/* Search syscache by name only */
	info->catlist = SearchSysCacheList( PKGELEMENTS, 3,
								  ObjectIdGetDatum( info->packageOid ),
								  CStringGetDatum( info->funcName ),
								  CharGetDatum((( info->isFunc ) ? FUNCTION_ELT : PROCEDURE_ELT )),
								  0 );	

	/*
	 *	info->catlist is a list of tuples that match the target name
	 *
	 *	Now try to find a procedure/function that exactly matches
	 *  the caller's argTypes.
	 *
	 *	Note: we don't have to worry about searching through the 
	 *	schema searchpath here - our caller already figured out 
	 *	which package the target belongs to and that package is
	 *  defined within a single namespace
	 */

	if( info->nargs == -1 )
	{
		/* 
		 * The user gave us a function/procedure name, but a complete
		 * signature (that is, he gave us 'foo' instead of 'foo(int)').
		 *
		 * If we found more than one function/procedure with the given
		 * name, it's an error (because we can't disambiguate the name
		 * without the argument types).
		 */

		if( info->catlist->n_members > 1 )
			return( FALSE );
	}

	for( i = 0; i < info->catlist->n_members; i++ )
	{
		HeapTuple				pkgtup  = &info->catlist->members[i]->tuple;
		Form_pkgelements 		pkgform = (Form_pkgelements) GETSTRUCT( pkgtup );

		if( info->nargs != -1 )
		{
			if( pkgform->nargs != info->nargs )
				continue;

			if( memcmp( info->argTypes, pkgform->argtypes.values, info->nargs * sizeof(Oid)) != 0 )
				continue;
		}

		/* Looks like this is the package element that we want. */
		completePackageTarget( info, pkgtup, info->schemaOid, PKGELEMENTS );
	}
	
	return( OidIsValid( info->targetOid ));
}		

/********************************************************************************
 * computeRequiredArgs()
 *
 *	Given an array of default values (defValDatum), this function computes the 
 *	number of required arguments 
 *		required = totalArgumentCount - countOfDefaultValues
 */

static void computeRequiredArgs( targetInfo * info, Datum defValDatum, bool isNull )
{
	if( isNull )
	{
		info->requiredArgs = info->nargs;
		return;
	}
	else
	{
		ArrayType  * argdefvals    = DatumGetArrayTypeP( defValDatum );
		Datum 	   * defvals_array = NULL;
		int 		 ndefvals 	   = 0;
		int			 i;

		deconstruct_array( argdefvals, TEXTOID, -1, false, 'i', &defvals_array, &ndefvals );

		info->requiredArgs = 0;

		for( i = 0; i < ndefvals; ++i )
		{
			char * value = DatumGetCString(DirectFunctionCall1( textout, defvals_array[i] ));

			if( value[0] == '-' )
				info->requiredArgs += 1;

			pfree( value );
		}
	}
}

/*******************************************************************************
 * completeProcTarget()
 *
 *	Given a pg_proc tuple, this function copies the target definition into
 *	the targetInfo structure.
 *
 *	NOTE: we split this code out into a separate function because the PL/pgSQL
 *		  implementation differs from the EDB SPL implementation.
 */

static void completeProcTarget( targetInfo * info, HeapTuple proctup )
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT( proctup );
	bool		 modesNull;
	bool		 namesNull;
	bool		 isFuncNull;
	bool		 defValsNull;
	bool		 allTypesNull;
	Datum		 argModes;
	Datum		 argNames;
	Datum		 argTypes;
	Datum		 isFunc;
	Datum		 defVals;

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
	isFunc 	 = SysCacheGetAttr( PROCNAMEARGSNSP, proctup, Anum_pg_proc_proisfunc,  	   &isFuncNull );
	defVals  = SysCacheGetAttr( PROCNAMEARGSNSP, proctup, Anum_pg_proc_proargdefvals,  &defValsNull );

	info->targetOid  = HeapTupleGetOid( proctup );
	info->schemaOid  = procform->pronamespace;
	info->targetName = pstrdup( NameStr( procform->proname ));
	info->argModes   = modesNull ? PointerGetDatum( NULL ) : (Datum)DatumGetArrayTypePCopy( argModes );
	info->argNames   = namesNull ? PointerGetDatum( NULL ) : (Datum)DatumGetArrayTypePCopy( argNames );
	info->langOid    = procform->prolang;
	info->isFunc     = isFunc;
	info->fqName	 = makeFullName( info->schemaOid, InvalidOid, info->targetName );
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

	/*
	 * Compute the number of required arguments 
	 *
	 *	NOTE: we must call this function after info->nargs is set since 
	 *		  computeRequiredArgs() relies on that value
	 */

	computeRequiredArgs( info, defVals, defValsNull );

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
 * getProcCandidates()
 *
 *	Given a target name (not a complete signature), this function will return a
 *	list of matching pg_proc tuples.
 *
 *	NOTE: we split this code out into a separate function because the PL/pgSQL
 *		  implementation differs from the EDB SPL implementation.
 */

static CatCList * getProcCandidates( targetInfo * info )
{
	return( SearchSysCacheList( PROCNAMEARGSNSP, 2, CStringGetDatum( info->funcName ), BoolGetDatum( info->isFunc ), 0, 0 ));
}
