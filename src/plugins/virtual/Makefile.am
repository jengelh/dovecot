AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/lib-storage/index \
	-I$(top_srcdir)/src/lib-imap-storage \
	-I$(top_srcdir)/src/lib-var-expand

NOPLUGIN_LDFLAGS =
lib20_virtual_plugin_la_LDFLAGS = -module -avoid-version

module_LTLIBRARIES = \
	lib20_virtual_plugin.la

lib20_virtual_plugin_la_SOURCES = \
	virtual-config.c \
	virtual-list.c \
	virtual-mail.c \
	virtual-plugin.c \
	virtual-search.c \
	virtual-storage.c \
	virtual-save.c \
	virtual-settings.c \
	virtual-sync.c \
	virtual-transaction.c

noinst_HEADERS = \
	virtual-plugin.h \
	virtual-settings.h \
	virtual-storage.h \
	virtual-transaction.h
