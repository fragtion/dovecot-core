noinst_LTLIBRARIES = libimap_client.la

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-test \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-dns-client \
	-I$(top_srcdir)/src/lib-sasl \
	-I$(top_srcdir)/src/lib-ssl-iostream \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-var-expand

libimap_client_la_SOURCES = \
	imapc-client.c \
	imapc-connection.c \
	imapc-msgmap.c \
	imapc-settings.c

headers = \
	imapc-client.h \
	imapc-client-private.h \
	imapc-connection.h \
	imapc-msgmap.h \
	imapc-settings.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)

test_programs = \
	test-imapc-client

noinst_PROGRAMS = $(test_programs)

test_deps = \
	$(noinst_LTLIBRARIES) \
	../lib-ssl-iostream/libssl_iostream.la \
	../lib-sasl/libsasl.la \
	../lib-imap/libimap.la \
	../lib-mail/libmail.la \
	../lib-charset/libcharset.la \
	../lib-dns-client/libdns-client.la  \
	../lib-dns/libdns.la \
	../lib-auth/libauth.la \
	../lib-otp/libotp.la \
	../lib-json/libjson.la \
	../lib-settings/libsettings.la \
	../lib-dns/libdns.la \
	../lib-var-expand/libvar_expand.la \
	../lib-test/libtest.la \
	../lib/liblib.la

test_libs = \
	$(test_deps) \
	$(MODULE_LIBS)

test_imapc_client_SOURCES = test-imapc-client.c
test_imapc_client_LDADD = $(test_libs)
test_imapc_client_DEPENDENCIES = $(test_deps)

check-local:
	for bin in $(test_programs); do \
	  if ! $(RUN_TEST) ./$$bin; then exit 1; fi; \
	done
