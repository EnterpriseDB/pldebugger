################################################################################
##
##	This Makefile will build the following targets:
##
## 	  plugin_debugger.so	 - a PL/pgSQL interpreter plugin, this is the server
##                             side of the PL/pgSQL debugger
##
##	  pldbgapi.so		     - a set of functions that form an SQL interface to
##							   the PL/pgSQL debugger
##
##    plugin_tracer.so	     - a PL/pgSQL interpreter plugin that can trace the 
##							   code path (and variable values) through the 
##							   PL/pgSQL code executed by a backend
##
##	  plugin_profiler.so	 - a PL/pgSQL plugin that profiles the execution
##							   of all PL/pgSQL code executed by a backend
##
##  Note that this Makefile will build shared-objects with the correct suffix
##  (.so, .dll, or .sl) depending on the platform that you are compiling on,
##
##	Note: $(DLSUFFIX) is set to .so, .dll, or .sl, depending on what kind
##  of platform you are compiling on.  So plugin_debugger$(DLSUFFIX) is 
##  equivalent to plugin_debugger.dll on Windows and plugin_debugger.so on 
##  Linux
##
##  The pldbgapi.so shared-object should be installed in $libdir, the
##  plugin shared-objects should be installed in $libdir/plugins
##
## Copyright (c) 2004-2007 EnterpriseDB Corporation. All Rights Reserved.
##
## Licensed under the Artistic License, see 
##		http://www.opensource.org/licenses/artistic-license.php
## for full details

SHAREDLIBS      = pldbgapi targetinfo
PLUGINS         = plugin_debugger plugin_profiler # plugin_tracer
INSTALL_scripts = pldbgapi.sql
DOCS		    = README.pldebugger README.plprofiler

subdir       = contrib/debugger
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk

ifeq ($(PORTNAME), win32)
SHLIB_LINK += -lwsock32
SYMLINKCMD  = cp
else
SYMLINKCMD  = ln -s
endif

ifeq ($(PORTNAME), win32)
override CFLAGS += $(CFLAGS_SL) -I$(top_builddir)/src/pl/plpgsql/src
else
override CFLAGS += $(CFLAGS_SL) 
endif

SHLIB_LINK      += $(BE_DLLLIBS)

all:	$(addsuffix $(DLSUFFIX), $(SHAREDLIBS)) $(addsuffix $(DLSUFFIX), $(PLUGINS)) $(INSTALL_scripts)

install: all installdirs
	@for file in $(addsuffix $(DLSUFFIX), $(SHAREDLIBS)); do \
	  echo "$(INSTALL_SHLIB) $$file '$(DESTDIR)$(pkglibdir)'"; \
	  $(INSTALL_SHLIB) $$file '$(DESTDIR)$(pkglibdir)'; \
	done
	@for file in $(addsuffix $(DLSUFFIX), $(PLUGINS)); do \
	  echo "$(INSTALL_SHLIB) $$file '$(DESTDIR)$(pkglibdir)/plugins'"; \
	  $(INSTALL_SHLIB) $$file '$(DESTDIR)$(pkglibdir)/plugins'; \
	done
	@for file in $(addprefix $(srcdir)/, $(INSTALL_scripts)); do \
	  echo "$(INSTALL_DATA) $$file '$(DESTDIR)$(datadir)/contrib'"; \
	  $(INSTALL_DATA) $$file '$(DESTDIR)$(datadir)/contrib'; \
	done

installdirs:
	# MKDIR_P replace mkinstalldirs in PG8.5+
	$(MKDIR_P)$(mkinstalldirs) $(DESTDIR)$(pkglibdir)
	$(MKDIR_P)$(mkinstalldirs) $(DESTDIR)$(pkglibdir)/plugins
	$(MKDIR_P)$(mkinstalldirs) $(DESTDIR)$(datadir)/contrib

clean:
	@for file in $(addsuffix $(DLSUFFIX), $(SHAREDLIBS)); do \
		echo "  rm $$file"; \
		rm -f $$file; \
	done
	@for file in $(addsuffix $(DLSUFFIX), $(PLUGINS)); do \
		echo "  rm $$file"; \
		rm -f $$file; \
	done

################################################################################
## Rules for making the debugger plugin.
##
## 

plugin_debugger$(DLSUFFIX): 	CFLAGS += -DINCLUDE_PACKAGE_SUPPORT=0 -I$(top_builddir)/src/pl/plpgsql/src

################################################################################
## Rules for making the pldbgapi
##
##   NOTE: On Windows, pldbgapi.dll must link against plugin_debugger.dll 
##		   (because pldbgapi.dll needs the global breakpoint code defined 
##		   in plugin_debugger.dll).  
##
##	 	   Ideally, we could just define a target-specific variable and 
##		   an explicit pre-requisite to handle this:
##			pldbgapi$(DLSUFFIX):  SHLIB_LINK += plugin_debugger$(DLSUFFIX)
##		    pldbgapi$(DLSUFFIX):  plugin_debugger$(DLSUFFIX)
##
##		   But GNU Make propogates the target-specific variable to the 
##		   the prerequisite which would force plugin_debugger.dll to 
##		   link against itself
##   

ifeq ($(PORTNAME), win32)

pldbgapi$(DLSUFFIX):	pldbgapi.o plugin_debugger$(DLSUFFIX)
	$(DLLTOOL) --export-all --output-def $*.def $<
	$(DLLWRAP) -o $@ --def $*.def $< $(SHLIB_LINK) plugin_debugger$(DLSUFFIX)
	rm -r $*.def

endif

################################################################################
##
##  NOTE: On OS X (Darwin), we ignore undefined references in pldbgapi. These
##                are actually in plugin_debugger.so and will be present at
##                runtime. Someone with more ld/gcc foo than I may be able to
##                figure out a nicer way of telling the linker these symbols
##                really do exist!! -Dave.

ifeq ($(PORTNAME), darwin)

pldbgapi$(DLSUFFIX):   CFLAGS += -undefined dynamic_lookup

endif


################################################################################
## Rules for making the profiler plugins
##
##

plugin_profiler$(DLSUFFIX):     CFLAGS += -DINCLUDE_PACKAGE_SUPPORT=0 -I$(top_builddir)/src/pl/plpgsql/src
#plugin_tracer$(DLSUFFIX):       CFLAGS += -DINCLUDE_PACKAGE_SUPPORT=0 -I$(top_builddir)/src/pl/plpgsql/src


#################################################################################
## Convenience targets - these are here just in case you forget which target you 
##                       really want to build
##

plugin_debugger: 		plugin_debugger$(DLSUFFIX)
	@echo "complete (you probably wanted to make 'plugin_debugger.so' instead)"

plugin_tracer: 			plugin_tracer$(DLSUFFIX)
	@echo "complete (you probably wanted to 'make plugin_tracer.so' instead)"

plugin_profiler:	 	plugin_profiler$(DLSUFFIX)
	@echo "complete (you probably wanted to 'make plugin_profiler.so' instead)"

pldbgapi: 				pldbgapi$(DLSUFFIX)
	@echo "complete (you probably wanted to 'make pldbgapi.so' instead)"
