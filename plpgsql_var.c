#include "plpgsql_var.h"

static void plpgsql_fulfill_promise(PLpgSQL_execstate *estate, PLpgSQL_var *var);
static HeapTuple 	make_tuple_from_row(PLpgSQL_execstate *estate, PLpgSQL_row *row, TupleDesc tupdesc);

static void 		assign_text_var(PLpgSQL_execstate *estate, PLpgSQL_var *var, const char *str);

static void			assign_simple_var(PLpgSQL_execstate *estate, PLpgSQL_var *var, Datum newvalue, bool isnull, bool freeable);

void exec_eval_datum(PLpgSQL_execstate *estate,
				PLpgSQL_datum *datum,
				Oid *typeid,
				int32 *typetypmod,
				Datum *value,
				bool *isnull)
{
	MemoryContext oldcontext;

	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_PROMISE:
			/* fulfill promise if needed, then handle like regular var */
			plpgsql_fulfill_promise(estate, (PLpgSQL_var *) datum);

			/* FALL THRU */

		case PLPGSQL_DTYPE_VAR:
			{
				PLpgSQL_var *var = (PLpgSQL_var *) datum;

				*typeid = var->datatype->typoid;
				*typetypmod = var->datatype->atttypmod;
				*value = var->value;
				*isnull = var->isnull;
				break;
			}

		case PLPGSQL_DTYPE_ROW:
			{
				PLpgSQL_row *row = (PLpgSQL_row *) datum;
				HeapTuple	tup;

				/* We get here if there are multiple OUT parameters */
				if (!row->rowtupdesc)	/* should not happen */
					elog(ERROR, "row variable has no tupdesc");
				/* Make sure we have a valid type/typmod setting */
				BlessTupleDesc(row->rowtupdesc);


				oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
				tup = make_tuple_from_row(estate, row, row->rowtupdesc);
				if (tup == NULL)	/* should not happen */
					elog(ERROR, "row not compatible with its own tupdesc");
				*typeid = row->rowtupdesc->tdtypeid;
				*typetypmod = row->rowtupdesc->tdtypmod;
				*value = HeapTupleGetDatum(tup);
				*isnull = false;
				MemoryContextSwitchTo(oldcontext);
				break;
			}

		case PLPGSQL_DTYPE_REC:
			{
				PLpgSQL_rec *rec = (PLpgSQL_rec *) datum;

				if (rec->erh == NULL)
				{
					/* Treat uninstantiated record as a simple NULL */
					*value = (Datum) 0;
					*isnull = true;
					/* Report variable's declared type */
					*typeid = rec->rectypeid;
					*typetypmod = -1;
				}
				else
				{
					if (ExpandedRecordIsEmpty(rec->erh))
					{
						/* Empty record is also a NULL */
						*value = (Datum) 0;
						*isnull = true;
					}
					else
					{
						*value = ExpandedRecordGetDatum(rec->erh);
						*isnull = false;
					}
					if (rec->rectypeid != RECORDOID)
					{
						/* Report variable's declared type, if not RECORD */
						*typeid = rec->rectypeid;
						*typetypmod = -1;
					}
					else
					{
						/* Report record's actual type if declared RECORD */
						*typeid = rec->erh->er_typeid;
						*typetypmod = rec->erh->er_typmod;
					}
				}
				break;
			}

		case PLPGSQL_DTYPE_RECFIELD:
			{
				PLpgSQL_recfield *recfield = (PLpgSQL_recfield *) datum;
				PLpgSQL_rec *rec;
				ExpandedRecordHeader *erh;

				rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);
				erh = rec->erh;

				/*
				 * If record variable is NULL, instantiate it if it has a
				 * named composite type, else complain.  (This won't change
				 * the logical state of the record: it's still NULL.)
				 */
				if (erh == NULL)
				{
				    /* Treat uninstantiated record as a simple NULL */
				    *value = (Datum) 0;
				    *isnull = true;
				    /* Report variable's declared type */
				    *typeid = (Oid) 0; //rec->rectypeid;
				    *typetypmod = -1;
				    break;
					//instantiate_empty_record_variable(estate, rec);
					//erh = rec->erh;
				}

				/*
				 * Look up the field's properties if we have not already, or
				 * if the tuple descriptor ID changed since last time.
				 */
				if (unlikely(recfield->rectupledescid != erh->er_tupdesc_id))
				{
					if (!expanded_record_lookup_field(erh,
													  recfield->fieldname,
													  &recfield->finfo))
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("record \"%s\" has no field \"%s\"",
										rec->refname, recfield->fieldname)));
					recfield->rectupledescid = erh->er_tupdesc_id;
				}

				/* Report type data. */
				*typeid = recfield->finfo.ftypeid;
				*typetypmod = recfield->finfo.ftypmod;

				/* And fetch the field value. */
				*value = expanded_record_get_field(erh,
												   recfield->finfo.fnumber,
												   isnull);
				break;
			}

		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
	}
}

