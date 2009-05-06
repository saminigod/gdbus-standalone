include $(top_srcdir)/Makefile.decl

NULL =

INCLUDES =                      \
        -g                      \
        -I$(top_srcdir)         \
	$(GLIB2_CFLAGS)		\
	$(GOBJECT2_CFLAGS)	\
	$(GIO2_CFLAGS)		\
	$(DBUS1_CFLAGS)		\
	$(NULL)

noinst_PROGRAMS = $(TEST_PROGS)
progs_ldadd     =                                       \
	$(GLIB2_LIBS)					\
	$(GOBJECT2_LIBS)				\
	$(GIO2_LIBS)					\
        $(top_builddir)/gdbus/libgdbus-standalone.la	\
	$(NULL)

TEST_PROGS += connection
TEST_PROGS += names
TEST_PROGS += proxy

connection_SOURCES = connection.c sessionbus.c sessionbus.h tests.h tests.c
connection_LDADD = $(progs_ldadd)

names_SOURCES = names.c sessionbus.c sessionbus.h tests.h tests.c
names_LDADD = $(progs_ldadd)

proxy_SOURCES = proxy.c sessionbus.c sessionbus.h tests.h tests.c
proxy_LDADD = $(progs_ldadd)