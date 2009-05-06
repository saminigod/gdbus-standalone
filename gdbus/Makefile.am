include $(top_srcdir)/Makefile.decl

NULL =

SUBDIRS=. tests

gdbus.def: gdbus.symbols
	(echo -e EXPORTS; $(CPP) -P -DINCLUDE_VARIABLES -DINCLUDE_INTERNAL_SYMBOLS -DG_OS_WIN32 -DALL_FILES - <$(srcdir)/gdbus.symbols | sed -e '/^$$/d' -e 's/^/	/' -e 's/G_GNUC_[^ ]*//g' | sort) > gdbus.def.tmp && \
	  mv gdbus.def.tmp gdbus.def

gdbusalias.h: gdbus.symbols
	$(PERL) $(srcdir)/makegdbusalias.pl < $(srcdir)/gdbus.symbols > gdbusalias.h.tmp && \
	  mv gdbusalias.h.tmp gdbusalias.h

gdbusaliasdef.c: gdbus.symbols
	$(PERL) $(srcdir)/makegdbusalias.pl -def < $(srcdir)/gdbus.symbols > gdbusaliasdef.c.tmp && \
	  mv gdbusaliasdef.c.tmp gdbusaliasdef.c

TESTS = abicheck.sh pltcheck.sh

AM_CPPFLAGS = \
	-DG_LOG_DOMAIN=\"GLib-DBus-Standalone\"			\
	-I$(top_builddir)					\
	-I$(top_srcdir)						\
	-DG_DISABLE_DEPRECATED					\
	-DG_DBUS_COMPILATION					\
	-DG_DBUS_I_UNDERSTAND_THAT_ABI_AND_API_IS_UNSTABLE	\
	$(NULL)

lib_LTLIBRARIES = libgdbus-standalone.la

marshal_sources = \
        gdbus-marshal.h	\
        gdbus-marshal.c	\
	$(NULL)

glib_genmarshal=glib-genmarshal

gdbus-marshal.h: gdbus-marshal.list
	$(glib_genmarshal) --prefix=_gdbus_marshal $(srcdir)/gdbus-marshal.list --header --internal > $@.tmp && \
	  mv $@.tmp $@

gdbus-marshal.c: gdbus-marshal.h gdbus-marshal.list
	(echo "#include \"gdbus-marshal.h\""; \
	$(glib_genmarshal) --prefix=_gdbus_marshal $(srcdir)/gdbus-marshal.list --body --internal) > $@.tmp && \
	  mv $@.tmp $@

gdbus_headers =				\
	gdbus.h				\
	gdbusenums.h			\
	gdbustypes.h			\
	gdbuserror.h			\
	gdbusconnection.h		\
	gbusnameowner.h			\
	gbusnamewatcher.h		\
	gdbusnamewatching.h		\
	gdbusnameowning.h		\
	gdbusproxy.h			\
	gdbusstructure.h		\
	gdbusvariant.h			\
	$(NULL)

libgdbus_standalone_la_SOURCES =					\
	gdbus.h								\
	gdbustypes.h							\
	gdbusenumtypes.h						\
	gdbus-marshal.h 		gdbus-marshal.c 		\
	gdbusenumtypes.h		gdbusenumtypes.c		\
	gdbuserror-lowlevel.h						\
	gdbuserror.h			gdbuserror.c			\
	gdbusmainloop-lowlevel.h	gdbusmainloop.c			\
	gdbusctypemapping.h-lowlevel.h					\
	gdbusctypemapping.h		gdbusctypemapping.c		\
	gdbusconnection.h-lowlevel.h					\
	gdbusconnection.h		gdbusconnection.c		\
	gbusnameowner.h			gbusnameowner.c			\
	gbusnamewatcher.h		gbusnamewatcher.c		\
	gdbusnamewatching.h		gdbusnamewatching.c		\
	gdbusnameowning.h		gdbusnameowning.c		\
	gdbusproxy.h			gdbusproxy.c			\
	gdbusstructure.h		gdbusstructure.c		\
	gdbusvariant.h			gdbusvariant.c			\
	gdbusprivate.h			gdbusprivate.c			\
	gdbusalias.h 							\
	gdbusaliasdef.c							\
	$(NULL)

$(libgdbus_standalone_la_OBJECTS): $(marshal_sources)

