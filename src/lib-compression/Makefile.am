noinst_LTLIBRARIES = libcompression.la

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-test \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-var-expand \
	$(ZSTD_CFLAGS)

libcompression_la_SOURCES = \
	compression.c \
	istream-decompress.c \
	istream-lz4.c \
	istream-zlib.c \
	istream-bzlib.c \
	istream-zstd.c \
	ostream-lz4.c \
	ostream-zlib.c \
	ostream-bzlib.c \
	ostream-zstd.c
libcompression_la_LIBADD = \
	$(COMPRESS_LIBS)

pkginc_libdir = $(pkgincludedir)
pkginc_lib_HEADERS = \
	compression.h \
	iostream-lz4.h \
	istream-zlib.h \
	ostream-zlib.h

noinst_HEADERS = \
	iostream-zstd-private.h

pkglib_LTLIBRARIES = libdovecot-compression.la
libdovecot_compression_la_SOURCES =
libdovecot_compression_la_LIBADD = libcompression.la $(LIBDOVECOT) $(COMPRESS_LIBS)
libdovecot_compression_la_DEPENDENCIES = libcompression.la $(LIBDOVECOT_DEPS)
libdovecot_compression_la_LDFLAGS = -export-dynamic

test_programs = \
	test-compression

noinst_PROGRAMS = $(test_programs) bench-compression

test_libs = \
	$(noinst_LTLIBRARIES) \
	../lib-dovecot/libdovecot.la

test_deps = $(test_libs)

test_compression_SOURCES = test-compression.c
test_compression_LDADD = $(test_libs)
test_compression_DEPENDENCIES = $(test_deps)

bench_compression_SOURCES = bench-compression.c
bench_compression_LDADD = $(test_libs)
bench_compression_DEPENDENCIES = $(test_deps)

check-local:
	for bin in $(test_programs); do \
	  if ! $(RUN_TEST) ./$$bin; then exit 1; fi; \
	done
