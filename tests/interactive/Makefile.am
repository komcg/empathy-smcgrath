AM_CPPFLAGS =						\
	$(ERROR_CFLAGS)					\
	-I$(top_srcdir)                          	\
	-DPKGDATADIR=\""$(pkgdatadir)"\"		\
	$(EMPATHY_CFLAGS)				\
	$(WARN_CFLAGS)					\
	$(DISABLE_DEPRECATED)

LDADD =								\
	$(top_builddir)/libempathy-gtk/libempathy-gtk.la	\
	$(top_builddir)/libempathy/libempathy.la		\
	$(EMPATHY_LIBS)

noinst_PROGRAMS =			\
	contact-manager			\
	empetit				\
	test-empathy-account-assistant \
	test-empathy-presence-chooser	\
	test-empathy-status-preset-dialog \
	test-empathy-protocol-chooser

contact_manager_SOURCES = contact-manager.c
empetit_SOURCES = empetit.c
test_empathy_presence_chooser_SOURCES = test-empathy-presence-chooser.c
test_empathy_status_preset_dialog_SOURCES = test-empathy-status-preset-dialog.c
test_empathy_protocol_chooser_SOURCES = test-empathy-protocol-chooser.c
test_empathy_account_assistant_SOURCES = test-empathy-account-assistant.c

test_empathy_account_assistant_CFLAGS = -I$(top_srcdir)/src
test_empathy_account_assistant_LDADD = 			\
	$(top_builddir)/src/empathy-account-assistant.o	\
	$(top_builddir)/src/empathy-import-utils.o	\
	$(top_builddir)/src/empathy-import-pidgin.o	\
	$(top_builddir)/src/empathy-import-widget.o	\
	$(LDADD)
