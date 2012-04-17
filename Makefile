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

EXTENSION  = pldbgapi
MODULE_big = pldbgapi

# files for building pldbgapi.so
OBJS	   = pldbgapi.o targetinfo.o

PLUGINS    = plugin_debugger
DATA       = pldbgapi--1.0.sql pldbgapi--unpackaged--1.0.sql
DOCS	   = README.pldebugger

# PGXS build needs PostgreSQL 9.2 or later. Earlier versions didn't install
# plpgsql.h, so you needed the full source tree to access it.
ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir       = contrib/pldebugger
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

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

all:	$(addsuffix $(DLSUFFIX), $(PLUGINS))

install: all installdirs installdir-plugins install-plugins

clean: clean-plugins

uninstall: uninstall-plugins

install-plugins: installdir-plugins $(addsuffix $(DLSUFFIX), $(PLUGINS))
	$(INSTALL_SHLIB) $(addsuffix $(DLSUFFIX), $(PLUGINS)) '$(DESTDIR)$(pkglibdir)/plugins/'

clean-plugins:
	rm -f $(addsuffix $(DLSUFFIX), $(PLUGINS)) $(addsuffix .o, $(PLUGINS))

uninstall-plugins:
	rm -f $(addprefix '$(DESTDIR)$(pkglibdir)'/plugins/, $(addsuffix $(DLSUFFIX), $(PLUGINS)))

# MKDIR_P replaced mkinstalldirs in PG8.5+
installdir-plugins:
	$(MKDIR_P)$(mkinstalldirs) '$(DESTDIR)$(pkglibdir)/plugins'

################################################################################
## Rules for making the debugger plugin.
##
## 

plugin_debugger$(DLSUFFIX): 	CFLAGS += -I$(top_builddir)/src/pl/plpgsql/src


################################################################################
## If we're building against EnterpriseDB's Advanced Server, also build a
## debugger module for the SPL language. It's pretty much the same as the
## PL/pgSQL one, but the structs have some extra fields and are thus not
## binary-compatible. We make a copy of the .c file, and pass the
## INCLUDE_PACKAGE_SUPPORT=1 flag to compile it against SPL instead of PL/pgSQL
##
## To enable this, you need to run make as "make INCLUDE_PACKAGE_SUPPORT=1"
## 
ifdef INCLUDE_PACKAGE_SUPPORT
plugin_spl_debugger.c:  plugin_debugger.c
	$(SYMLINKCMD) $(module_srcdir)plugin_debugger.c plugin_spl_debugger.c

plugin_spl_debugger$(DLSUFFIX): 	CFLAGS += -DINCLUDE_PACKAGE_SUPPORT=1 -I$(top_builddir)/src/pl/edb-spl/src
endif

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
