noinst_LTLIBRARIES = libfs.la

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-test \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-dict \
	-I$(top_srcdir)/src/lib-ssl-iostream \
	-I$(top_srcdir)/src/lib-var-expand \
	-DMODULE_DIR=\""$(moduledir)"\"

libfs_la_SOURCES = \
	fs-api.c \
	fs-dict.c \
	fs-metawrap.c \
	fs-randomfail.c \
	fs-posix.c \
	fs-test.c \
	fs-test-async.c \
	fs-sis.c \
	fs-sis-common.c \
	fs-sis-queue.c \
	fs-wrapper.c \
	istream-fs-file.c \
	istream-fs-stats.c \
	istream-metawrap.c \
	ostream-metawrap.c \
	ostream-cmp.c

headers = \
	fs-api.h \
	fs-api-private.h \
	fs-sis-common.h \
	fs-wrapper.h \
	fs-test.h \
	istream-fs-file.h \
	istream-fs-stats.h \
	istream-metawrap.h \
	ostream-metawrap.h \
	ostream-cmp.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)

noinst_PROGRAMS = $(test_programs)

test_programs = \
	test-fs-metawrap \
	test-fs-posix

test_deps = \
	$(noinst_LTLIBRARIES) \
	../lib-dict/libdict.la \
	../lib-settings/libsettings.la \
	../lib-dns/libdns.la \
	../lib-var-expand/libvar_expand.la \
	../lib-test/libtest.la \
	../lib/liblib.la

test_libs = \
	$(test_deps) \
	$(MODULE_LIBS)

test_fs_metawrap_SOURCES = test-fs-metawrap.c
test_fs_metawrap_LDADD = $(test_libs)
test_fs_metawrap_DEPENDENCIES = $(test_deps)

test_fs_posix_SOURCES = test-fs-posix.c
test_fs_posix_LDADD = $(test_libs)
test_fs_posix_DEPENDENCIES = $(test_deps)

check-local:
	for bin in $(test_programs); do \
	  if ! $(RUN_TEST) ./$$bin; then exit 1; fi; \
	done
