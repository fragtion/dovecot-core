pkglibexecdir = $(libexecdir)/dovecot

pkglibexec_PROGRAMS = mesh

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-master \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-auth-client \
	-I$(top_srcdir)/src/lib-ssl-iostream \
	-I$(top_srcdir)/src/lib-http \
	-I$(top_srcdir)/src/lib-json \
	-I$(top_srcdir)/src/lib-doveadm \
	-I$(top_srcdir)/src/lib-var-expand \
	$(BINARY_CFLAGS)

mesh_LDADD = $(LIBDOVECOT) \
	$(BINARY_LDFLAGS)

mesh_DEPENDENCIES = $(LIBDOVECOT_DEPS)

mesh_SOURCES = \
	main.c \
	mesh-conn.c \
	mesh-http.c \
	mesh-peer.c \
	mesh-queue.c \
	mesh-settings.c \
	mesh-state.c \
	mesh-status.c \
	mesh-sync.c \
	mesh-ui.c \
	mesh-verify.c

nodist_mesh_SOURCES = \
	mesh-ui-data.c

UI_FILES = \
	$(srcdir)/ui/index.html \
	$(srcdir)/ui/mesh.css \
	$(srcdir)/ui/mesh.js

mesh-ui-data.c: $(UI_FILES) $(srcdir)/mesh-ui-gen.pl
	$(AM_V_GEN)$(PERL) $(srcdir)/mesh-ui-gen.pl $(UI_FILES) > $@.tmp && mv $@.tmp $@

BUILT_SOURCES = mesh-ui-data.c
CLEANFILES = mesh-ui-data.c

# Lua passdb for LMTP delivery affinity, see the file
pkgdata_DATA = mesh-route.lua

noinst_HEADERS = \
	mesh-common.h \
	mesh-settings.h \
	mesh-ui.h

EXTRA_DIST = \
	HANDOFF.md \
	README.md \
	mesh-route.lua \
	mesh-ui-gen.pl \
	$(UI_FILES) \
	harness/cluster.py \
	harness/phase0.py \
	harness/phase1.py \
	harness/workload.py
