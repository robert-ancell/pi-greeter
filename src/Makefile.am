sbin_PROGRAMS = pi-greeter

pi_greeter_SOURCES = \
	pi-greeter.c

AM_CPPFLAGS = \
	-I$(top_srcdir) \
	-DHAVE_CONFIG_H \
	-DGETTEXT_PACKAGE=\"$(GETTEXT_PACKAGE)\" \
	-DLOCALEDIR=\""$(localedir)"\" \
	-DGREETER_DATA_DIR=\""$(datadir)/pi-greeter"\" \
	-DCONFIG_FILE=\""$(sysconfdir)/lightdm/pi-greeter.conf"\" \
	-DINDICATOR_DIR=\""$(INDICATORDIR)"\" \
	-DUNITY_INDICATOR_DIR=\""$(UNITY_INDICATORDIR)"\" \
	$(WARN_CFLAGS)

pi_greeter_CFLAGS = \
	$(GTK_CFLAGS) \
	$(GMODULE_CFLAGS) \
	$(GTHREAD_CFLAGS) \
	$(LIGHTDMGOBJECT_CFLAGS) \
	$(LIBX11_CFLAGS) \
	$(LIBINDICATOR_CFLAGS) \
	$(LIBIDO_CFLAGS)

pi_greeter_LDADD = \
	$(GTK_LIBS) \
	$(GMODULE_LIBS) \
	$(GTHREAD_LIBS) \
	$(LIGHTDMGOBJECT_LIBS) \
	$(LIBX11_LIBS) \
	$(LIBINDICATOR_LIBS) \
	$(LIBIDO_LIBS)