libgdbus_standalone_la_CFLAGS = 			\
	$(GLIB2_CFLAGS)					\
	$(GOBJECT2_CFLAGS)				\
	$(GIO2_CFLAGS)					\
	$(DBUS1_CFLAGS)					\
	$(NULL)

libgdbus_standalone_la_LIBADD = 			\
	$(GLIB2_LIBS)					\
	$(GOBJECT2_LIBS)				\
	$(GIO2_LIBS)					\
	$(DBUS1_LIBS)					\
	$(NULL)
install-def-file:
uninstall-def-file:

export_symbols = -export-symbols-regex '^g_.*'

install-data-local: install-ms-lib install-def-file

uninstall-local: uninstall-ms-lib uninstall-def-file

LT_CURRENT=0
LT_REVISION=0
LT_AGE=0

libgdbus_standalone_la_LDFLAGS = \
	-version-info $(LT_CURRENT):$(LT_REVISION):$(LT_AGE) \
	-export-dynamic $(no_undefined) $(export_symbols)

libgdbus_standalone_la_DEPENDENCIES = $(gdbus_def)

gdbusincludedir=$(includedir)/gdbus-standalone
gdbusinclude_HEADERS = 			\
	$(gdbus_headers)		\
	gdbusenumtypes.h		\
	$(NULL)

gdbuslowlevel_headers = 		\
	gdbus-lowlevel.h		\
	gdbuserror-lowlevel.h		\
	gdbusconnection-lowlevel.h	\
	gdbusmainloop-lowlevel.h	\
	gdbusctypemapping-lowlevel.h	\
	$(NULL)

gdbuslowlevelincludedir=$(includedir)/dbus-1.0/gdbus-lowlevel-standalone/gdbus
gdbuslowlevelinclude_HEADERS = 			\
		$(gdbuslowlevel_headers)	\
		$(NULL)

# these sources (also mentioned above) are generated.
BUILT_SOURCES = 			\
	gdbus-marshal.h 		\
	gdbus-marshal.c 		\
	gdbusalias.h 			\
	gdbusaliasdef.c			\
	gdbusenumtypes.h		\
	gdbusenumtypes.c		\
	$(NULL)

EXTRA_DIST += 				\
	gdbus-marshal.list		\
	gdbus.symbols			\
	gdbusenumtypes.h.template	\
	gdbusenumtypes.c.template	\
	makegdbusalias.pl		\
	abicheck.sh			\
	pltcheck.sh			\
	$(NULL)

CLEANFILES = 			\
	$(marshal_sources)	\
	$(NULL)

gdbusenumtypes.h: $(gdbus_headers) gdbusenumtypes.h.template Makefile.am
	( top_builddir=`cd $(top_builddir) && pwd`; \
	  cd $(srcdir) && glib-mkenums --template gdbusenumtypes.h.template $(gdbus_headers) ) > \
	    gdbusenumtypes.h.tmp && \
	    mv gdbusenumtypes.h.tmp gdbusenumtypes.h

gdbusenumtypes.c: $(gdbus_headers) gdbusenumtypes.c.template Makefile.am
	( top_builddir=`cd $(top_builddir) && pwd`; \
	  cd $(srcdir) && glib-mkenums --template gdbusenumtypes.c.template $(gdbus_headers) ) > \
	    gdbusenumtypes.c.tmp && mv gdbusenumtypes.c.tmp gdbusenumtypes.c

noinst_PROGRAMS =
noinst_PROGRAMS += example-own-name
noinst_PROGRAMS += example-watch-name

example_watch_name_SOURCES = example-watch-name.c
example_watch_name_CFLAGS  = $(GLIB2_CFLAGS) $(GOBJECT2_CFLAGS) $(GIO2_CFLAGS)
example_watch_name_LDADD   = libgdbus-standalone.la  $(GLIB2_LIBS) $(GOBJECT2_LIBS) $(GIO2_LIBS)

example_own_name_SOURCES = example-own-name.c
example_own_name_CFLAGS  = $(GLIB2_CFLAGS) $(GOBJECT2_CFLAGS) $(GIO2_CFLAGS)
example_own_name_LDADD   = libgdbus-standalone.la $(GLIB2_LIBS) $(GOBJECT2_LIBS) $(GIO2_LIBS)

clean-local :
	rm -f *~