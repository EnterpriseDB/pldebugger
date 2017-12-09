#ifndef PLDEBUGGER_H
#define PLDEBUGGER_H

#include "globalbp.h"
#include "storage/lwlock.h"

/*
 * We keep one per_session_ctx structure per backend. This structure holds all
 * of the stuff that we need to track from one function call to the next.
 */
typedef struct
{
	bool	 step_into_next_func;	/* Should we step into the next function?				 */
	int		 client_r;				/* Read stream connected to client						 */
	int		 client_w;				/* Write stream connected to client						 */
} per_session_ctx_t;

extern per_session_ctx_t per_session_ctx;


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

extern errorHandlerCtx client_lost;

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

typedef struct
{
	void	(* initialize)(void);
	bool	(* frame_belongs_to_me)(ErrorContextCallback *frame);
	void	(* send_stack_frame)(ErrorContextCallback *frame);
	void	(* send_vars)(ErrorContextCallback *frame);
	void	(* select_frame)(ErrorContextCallback *frame);
	void	(* print_var)(ErrorContextCallback *frame, const char *var_name, int lineno);
	bool	(* do_deposit)(ErrorContextCallback *frame, const char *var_name,
						   int line_number, const char *value);
	Oid		(* get_func_oid)(ErrorContextCallback *frame);
	void	(* send_cur_line)(ErrorContextCallback *frame);
} debugger_language_t;

/* in plugin_debugger.c */
extern void initGlobalBreakpoints(void);
extern bool plugin_debugger_main_loop(void);

extern bool breakAtThisLine( Breakpoint ** dst, eBreakpointScope * scope, Oid funcOid, int lineNumber );
extern bool attach_to_proxy( Breakpoint * breakpoint );
extern void setBreakpoint( char * command );
extern void clearBreakpoint( char * command );
extern bool breakpointsForFunction( Oid funcOid );

extern void	dbg_send( const char *fmt, ... )
#ifdef PG_PRINTF_ATTRIBUTE
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(PG_PRINTF_ATTRIBUTE, 1, 2)))
#endif
;
extern char 	   * dbg_read_str(void);

extern LWLockId  getPLDebuggerLock(void);

/* in plpgsql_debugger.c */
extern void plpgsql_debugger_fini(void);

extern debugger_language_t plpgsql_debugger_lang;
#ifdef INCLUDE_PACKAGE_SUPPORT
extern debugger_language_t spl_debugger_lang;
#endif

#if PG_VERSION_NUM >= 110000
	#ifndef TRUE
		#define TRUE true
	#endif
	#ifndef FALSE
		#define FALSE false
	#endif
#endif

#endif
