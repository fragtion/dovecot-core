AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-var-expand \
	-I$(top_srcdir)/src/plugins/notify

NOPLUGIN_LDFLAGS =
lib25_mesh_plugin_la_LDFLAGS = -module -avoid-version

module_LTLIBRARIES = \
	lib25_mesh_plugin.la

lib25_mesh_plugin_la_SOURCES = \
	mesh-plugin.c

noinst_HEADERS = \
	mesh-plugin.h