char * convert_value_to_string(PLpgSQL_execstate *estate, Datum value, Oid valtype)
{
	char	   *result;
	MemoryContext oldcontext;
	Oid			typoutput;
	bool		typIsVarlena;

	oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
	getTypeOutputInfo(valtype, &typoutput, &typIsVarlena);
	result = OidOutputFunctionCall(typoutput, value);
	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * If the variable has an armed "promise", compute the promised value
 * and assign it to the variable.
 * The assignment automatically disarms the promise.
 */
static void plpgsql_fulfill_promise(PLpgSQL_execstate *estate,
						PLpgSQL_var *var)
{
	MemoryContext oldcontext;

	if (var->promise == PLPGSQL_PROMISE_NONE)
		return;					/* nothing to do */

	/*
	 * This will typically be invoked in a short-lived context such as the
	 * mcontext.  We must create variable values in the estate's datum
	 * context.  This quick-and-dirty solution risks leaking some additional
	 * cruft there, but since any one promise is honored at most once per
	 * function call, it's probably not worth being more careful.
	 */
	oldcontext = MemoryContextSwitchTo(estate->datum_context);

	switch (var->promise)
	{
		case PLPGSQL_PROMISE_TG_NAME:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			assign_simple_var(estate, var,
							  DirectFunctionCall1(namein,
												  CStringGetDatum(estate->trigdata->tg_trigger->tgname)),
							  false, true);
			break;

		case PLPGSQL_PROMISE_TG_WHEN:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			if (TRIGGER_FIRED_BEFORE(estate->trigdata->tg_event))
				assign_text_var(estate, var, "BEFORE");
			else if (TRIGGER_FIRED_AFTER(estate->trigdata->tg_event))
				assign_text_var(estate, var, "AFTER");
			else if (TRIGGER_FIRED_INSTEAD(estate->trigdata->tg_event))
				assign_text_var(estate, var, "INSTEAD OF");
			else
				elog(ERROR, "unrecognized trigger execution time: not BEFORE, AFTER, or INSTEAD OF");
			break;

		case PLPGSQL_PROMISE_TG_LEVEL:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			if (TRIGGER_FIRED_FOR_ROW(estate->trigdata->tg_event))
				assign_text_var(estate, var, "ROW");
			else if (TRIGGER_FIRED_FOR_STATEMENT(estate->trigdata->tg_event))
				assign_text_var(estate, var, "STATEMENT");
			else
				elog(ERROR, "unrecognized trigger event type: not ROW or STATEMENT");
			break;

		case PLPGSQL_PROMISE_TG_OP:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			if (TRIGGER_FIRED_BY_INSERT(estate->trigdata->tg_event))
				assign_text_var(estate, var, "INSERT");
			else if (TRIGGER_FIRED_BY_UPDATE(estate->trigdata->tg_event))
				assign_text_var(estate, var, "UPDATE");
			else if (TRIGGER_FIRED_BY_DELETE(estate->trigdata->tg_event))
				assign_text_var(estate, var, "DELETE");
			else if (TRIGGER_FIRED_BY_TRUNCATE(estate->trigdata->tg_event))
				assign_text_var(estate, var, "TRUNCATE");
			else
				elog(ERROR, "unrecognized trigger action: not INSERT, DELETE, UPDATE, or TRUNCATE");
			break;

		case PLPGSQL_PROMISE_TG_RELID:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			assign_simple_var(estate, var,
							  ObjectIdGetDatum(estate->trigdata->tg_relation->rd_id),
							  false, false);
			break;

		case PLPGSQL_PROMISE_TG_TABLE_NAME:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			assign_simple_var(estate, var,
							  DirectFunctionCall1(namein,
												  CStringGetDatum(RelationGetRelationName(estate->trigdata->tg_relation))),
							  false, true);
			break;

		case PLPGSQL_PROMISE_TG_TABLE_SCHEMA:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			assign_simple_var(estate, var,
							  DirectFunctionCall1(namein,
												  CStringGetDatum(get_namespace_name(RelationGetNamespace(estate->trigdata->tg_relation)))),
							  false, true);
			break;

		case PLPGSQL_PROMISE_TG_NARGS:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			assign_simple_var(estate, var,
							  Int16GetDatum(estate->trigdata->tg_trigger->tgnargs),
							  false, false);
			break;

		case PLPGSQL_PROMISE_TG_ARGV:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			if (estate->trigdata->tg_trigger->tgnargs > 0)
			{
				/*
				 * For historical reasons, tg_argv[] subscripts start at zero
				 * not one.  So we can't use construct_array().
				 */
				int			nelems = estate->trigdata->tg_trigger->tgnargs;
				Datum	   *elems;
				int			dims[1];
				int			lbs[1];
				int			i;

				elems = palloc(sizeof(Datum) * nelems);
				for (i = 0; i < nelems; i++)
					elems[i] = CStringGetTextDatum(estate->trigdata->tg_trigger->tgargs[i]);
				dims[0] = nelems;
				lbs[0] = 0;

				assign_simple_var(estate, var,
								  PointerGetDatum(construct_md_array(elems, NULL,
																	 1, dims, lbs,
																	 TEXTOID,
																	 -1, false, TYPALIGN_INT)),
								  false, true);
			}
			else
			{
				assign_simple_var(estate, var, (Datum) 0, true, false);
			}
			break;

		case PLPGSQL_PROMISE_TG_EVENT:
			if (estate->evtrigdata == NULL)
				elog(ERROR, "event trigger promise is not in an event trigger function");
			assign_text_var(estate, var, estate->evtrigdata->event);
			break;

		case PLPGSQL_PROMISE_TG_TAG:
			if (estate->evtrigdata == NULL)
				elog(ERROR, "event trigger promise is not in an event trigger function");
			assign_text_var(estate, var, GetCommandTagName(estate->evtrigdata->tag));
			break;

		default:
			elog(ERROR, "unrecognized promise type: %d", var->promise);
	}

	MemoryContextSwitchTo(oldcontext);
}



static HeapTuple make_tuple_from_row(PLpgSQL_execstate *estate, PLpgSQL_row *row, TupleDesc tupdesc)
{
	int			natts = tupdesc->natts;
	HeapTuple	tuple;
	Datum	   *dvalues;
	bool	   *nulls;
	int			i;

	if (natts != row->nfields)
		return NULL;

	dvalues = (Datum *) eval_mcontext_alloc0(estate, natts * sizeof(Datum));
	nulls = (bool *) eval_mcontext_alloc(estate, natts * sizeof(bool));

	for (i = 0; i < natts; i++)
	{
		Oid			fieldtypeid;
		int32		fieldtypmod;

		if (TupleDescAttr(tupdesc, i)->attisdropped)
		{
			nulls[i] = true;	/* leave the column as null */
			continue;
		}

		exec_eval_datum(estate, estate->datums[row->varnos[i]],
						&fieldtypeid, &fieldtypmod,
						&dvalues[i], &nulls[i]);
		if (fieldtypeid != TupleDescAttr(tupdesc, i)->atttypid)
			return NULL;
		/* XXX should we insist on typmod match, too? */
	}

	tuple = heap_form_tuple(tupdesc, dvalues, nulls);

	return tuple;
}

static void assign_text_var(PLpgSQL_execstate *estate, PLpgSQL_var *var, const char *str)
{
	assign_simple_var(estate, var, CStringGetTextDatum(str), false, true);
}

static void assign_simple_var(PLpgSQL_execstate *estate, PLpgSQL_var *var,
				  Datum newvalue, bool isnull, bool freeable)
{
	Assert(var->dtype == PLPGSQL_DTYPE_VAR ||
		   var->dtype == PLPGSQL_DTYPE_PROMISE);

	/*
	 * In non-atomic contexts, we do not want to store TOAST pointers in
	 * variables, because such pointers might become stale after a commit.
	 * Forcibly detoast in such cases.  We don't want to detoast (flatten)
	 * expanded objects, however; those should be OK across a transaction
	 * boundary since they're just memory-resident objects.  (Elsewhere in
	 * this module, operations on expanded records likewise need to request
	 * detoasting of record fields when !estate->atomic.  Expanded arrays are
	 * not a problem since all array entries are always detoasted.)
	 */
	if (!estate->atomic && !isnull && var->datatype->typlen == -1 &&
		VARATT_IS_EXTERNAL_NON_EXPANDED(DatumGetPointer(newvalue)))
	{
		MemoryContext oldcxt;
		Datum		detoasted;

		/*
		 * Do the detoasting in the eval_mcontext to avoid long-term leakage
		 * of whatever memory toast fetching might leak.  Then we have to copy
		 * the detoasted datum to the function's main context, which is a
		 * pain, but there's little choice.
		 */
		oldcxt = MemoryContextSwitchTo(get_eval_mcontext(estate));
		detoasted = PointerGetDatum(detoast_external_attr((struct varlena *) DatumGetPointer(newvalue)));
		MemoryContextSwitchTo(oldcxt);
		/* Now's a good time to not leak the input value if it's freeable */
		if (freeable)
			pfree(DatumGetPointer(newvalue));
		/* Once we copy the value, it's definitely freeable */
		newvalue = datumCopy(detoasted, false, -1);
		freeable = true;
		/* Can't clean up eval_mcontext here, but it'll happen before long */
	}

	/* Free the old value if needed */
	if (var->freeval)
	{
		if (DatumIsReadWriteExpandedObject(var->value,
										   var->isnull,
										   var->datatype->typlen))
			DeleteExpandedObject(var->value);
		else
			pfree(DatumGetPointer(var->value));
	}
	/* Assign new value to datum */
	var->value = newvalue;
	var->isnull = isnull;
	var->freeval = freeable;

	/*
	 * If it's a promise variable, then either we just assigned the promised
	 * value, or the user explicitly assigned an overriding value.  Either
	 * way, cancel the promise.
	 */
	var->promise = PLPGSQL_PROMISE_NONE;
}
