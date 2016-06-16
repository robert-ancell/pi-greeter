/*
 * Copyright (C) 2010-2011 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 * 
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <glib-unix.h>

#include <locale.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <cairo-xlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdkx.h>
#include <glib.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gslist.h>

#ifdef HAVE_LIBINDICATOR
#include <libindicator/indicator-object.h>
#ifdef HAVE_LIBINDICATOR_NG
#include <libindicator/indicator-ng.h>
#endif
#endif

#ifdef HAVE_LIBIDO
/* Some indicators need ido library */
#include "libido/libido.h"
#endif

#include <lightdm.h>

static LightDMGreeter *greeter;
static GKeyFile *state;
static gchar *state_filename;

/* Defaults */
static gchar *default_font_name, *default_theme_name, *default_icon_theme_name;
static GdkPixbuf *default_background_pixbuf = NULL;

/* Panel Widgets */
static GtkWidget *menubar, *power_menuitem, *session_menuitem, *language_menuitem;
static GtkMenu *session_menu, *language_menu;

/* Login Window Widgets */
static GtkWindow *login_window;
static GtkImage *user_image;
static GtkComboBox *user_combo;
static GtkEntry *username_entry, *password_entry;
static GtkLabel *message_label;
static GtkInfoBar *info_bar;
static GtkButton *cancel_button, *login_button;

static gchar **a11y_keyboard_command;
static GtkWindow *onboard_window;

/* Pending Questions */
static GSList *pending_questions = NULL;

GSList *backgrounds = NULL;

/* Current choices */
static gchar *current_session;
static gchar *current_language;

/* Screensaver values */
int timeout, interval, prefer_blanking, allow_exposures;

static GdkColor *default_background_color = NULL;
static gboolean cancelling = FALSE, prompted = FALSE;
static gboolean prompt_active = FALSE, password_prompted = FALSE;
static GdkRegion *window_region = NULL;
static gchar *wp_mode = NULL;

typedef struct
{
  gboolean is_prompt;
  union {
    LightDMMessageType message;
    LightDMPromptType prompt;
  } type;
  gchar *text;
} PAMConversationMessage;

typedef struct
{
    gint value;
    /* +0 and -0 */
    gint sign;
    /* interpret 'value' as percentage of screen width/height */
    gboolean percentage;
    /* -1: left/top, 0: center, +1: right,bottom */
    gint anchor;
} DimensionPosition;

typedef struct
{
    DimensionPosition x, y;
} WindowPosition;

const WindowPosition CENTERED_WINDOW_POS = { .x = {50, +1, TRUE, 0}, .y = {50, +1, TRUE, 0} };
WindowPosition main_window_pos;

GdkPixbuf* default_user_pixbuf = NULL;
gchar* default_user_icon = "avatar-default";

static void
pam_message_finalize (PAMConversationMessage *message)
{
    g_free (message->text);
    g_free (message);
}


static void
add_indicator_to_panel (GtkWidget *indicator_item, gint index)
{
    gint insert_pos = 0;
    GList* items = gtk_container_get_children (GTK_CONTAINER (menubar));
    GList* item;
    for (item = items; item; item = item->next)
    {
        if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item->data), "indicator-custom-index-data")) < index)
            break;
        insert_pos++;
    }
    g_list_free (items);

    gtk_menu_shell_insert (GTK_MENU_SHELL (menubar), GTK_WIDGET (indicator_item), insert_pos);
}

#ifdef HAVE_LIBINDICATOR
static gboolean
entry_scrolled (GtkWidget *menuitem, GdkEventScroll *event, gpointer data)
{
    IndicatorObject      *io;
    IndicatorObjectEntry *entry;

    g_return_val_if_fail (GTK_IS_WIDGET (menuitem), FALSE);

    io = g_object_get_data (G_OBJECT (menuitem), "indicator-custom-object-data");
    entry = g_object_get_data (G_OBJECT (menuitem), "indicator-custom-entry-data");

    g_return_val_if_fail (INDICATOR_IS_OBJECT (io), FALSE);

    g_signal_emit_by_name (io, "scroll", 1, event->direction);
    g_signal_emit_by_name (io, "scroll-entry", entry, 1, event->direction);

    return FALSE;
}

static void
entry_activated (GtkWidget *widget, gpointer user_data)
{
    IndicatorObject      *io;
    IndicatorObjectEntry *entry;

    g_return_if_fail (GTK_IS_WIDGET (widget));

    io = g_object_get_data (G_OBJECT (widget), "indicator-custom-object-data");
    entry = g_object_get_data (G_OBJECT (widget), "indicator-custom-entry-data");

    g_return_if_fail (INDICATOR_IS_OBJECT (io));

    return indicator_object_entry_activate (io, entry, gtk_get_current_event_time ());
}

static GtkWidget*
create_menuitem (IndicatorObject *io, IndicatorObjectEntry *entry, GtkWidget *menubar)
{
    GtkWidget *box, *menuitem;
    gint index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (io), "indicator-custom-index-data"));

    box = gtk_hbox_new (FALSE, 0);
    menuitem = gtk_menu_item_new ();

    gtk_widget_add_events(GTK_WIDGET(menuitem), GDK_SCROLL_MASK);

    g_object_set_data (G_OBJECT (menuitem), "indicator-custom-box-data", box);
    g_object_set_data (G_OBJECT (menuitem), "indicator-custom-object-data", io);
    g_object_set_data (G_OBJECT (menuitem), "indicator-custom-entry-data", entry);
    g_object_set_data (G_OBJECT (menuitem), "indicator-custom-index-data", GINT_TO_POINTER (index));

    g_signal_connect (G_OBJECT (menuitem), "activate", G_CALLBACK (entry_activated), NULL);
    g_signal_connect (G_OBJECT (menuitem), "scroll-event", G_CALLBACK (entry_scrolled), NULL);

    if (entry->image)
        gtk_box_pack_start (GTK_BOX(box), GTK_WIDGET(entry->image), FALSE, FALSE, 1);

    if (entry->label)
        gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(entry->label), FALSE, FALSE, 1);

    if (entry->menu)
        gtk_menu_item_set_submenu (GTK_MENU_ITEM (menuitem), GTK_WIDGET (entry->menu));

    gtk_container_add (GTK_CONTAINER (menuitem), box);
    gtk_widget_show (box);
    add_indicator_to_panel (menuitem, index);

    return menuitem;
}

static void
entry_added (IndicatorObject *io, IndicatorObjectEntry *entry, gpointer user_data)
{
    GHashTable *menuitem_lookup;
    GtkWidget  *menuitem;

    /* if the menuitem doesn't already exist, create it now */
    menuitem_lookup = g_object_get_data (G_OBJECT (io), "indicator-custom-menuitems-data");
    g_return_if_fail (menuitem_lookup);
    menuitem = g_hash_table_lookup (menuitem_lookup, entry);
    if (!GTK_IS_WIDGET (menuitem))
    {
        menuitem = create_menuitem (io, entry, GTK_WIDGET (user_data));
        g_hash_table_insert (menuitem_lookup, entry, menuitem);
    }

    gtk_widget_show (menuitem);
}

static void
entry_removed_cb (GtkWidget *widget, gpointer userdata)
{
    IndicatorObject *io;
    GHashTable      *menuitem_lookup;
    GtkWidget       *menuitem;
    gpointer         entry;

    io = g_object_get_data (G_OBJECT (widget), "indicator-custom-object-data");
    if (!INDICATOR_IS_OBJECT (io))
        return;

    entry = g_object_get_data (G_OBJECT (widget), "indicator-custom-entry-data");
    if (entry != userdata)
        return;

    menuitem_lookup = g_object_get_data (G_OBJECT (io), "indicator-custom-menuitems-data");
    g_return_if_fail (menuitem_lookup);
    menuitem = g_hash_table_lookup (menuitem_lookup, entry);
    if (GTK_IS_WIDGET (menuitem))
        gtk_widget_hide (menuitem);

    gtk_widget_destroy (widget);
}

static void
entry_removed (IndicatorObject *io, IndicatorObjectEntry *entry, gpointer user_data)
{
    gtk_container_foreach (GTK_CONTAINER (user_data), entry_removed_cb, entry);
}

static void
menu_show (IndicatorObject *io, IndicatorObjectEntry *entry, guint32 timestamp, gpointer user_data)
{
    IndicatorObjectEntry *entrydata;
    GtkWidget            *menuitem;
    GList                *entries, *lp;

    menuitem = GTK_WIDGET (user_data);

    if (!entry)
    {
        /* Close any open menus instead of opening one */
        entries = indicator_object_get_entries (io);
        for (lp = entries; lp; lp = g_list_next (entry))
        {
            entrydata = lp->data;
            gtk_menu_popdown (entrydata->menu);
        }
        g_list_free (entries);

        /* And tell the menuitem to exit activation mode too */
        gtk_menu_shell_cancel (GTK_MENU_SHELL (menuitem));
    }
}

static void
greeter_set_env (const gchar* key, const gchar* value)
{
    g_setenv (key, value, TRUE);

    GDBusProxy* proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       NULL,
                                                       "org.freedesktop.DBus",
                                                       "/org/freedesktop/DBus",
                                                       "org.freedesktop.DBus",
                                                       NULL, NULL);
    GVariant *result;
    GVariantBuilder *builder = g_variant_builder_new (G_VARIANT_TYPE_ARRAY);
    g_variant_builder_add (builder, "{ss}", key, value);
    result = g_dbus_proxy_call_sync (proxy, "UpdateActivationEnvironment", g_variant_new ("(a{ss})", builder),
                                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    g_variant_unref (result);
    g_variant_builder_unref (builder);
    g_object_unref (proxy);
}
#endif

static gboolean
menu_item_accel_closure_cb (GtkAccelGroup *accel_group,
                            GObject *acceleratable, guint keyval,
                            GdkModifierType modifier, gpointer data)
{
    gtk_menu_item_activate (data);
    return FALSE;
}

/* Maybe unnecessary (in future) trick to enable accelerators for hidden/detached menu items */
static void
reassign_menu_item_accel (GtkWidget *item)
{
    GtkAccelKey key;
    const gchar *accel_path = gtk_menu_item_get_accel_path (GTK_MENU_ITEM (item));

    if (accel_path && gtk_accel_map_lookup_entry (accel_path, &key))
    {
        GClosure *closure = g_cclosure_new (G_CALLBACK (menu_item_accel_closure_cb), item, NULL);
        gtk_accel_group_connect (gtk_menu_get_accel_group (GTK_MENU (gtk_widget_get_parent (item))),
                                 key.accel_key, key.accel_mods, key.accel_flags, closure);
        g_closure_unref (closure);
    }

    gtk_container_foreach (GTK_CONTAINER (gtk_menu_item_get_submenu (GTK_MENU_ITEM (item))),
                           (GtkCallback)reassign_menu_item_accel, NULL);
}

static void
#ifdef START_INDICATOR_SERVICES
init_indicators (GKeyFile* config, GPid* indicator_pid, GPid* spi_pid)
#else
init_indicators (GKeyFile* config)
#endif
{
    gchar **names = NULL;
    gsize length = 0;
    guint i;
    GHashTable *builtin_items = NULL;
    GHashTableIter iter;
    gpointer iter_value;
    gboolean fallback = FALSE;

#ifdef START_INDICATOR_SERVICES
    GError *error = NULL;
    gchar *AT_SPI_CMD[] = {"/usr/lib/at-spi2-core/at-spi-bus-launcher", "--launch-immediately", NULL};
    gchar *INDICATORS_CMD[] = {"init", "--user", "--startup-event", "indicator-services-start", NULL};
#endif

    if (g_key_file_has_key (config, "greeter", "indicators", NULL))
    {   /* no option = default list, empty value = empty list */
        names = g_key_file_get_string_list (config, "greeter", "indicators", &length, NULL);
    }
    else if (g_key_file_has_key (config, "greeter", "show-indicators", NULL))
    {   /* fallback mode: no option = empty value = default list */
        names = g_key_file_get_string_list (config, "greeter", "show-indicators", &length, NULL);
        if (length == 0)
            fallback = TRUE;
    }

    if (names && !fallback)
    {
        builtin_items = g_hash_table_new (g_str_hash, g_str_equal);

        g_hash_table_insert (builtin_items, "~power", power_menuitem);
        g_hash_table_insert (builtin_items, "~session", session_menuitem);
        g_hash_table_insert (builtin_items, "~language", language_menuitem);

        g_hash_table_iter_init (&iter, builtin_items);
        while (g_hash_table_iter_next (&iter, NULL, &iter_value))
            gtk_container_remove (GTK_CONTAINER (menubar), iter_value);
    }

    for (i = 0; i < length; ++i)
    {
        if (names[i][0] == '~' && g_hash_table_lookup_extended (builtin_items, names[i], NULL, &iter_value))
        {   /* Built-in indicators */
            g_object_set_data (G_OBJECT (iter_value), "indicator-custom-index-data", GINT_TO_POINTER (i));
            add_indicator_to_panel (iter_value, i);
            g_hash_table_remove (builtin_items, (gconstpointer)names[i]);
            continue;
        }

        #ifdef HAVE_LIBINDICATOR
        gchar* path = NULL;
        IndicatorObject* io = NULL;

        if (!inited)
        {
            /* Set indicators to run with reduced functionality */
            greeter_set_env ("INDICATOR_GREETER_MODE", "1");
            /* Don't allow virtual file systems? */
            greeter_set_env ("GIO_USE_VFS", "local");
            greeter_set_env ("GVFS_DISABLE_FUSE", "1");

            #ifdef START_INDICATOR_SERVICES
            if (!g_spawn_async (NULL, AT_SPI_CMD, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, spi_pid, &error))
                g_warning ("Failed to run \"at-spi-bus-launcher\": %s", error->message);
            g_clear_error (&error);

            if (!g_spawn_async (NULL, INDICATORS_CMD, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, indicator_pid, &error))
                g_warning ("Failed to run \"indicator-services\": %s", error->message);
            g_clear_error (&error);
            #endif
            inited = TRUE;
        }

        if (g_path_is_absolute (names[i]))
        {   /* library with absolute path */
            io = indicator_object_new_from_file (names[i]);
        }
        else if (g_str_has_suffix (names[i], G_MODULE_SUFFIX))
        {   /* library */
            path = g_build_filename (INDICATOR_DIR, names[i], NULL);
            io = indicator_object_new_from_file (path);
        }
        #ifdef HAVE_LIBINDICATOR_NG
        else
        {   /* service file */
            if (strchr (names[i], '.'))
                path = g_strdup_printf ("%s/%s", UNITY_INDICATOR_DIR, names[i]);
            else
                path = g_strdup_printf ("%s/com.canonical.indicator.%s", UNITY_INDICATOR_DIR, names[i]);
            io = INDICATOR_OBJECT (indicator_ng_new_for_profile (path, "desktop_greeter", NULL));
        }
        #endif

        if (io)
        {
            GList *entries, *lp;

            /* used to store/fetch menu entries */
            g_object_set_data_full (G_OBJECT (io), "indicator-custom-menuitems-data",
                                    g_hash_table_new (g_direct_hash, g_direct_equal),
                                    (GDestroyNotify) g_hash_table_destroy);
            g_object_set_data (G_OBJECT (io), "indicator-custom-index-data", GINT_TO_POINTER (i));

            g_signal_connect (G_OBJECT (io), INDICATOR_OBJECT_SIGNAL_ENTRY_ADDED,
                              G_CALLBACK (entry_added), menubar);
            g_signal_connect (G_OBJECT (io), INDICATOR_OBJECT_SIGNAL_ENTRY_REMOVED,
                              G_CALLBACK (entry_removed), menubar);
            g_signal_connect (G_OBJECT (io), INDICATOR_OBJECT_SIGNAL_MENU_SHOW,
                              G_CALLBACK (menu_show), menubar);

            entries = indicator_object_get_entries (io);
            for (lp = entries; lp; lp = g_list_next (lp))
                entry_added (io, lp->data, menubar);
            g_list_free (entries);
        }
        else
        {
            g_warning ("Indicator \"%s\": failed to load", names[i]);
        }

        g_free (path);
        #endif
    }
    if (names)
        g_strfreev (names);

    if (builtin_items)
    {
        g_hash_table_iter_init (&iter, builtin_items);
        while (g_hash_table_iter_next (&iter, NULL, &iter_value))
        {
            reassign_menu_item_accel (iter_value);
            gtk_widget_hide (iter_value);
        }

        g_hash_table_unref (builtin_items);
    }
}

static gboolean
is_valid_session (GList* items, const gchar* session)
{
    for (; items; items = g_list_next (items))
        if (g_strcmp0 (session, lightdm_session_get_key (items->data)) == 0)
            return TRUE;
    return FALSE;
}

static gchar *
get_session (void)
{
    return g_strdup (current_session);
}

static void
set_session (const gchar *session)
{
    gchar *last_session = NULL;
    GList *sessions = lightdm_get_sessions ();

    /* Validation */
    if (!session || !is_valid_session (sessions, session))
    {
        /* previous session */
        last_session = g_key_file_get_value (state, "greeter", "last-session", NULL);
        if (last_session && g_strcmp0 (session, last_session) != 0 &&
            is_valid_session (sessions, last_session))
            session = last_session;
        else
        {
            /* default */
            const gchar* default_session = lightdm_greeter_get_default_session_hint (greeter);
            if (g_strcmp0 (session, default_session) != 0 &&
                is_valid_session (sessions, default_session))
                session = default_session;
            /* first in the sessions list */
            else if (sessions)
                session = lightdm_session_get_key (sessions->data);
            /* give up */
            else
                session = NULL;
        }
    }

    if (gtk_widget_get_visible (session_menuitem))
    {
        GList *menu_iter = NULL;
        GList *menu_items = gtk_container_get_children (GTK_CONTAINER (session_menu));
        if (session)
        {
            for (menu_iter = menu_items; menu_iter != NULL; menu_iter = g_list_next(menu_iter))
                if (g_strcmp0 (session, g_object_get_data (G_OBJECT (menu_iter->data), "session-key")) == 0)
                {
                    break;
                }
        }
        if (!menu_iter)
            menu_iter = menu_items;
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_iter->data), TRUE);
    }

    g_free (current_session);
    current_session = g_strdup(session);
    g_free (last_session);
}

static gchar *
get_language (void)
{
    GList *menu_items, *menu_iter;

    /* if the user manually selected a language, use it */
    if (current_language)
        return g_strdup (current_language);

    menu_items = gtk_container_get_children(GTK_CONTAINER(language_menu));    
    for (menu_iter = menu_items; menu_iter != NULL; menu_iter = g_list_next(menu_iter))
    {
        if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menu_iter->data)))
        {
            return g_strdup(g_object_get_data (G_OBJECT (menu_iter->data), "language-code"));
        }
    }

    return NULL;
}

static void
set_language (const gchar *language)
{
    const gchar *default_language = NULL;    
    GList *menu_items, *menu_iter;

    if (!gtk_widget_get_visible (language_menuitem))
    {
        g_free (current_language);
        current_language = g_strdup (language);
        return;
    }

    menu_items = gtk_container_get_children(GTK_CONTAINER(language_menu));

    if (language)
    {
        for (menu_iter = menu_items; menu_iter != NULL; menu_iter = g_list_next(menu_iter))
        {
            gchar *s;
            gboolean matched;
            s = g_strdup(g_object_get_data (G_OBJECT (menu_iter->data), "language-code"));
            matched = g_strcmp0 (s, language) == 0;
            g_free (s);
            if (matched)
            {
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_iter->data), TRUE);
                g_free (current_language);
                current_language = g_strdup(language);
                gtk_menu_item_set_label(GTK_MENU_ITEM(language_menuitem),language);
                return;
            }
        }
    }

    /* If failed to find this language, then try the default */
    if (lightdm_get_language ()) {
        default_language = lightdm_language_get_code (lightdm_get_language ());
        gtk_menu_item_set_label(GTK_MENU_ITEM (language_menuitem), default_language);
    }
    if (default_language && g_strcmp0 (default_language, language) != 0)
        set_language (default_language);
    /* If all else fails, just use the first language from the menu */
    else {
        for (menu_iter = menu_items; menu_iter != NULL; menu_iter = g_list_next(menu_iter))
        {
            if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menu_iter->data)))
                gtk_menu_item_set_label(GTK_MENU_ITEM(language_menuitem), g_strdup(g_object_get_data (G_OBJECT (menu_iter->data), "language-code")));
        }
    }
}

static void
set_message_label (const gchar *text)
{
    gtk_widget_set_visible (GTK_WIDGET (info_bar), g_strcmp0 (text, "") != 0);
    gtk_label_set_text (message_label, text);
}

static void
set_login_button_label (LightDMGreeter *greeter, const gchar *username)
{
    LightDMUser *user;
    gboolean logged_in = FALSE;

    user = lightdm_user_list_get_user_by_name (lightdm_user_list_get_instance (), username);
    if (user)
        logged_in = lightdm_user_get_logged_in (user);
    if (logged_in)
        gtk_button_set_label (login_button, _("Unlock"));
    else
        gtk_button_set_label (login_button, _("Log In"));
    gtk_widget_set_can_default (GTK_WIDGET (login_button), TRUE);
    gtk_widget_grab_default (GTK_WIDGET (login_button));
    /* and disable the session and language widgets */
    gtk_widget_set_sensitive (GTK_WIDGET (session_menuitem), !logged_in);
    gtk_widget_set_sensitive (GTK_WIDGET (language_menuitem), !logged_in);
}

static void set_background (GdkPixbuf *new_bg);

static void
set_user_background (const gchar *username)
{
    LightDMUser *user;
    const gchar *path;
    GdkPixbuf *bg = NULL;
    GError *error = NULL;

    user = lightdm_user_list_get_user_by_name (lightdm_user_list_get_instance (), username);
    if (user)
    {
        path = lightdm_user_get_background (user);
        if (path)
        {
            bg = gdk_pixbuf_new_from_file (path, &error);
            if (!bg)
            {
                g_warning ("Failed to load user background: %s", error->message);
                g_clear_error (&error);
            }
        }
    }

    set_background (bg);
    if (bg)
        g_object_unref (bg);
}

static void
set_user_image (const gchar *username)
{
    const gchar *path;
    LightDMUser *user;
    GdkPixbuf *image = NULL;
    GError *error = NULL;

    user = lightdm_user_list_get_user_by_name (lightdm_user_list_get_instance (), username);
    if (user)
    {
        path = lightdm_user_get_image (user);
        if (path)
        {
            image = gdk_pixbuf_new_from_file_at_scale (path, 80, 80, FALSE, &error);
            if (image)
            {
                gtk_image_set_from_pixbuf (GTK_IMAGE (user_image), image);
                g_object_unref (image);
                return;
            }
            else
            {
                g_warning ("Failed to load user image: %s", error->message);
                g_clear_error (&error);
            }
        }
    }
    
    if (default_user_pixbuf)
        gtk_image_set_from_pixbuf (GTK_IMAGE (user_image), default_user_pixbuf);
    else
        gtk_image_set_from_icon_name (GTK_IMAGE (user_image), default_user_icon, GTK_ICON_SIZE_DIALOG);
}

/* Function translate user defined coordinates to absolute value */
static gint
get_absolute_position (const DimensionPosition *p, gint screen, gint window)
{
    gint x = p->percentage ? (screen*p->value)/100 : p->value;
    x = p->sign < 0 ? screen - x : x;
    if (p->anchor > 0)
        x -= window;
    else if (p->anchor == 0)
        x -= window/2;

    if (x < 0)                     /* Offscreen: left/top */
        return 0;
    else if (x + window > screen)  /* Offscreen: right/bottom */
        return screen - window;
    else
        return x;
}

static void
center_window (GtkWindow *window, GtkAllocation *unused, const WindowPosition *pos)
{   
    GdkScreen *screen = gtk_window_get_screen (window);
    GtkAllocation allocation;
    GdkRectangle monitor_geometry;

    gdk_screen_get_monitor_geometry (screen, gdk_screen_get_primary_monitor (screen), &monitor_geometry);
    gtk_widget_get_allocation (GTK_WIDGET (window), &allocation);
    gtk_window_move (window,
                     monitor_geometry.x + get_absolute_position (&pos->x, monitor_geometry.width, allocation.width),
                     monitor_geometry.y + get_absolute_position (&pos->y, monitor_geometry.height, allocation.height));
}

static GdkRegion *
cairo_region_from_rectangle (gint width, gint height, gint radius)
{
    GdkRegion *region;

    gint x = radius, y = 0;
    gint xChange = 1 - (radius << 1);
    gint yChange = 0;
    gint radiusError = 0;

    GdkRectangle rect;

    rect.x = radius;
    rect.y = radius;
    rect.width = width - radius * 2;
    rect.height = height - radius * 2;

    region = gdk_region_rectangle (&rect);

    while(x >= y)
    {

        rect.x = -x + radius;
        rect.y = -y + radius;
        rect.width = x - radius + width - rect.x;
        rect.height =  y - radius + height - rect.y;

        gdk_region_union_with_rect(region, &rect);

        rect.x = -y + radius;
        rect.y = -x + radius;
        rect.width = y - radius + width - rect.x;
        rect.height =  x - radius + height - rect.y;

        gdk_region_union_with_rect(region, &rect);

        y++;
        radiusError += yChange;
        yChange += 2;
        if(((radiusError << 1) + xChange) > 0)
        {
            x--;
            radiusError += xChange;
            xChange += 2;
        }
   }

   return region;
}

static gboolean
login_window_size_allocate (GtkWidget *widget, GdkRectangle *allocation, gpointer user_data)
{
    gint    radius = 2;

    GdkWindow *window = gtk_widget_get_window (widget);
    if (window_region)
        gdk_region_destroy(window_region);
    window_region = cairo_region_from_rectangle (allocation->width, allocation->height, radius);
    if (window) {
        gdk_window_shape_combine_region(window, window_region, 0, 0);
        gdk_window_input_shape_combine_region(window, window_region, 0, 0);
    }

    return TRUE;
}


static void draw_background (cairo_t *c, GdkPixbuf *bg, gint m_width, gint m_height)
{
    GdkPixbuf *p = NULL;
    gint p_height, p_width, offset_x = 0, offset_y = 0;
    gdouble scale_x, scale_y;

    gdk_cairo_set_source_color (c, default_background_color);
    cairo_rectangle (c, 0, 0, m_width, m_height);
    cairo_fill (c);

    if (bg && strcmp (wp_mode, "color"))
    {
        p_width = gdk_pixbuf_get_width (bg);
        p_height = gdk_pixbuf_get_height (bg);
        scale_x = (float) m_width / p_width;
        scale_y = (float) m_height / p_height;

        if (!strcmp (wp_mode, "center"))
        {
            offset_x = (m_width - p_width) / 2;
            offset_y = (m_height - p_height) / 2;
            p = gdk_pixbuf_copy (bg);
        }
        else if (!strcmp (wp_mode, "fit"))
        {
            p_width *= scale_y < scale_x ? scale_y : scale_x;
            p_height *= scale_y < scale_x ? scale_y : scale_x;
            offset_x = (m_width - p_width) / 2;
            offset_y = (m_height - p_height) / 2;
            p = gdk_pixbuf_scale_simple (bg, p_width, p_height, GDK_INTERP_BILINEAR);
        }
        else if (!strcmp (wp_mode, "crop"))
        {
            p_width *= scale_x < scale_y ? scale_y : scale_x;
            p_height *= scale_x < scale_y ? scale_y : scale_x;
            offset_x = (m_width - p_width) / 2;
            offset_y = (m_height - p_height) / 2;
            p = gdk_pixbuf_scale_simple (bg, p_width, p_height, GDK_INTERP_BILINEAR);
        }
        else if (!strcmp (wp_mode, "stretch"))
        {
            p = gdk_pixbuf_scale_simple (bg, m_width, m_height, GDK_INTERP_BILINEAR);
        }
        else if (!strcmp (wp_mode, "tile"))
        {
            p = gdk_pixbuf_copy (bg);
        }
        gdk_cairo_set_source_pixbuf (c, p, offset_x, offset_y);
        if (!strcmp (wp_mode, "tile")) cairo_pattern_set_extend (cairo_get_source (c), CAIRO_EXTEND_REPEAT);
    }
    else gdk_cairo_set_source_color (c, default_background_color);
    cairo_paint (c);
}

static gboolean
background_window_expose (GtkWidget    *widget,
                                       GdkEventExpose *event,
                                       gpointer user_data)
{
    GdkWindow *wd = gtk_widget_get_window (widget);
    cairo_t *cr = gdk_cairo_create (wd);
    draw_background (cr, default_background_pixbuf, gdk_window_get_width (wd), gdk_window_get_height (wd));
    return FALSE;
}

static void
start_authentication (const gchar *username)
{
    gchar *data;
    gsize data_length;
    GError *error = NULL;

    cancelling = FALSE;
    prompted = FALSE;
    password_prompted = FALSE;
    prompt_active = FALSE;

    if (pending_questions)
    {
        g_slist_free_full (pending_questions, (GDestroyNotify) pam_message_finalize);
        pending_questions = NULL;
    }

    g_key_file_set_value (state, "greeter", "last-user", username);
    data = g_key_file_to_data (state, &data_length, &error);
    if (error)
        g_warning ("Failed to save state file: %s", error->message);
    g_clear_error (&error);
    if (data)
    {
        g_file_set_contents (state_filename, data, data_length, &error);
        if (error)
            g_warning ("Failed to save state file: %s", error->message);
        g_clear_error (&error);
    }
    g_free (data);

    if (g_strcmp0 (username, "*other") == 0)
    {
        gtk_widget_show (GTK_WIDGET (username_entry));
        gtk_widget_show (GTK_WIDGET (cancel_button));
        lightdm_greeter_authenticate (greeter, NULL);
    }
    else if (g_strcmp0 (username, "*guest") == 0)
    {
        lightdm_greeter_authenticate_as_guest (greeter);
    }
    else
    {
        LightDMUser *user;

        user = lightdm_user_list_get_user_by_name (lightdm_user_list_get_instance (), username);
        if (user)
        {
            if (!current_session)
                set_session (lightdm_user_get_session (user));
            if (!current_language)
                set_language (lightdm_user_get_language (user));
        }
        else
        {
            set_session (NULL);
            set_language (NULL);
        }

        lightdm_greeter_authenticate (greeter, username);
    }
}

static void
cancel_authentication (void)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    gboolean other = FALSE;

    if (pending_questions)
    {
        g_slist_free_full (pending_questions, (GDestroyNotify) pam_message_finalize);
        pending_questions = NULL;
    }

    /* If in authentication then stop that first */
    cancelling = FALSE;
    if (lightdm_greeter_get_in_authentication (greeter))
    {
        cancelling = TRUE;
        lightdm_greeter_cancel_authentication (greeter);
        set_message_label ("");
    }

    /* Make sure password entry is back to normal */
    gtk_entry_set_visibility (password_entry, FALSE);

    /* Force refreshing the prompt_box for "Other" */
    model = gtk_combo_box_get_model (user_combo);

    if (gtk_combo_box_get_active_iter (user_combo, &iter))
    {
        gchar *user;

        gtk_tree_model_get (GTK_TREE_MODEL (model), &iter, 0, &user, -1);
        other = (g_strcmp0 (user, "*other") == 0);
        g_free (user);
    }

    /* Start a new login or return to the user list */
    if (other || lightdm_greeter_get_hide_users_hint (greeter))
        start_authentication ("*other");
    else
        gtk_widget_grab_focus (GTK_WIDGET (user_combo));
}

static void
start_session (void)
{
    gchar *language;
    gchar *session;
    gchar *data;
    gsize data_length;
    GError *error = NULL;

    language = get_language ();
    if (language)
        lightdm_greeter_set_language (greeter, language);
    g_free (language);

    session = get_session ();

    /* Remember last choice */
    g_key_file_set_value (state, "greeter", "last-session", session);

    data = g_key_file_to_data (state, &data_length, &error);
    if (error)
        g_warning ("Failed to save state file: %s", error->message);
    g_clear_error (&error);
    if (data)
    {
        g_file_set_contents (state_filename, data, data_length, &error);
        if (error)
            g_warning ("Failed to save state file: %s", error->message);
        g_clear_error (&error);
    }
    g_free (data);

    if (!lightdm_greeter_start_session_sync (greeter, session, NULL))
    {
        set_message_label (_("Failed to start session"));
        start_authentication (lightdm_greeter_get_authentication_user (greeter));
    }
    g_free (session);
}

void
session_selected_cb(GtkMenuItem *menuitem, gpointer user_data);
G_MODULE_EXPORT
void
session_selected_cb(GtkMenuItem *menuitem, gpointer user_data)
{
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menuitem)))
    {
       gchar *session = g_object_get_data (G_OBJECT (menuitem), "session-key");
       set_session(session);
    }
}

void
language_selected_cb(GtkMenuItem *menuitem, gpointer user_data);
G_MODULE_EXPORT
void
language_selected_cb(GtkMenuItem *menuitem, gpointer user_data)
{
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menuitem)))
    {
       gchar *language = g_object_get_data (G_OBJECT (menuitem), "language-code");
       set_language(language);
    }
}

gboolean
password_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
G_MODULE_EXPORT
gboolean
password_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    if ((event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_Down) &&
        gtk_widget_get_visible(GTK_WIDGET(user_combo)))
    {
        gboolean available;
        GtkTreeIter iter;
        GtkTreeModel *model = gtk_combo_box_get_model (user_combo);

        /* Back to username_entry if it is available */
        if (event->keyval == GDK_KEY_Up &&
            gtk_widget_get_visible (GTK_WIDGET (username_entry)) && widget == GTK_WIDGET (password_entry))
        {
            gtk_widget_grab_focus (GTK_WIDGET (username_entry));
            return TRUE;
        }

        if (!gtk_combo_box_get_active_iter (user_combo, &iter))
            return FALSE;

        if (event->keyval == GDK_KEY_Up)
        {
            GtkTreePath *path = gtk_tree_model_get_path (model, &iter);
            available = gtk_tree_path_prev (path);
            if (available)
                available = gtk_tree_model_get_iter (model, &iter, path);
            gtk_tree_path_free (path);
        }
        else
            available = gtk_tree_model_iter_next (model, &iter);

        if (available)
            gtk_combo_box_set_active_iter (user_combo, &iter);

        return TRUE;
    }
    return FALSE;
}

gboolean
username_focus_out_cb (GtkWidget *widget, GdkEvent *event, gpointer user_data);
G_MODULE_EXPORT
gboolean
username_focus_out_cb (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    if (!g_strcmp0(gtk_entry_get_text(username_entry), "") == 0)
        start_authentication(gtk_entry_get_text(username_entry));
    return FALSE;
}

gboolean
username_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
G_MODULE_EXPORT
gboolean
username_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    /* Acts as password_entry */
    if (event->keyval == GDK_KEY_Up)
        return password_key_press_cb (widget, event, user_data);
    /* Enter activates the password entry */
    else if (event->keyval == GDK_KEY_Return)
    {
        gtk_widget_grab_focus(GTK_WIDGET(password_entry));
        return TRUE;
    }
    else
        return FALSE;
}

gboolean
menubar_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
G_MODULE_EXPORT
gboolean
menubar_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    switch (event->keyval)
    {
    case GDK_KEY_Tab: case GDK_KEY_Escape:
    case GDK_KEY_Super_L: case GDK_KEY_Super_R:
    case GDK_KEY_F9: case GDK_KEY_F10:
    case GDK_KEY_F11: case GDK_KEY_F12:
        gtk_menu_shell_cancel (GTK_MENU_SHELL (menubar));
        gtk_window_present (login_window);
        return TRUE;
    default:
        return FALSE;
    };
}

gboolean
login_window_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
G_MODULE_EXPORT
gboolean
login_window_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    GtkWidget *item = NULL;

    if (event->keyval == GDK_KEY_F9)
        item = session_menuitem;
    else if (event->keyval == GDK_KEY_F10)
        item = language_menuitem;
    else if (event->keyval == GDK_KEY_F12)
        item = power_menuitem;
    else if (event->keyval != GDK_KEY_Escape &&
             event->keyval != GDK_KEY_Super_L &&
             event->keyval != GDK_KEY_Super_R)
        return FALSE;

    if (GTK_IS_MENU_ITEM (item) && gtk_widget_is_sensitive (item) && gtk_widget_get_visible (item))
        gtk_menu_shell_select_item (GTK_MENU_SHELL (menubar), item);
    else
        gtk_menu_shell_select_first (GTK_MENU_SHELL (menubar), TRUE);
    return TRUE;
}

static void set_displayed_user (LightDMGreeter *greeter, gchar *username)
{
    gchar *user_tooltip;
    LightDMUser *user;

    if (g_strcmp0 (username, "*other") == 0)
    {
        gtk_widget_show (GTK_WIDGET (username_entry));
        gtk_widget_show (GTK_WIDGET (cancel_button));
        user_tooltip = g_strdup(_("Other"));
    }
    else
    {
        gtk_widget_hide (GTK_WIDGET (username_entry));
        gtk_widget_show (GTK_WIDGET (cancel_button));
        gtk_widget_grab_focus (GTK_WIDGET (password_entry));
        user_tooltip = g_strdup(username);
    }

    if (g_strcmp0 (username, "*guest") == 0)
    {
        user_tooltip = g_strdup(_("Guest Session"));
    }

    set_login_button_label (greeter, username);
    set_user_background (username);
    set_user_image (username);
    user = lightdm_user_list_get_user_by_name (lightdm_user_list_get_instance (), username);
    if (user)
    {
        set_language (lightdm_user_get_language (user));
        set_session (lightdm_user_get_session (user));
    }
    else
        set_language (lightdm_language_get_code (lightdm_get_language ()));
    gtk_widget_set_tooltip_text (GTK_WIDGET (user_combo), user_tooltip);
    start_authentication (username);
    g_free (user_tooltip);
}

void user_combobox_active_changed_cb (GtkComboBox *widget, LightDMGreeter *greeter);
G_MODULE_EXPORT
void
user_combobox_active_changed_cb (GtkComboBox *widget, LightDMGreeter *greeter)
{
    GtkTreeModel *model;
    GtkTreeIter iter;

    model = gtk_combo_box_get_model (user_combo);

    if (gtk_combo_box_get_active_iter (user_combo, &iter))
    {
        gchar *user;

        gtk_tree_model_get (GTK_TREE_MODEL (model), &iter, 0, &user, -1);

        set_displayed_user(greeter, user);

        g_free (user);
    }
    set_message_label ("");
}

static const gchar*
get_message_label (void)
{
    return gtk_label_get_text (message_label);
}

static void
process_prompts (LightDMGreeter *greeter)
{
    if (!pending_questions)
        return;

    /* always allow the user to change username again */
    gtk_widget_set_sensitive (GTK_WIDGET (username_entry), TRUE);
    gtk_widget_set_sensitive (GTK_WIDGET (password_entry), TRUE);

    /* Special case: no user selected from list, so PAM asks us for the user
     * via a prompt. For that case, use the username field */
    if (!prompted && pending_questions && !pending_questions->next &&
        ((PAMConversationMessage *) pending_questions->data)->is_prompt &&
        ((PAMConversationMessage *) pending_questions->data)->type.prompt != LIGHTDM_PROMPT_TYPE_SECRET &&
        gtk_widget_get_visible ((GTK_WIDGET (username_entry))) &&
        lightdm_greeter_get_authentication_user (greeter) == NULL)
    {
        prompted = TRUE;
        prompt_active = TRUE;
        gtk_widget_grab_focus (GTK_WIDGET (username_entry));
        return;
    }

    while (pending_questions)
    {
        PAMConversationMessage *message = (PAMConversationMessage *) pending_questions->data;
        pending_questions = g_slist_remove (pending_questions, (gconstpointer) message);

        if (!message->is_prompt)
        {
            /* FIXME: this doesn't show multiple messages, but that was
             * already the case before. */
            set_message_label (message->text);
            continue;
        }

        gtk_entry_set_text (password_entry, "");
        gtk_entry_set_visibility (password_entry, message->type.prompt != LIGHTDM_PROMPT_TYPE_SECRET);
        if (get_message_label()[0] == 0 && password_prompted)
        {
            /* No message was provided beforehand and this is not the
             * first password prompt, so use the prompt as label,
             * otherwise the user will be completely unclear of what
             * is going on. Actually, the fact that prompt messages are
             * not shown is problematic in general, especially if
             * somebody uses a custom PAM module that wants to ask
             * something different. */
            gchar *str = message->text;
            if (g_str_has_suffix (str, ": "))
                str = g_strndup (str, strlen (str) - 2);
            else if (g_str_has_suffix (str, ":"))
                str = g_strndup (str, strlen (str) - 1);
            set_message_label (str);
            if (str != message->text)
                g_free (str);
        }
        gtk_widget_grab_focus (GTK_WIDGET (password_entry));
        prompted = TRUE;
        password_prompted = TRUE;
        prompt_active = TRUE;

        /* If we have more stuff after a prompt, assume that other prompts are pending,
         * so stop here. */
        break;
    }
}

void login_cb (GtkWidget *widget);
G_MODULE_EXPORT
void
login_cb (GtkWidget *widget)
{
    /* Reset to default screensaver values */
    if (lightdm_greeter_get_lock_hint (greeter))
        XSetScreenSaver(gdk_x11_display_get_xdisplay(gdk_display_get_default ()), timeout, interval, prefer_blanking, allow_exposures);        

    gtk_widget_set_sensitive (GTK_WIDGET (username_entry), FALSE);
    gtk_widget_set_sensitive (GTK_WIDGET (password_entry), FALSE);
    set_message_label ("");
    prompt_active = FALSE;

    if (lightdm_greeter_get_is_authenticated (greeter))
        start_session ();
    else if (lightdm_greeter_get_in_authentication (greeter))
    {
        lightdm_greeter_respond (greeter, gtk_entry_get_text (password_entry));
        /* If we have questions pending, then we continue processing
         * those, until we are done. (Otherwise, authentication will
         * not complete.) */
        if (pending_questions)
            process_prompts (greeter);
    }
    else
        start_authentication (lightdm_greeter_get_authentication_user (greeter));
}

void cancel_cb (GtkWidget *widget);
G_MODULE_EXPORT
void
cancel_cb (GtkWidget *widget)
{
    //cancel_authentication ();
    lightdm_shutdown (NULL);
}

static void
show_prompt_cb (LightDMGreeter *greeter, const gchar *text, LightDMPromptType type)
{
    PAMConversationMessage *message_obj = g_new (PAMConversationMessage, 1);
    if (message_obj)
    {
        message_obj->is_prompt = TRUE;
        message_obj->type.prompt = type;
        message_obj->text = g_strdup (text);
        pending_questions = g_slist_append (pending_questions, message_obj);
    }

    if (!prompt_active)
        process_prompts (greeter);
}

static void
show_message_cb (LightDMGreeter *greeter, const gchar *text, LightDMMessageType type)
{
    PAMConversationMessage *message_obj = g_new (PAMConversationMessage, 1);
    if (message_obj)
    {
        message_obj->is_prompt = FALSE;
        message_obj->type.message = type;
        message_obj->text = g_strdup (text);
        pending_questions = g_slist_append (pending_questions, message_obj);
    }

    if (!prompt_active)
        process_prompts (greeter);
}

static void
authentication_complete_cb (LightDMGreeter *greeter)
{
    prompt_active = FALSE;
    gtk_entry_set_text (password_entry, "");

    if (cancelling)
    {
        cancel_authentication ();
        return;
    }

    if (pending_questions)
    {
        g_slist_free_full (pending_questions, (GDestroyNotify) pam_message_finalize);
        pending_questions = NULL;
    }

    if (lightdm_greeter_get_is_authenticated (greeter))
    {
        if (prompted)
            start_session ();
    }
    else
    {
        if (prompted)
        {
            if (get_message_label()[0] == 0)
                set_message_label (_("Incorrect password, please try again"));
            start_authentication (lightdm_greeter_get_authentication_user (greeter));
        }
        else
            set_message_label (_("Failed to authenticate"));
    }
}

void suspend_cb (GtkWidget *widget, LightDMGreeter *greeter);
G_MODULE_EXPORT
void
suspend_cb (GtkWidget *widget, LightDMGreeter *greeter)
{
    lightdm_suspend (NULL);
}

void hibernate_cb (GtkWidget *widget, LightDMGreeter *greeter);
G_MODULE_EXPORT
void
hibernate_cb (GtkWidget *widget, LightDMGreeter *greeter)
{
    lightdm_hibernate (NULL);
}

static gboolean
show_power_prompt (const gchar* action, const gchar* message, const gchar* icon,
                   const gchar* dialog_name, const gchar* button_name)
{
    GtkWidget *dialog;
    GtkWidget *image;
    GtkWidget *button;
    gboolean   result;
    const GList *items, *item;
    gint logged_in_users = 0;
    gchar *warning;
    
    /* Check if there are still users logged in, count them and if so, display a warning */
    items = lightdm_user_list_get_users (lightdm_user_list_get_instance ());
    for (item = items; item; item = item->next)
    {
        LightDMUser *user = item->data;
        if (lightdm_user_get_logged_in (user))
            logged_in_users++;
    }
    if (logged_in_users > 0)
    {
        if (logged_in_users > 1)
            warning = g_strdup_printf (_("Warning: There are still %d users logged in."), logged_in_users);
        else
            warning = g_strdup_printf (_("Warning: There is still %d user logged in."), logged_in_users);
        message = g_markup_printf_escaped ("<b>%s</b>\n%s", warning, message);
        g_free (warning);
    }

    /* Prepare the dialog */
    dialog = gtk_message_dialog_new (NULL,
                                     GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_OTHER,
                                     GTK_BUTTONS_NONE,
                                     "%s", action);
    gtk_message_dialog_format_secondary_markup(GTK_MESSAGE_DIALOG(dialog), "%s", message);        
    button = gtk_dialog_add_button(GTK_DIALOG (dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
    gtk_widget_set_name(button, "cancel_button");
    button = gtk_dialog_add_button(GTK_DIALOG (dialog), action, GTK_RESPONSE_OK);
    gtk_widget_set_name(button, button_name);

    /* Add the icon */
    image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DIALOG);
    gtk_message_dialog_set_image(GTK_MESSAGE_DIALOG(dialog), image);

    /* Make the dialog themeable and attractive */
    gtk_widget_set_name(dialog, dialog_name);
    g_signal_connect (G_OBJECT (dialog), "size-allocate", G_CALLBACK (login_window_size_allocate), NULL);
    gtk_container_set_border_width(GTK_CONTAINER (dialog), 18);

    /* Hide the login window and show the dialog */
    gtk_widget_hide (GTK_WIDGET (login_window));
    gtk_widget_show_all (dialog);
    center_window (GTK_WINDOW (dialog), NULL, &CENTERED_WINDOW_POS);

    result = gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK;

    gtk_widget_destroy (dialog);
    gtk_widget_show (GTK_WIDGET (login_window));

    return result;
}

void restart_cb (GtkWidget *widget, LightDMGreeter *greeter);
G_MODULE_EXPORT
void
restart_cb (GtkWidget *widget, LightDMGreeter *greeter)
{
    if (show_power_prompt(_("Restart"), _("Are you sure you want to close all programs and restart the computer?"),
                          "view-refresh",
                          "restart_dialog", "restart_button"))
        lightdm_restart (NULL);
}

void shutdown_cb (GtkWidget *widget, LightDMGreeter *greeter);
G_MODULE_EXPORT
void
shutdown_cb (GtkWidget *widget, LightDMGreeter *greeter)
{
    if (show_power_prompt(_("Shut Down"), _("Are you sure you want to close all programs and shut down the computer?"),
                          "system-shutdown",
                          "shutdown_dialog", "shutdown_button"))
        lightdm_shutdown (NULL);
}

static void
user_added_cb (LightDMUserList *user_list, LightDMUser *user, LightDMGreeter *greeter)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    gboolean logged_in = FALSE;

    model = gtk_combo_box_get_model (user_combo);

    logged_in = lightdm_user_get_logged_in (user);

    gtk_list_store_append (GTK_LIST_STORE (model), &iter);
    gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                        0, lightdm_user_get_name (user),
                        1, lightdm_user_get_display_name (user),
                        2, logged_in ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                        -1);
}

static gboolean
get_user_iter (const gchar *username, GtkTreeIter *iter)
{
    GtkTreeModel *model;

    model = gtk_combo_box_get_model (user_combo);

    if (!gtk_tree_model_get_iter_first (model, iter))
        return FALSE;
    do
    {
        gchar *name;
        gboolean matched;

        gtk_tree_model_get (model, iter, 0, &name, -1);
        matched = g_strcmp0 (name, username) == 0;
        g_free (name);
        if (matched)
            return TRUE;
    } while (gtk_tree_model_iter_next (model, iter));

    return FALSE;
}

static void
user_changed_cb (LightDMUserList *user_list, LightDMUser *user, LightDMGreeter *greeter)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    gboolean logged_in = FALSE;

    if (!get_user_iter (lightdm_user_get_name (user), &iter))
        return;
    logged_in = lightdm_user_get_logged_in (user);

    model = gtk_combo_box_get_model (user_combo);

    gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                        0, lightdm_user_get_name (user),
                        1, lightdm_user_get_display_name (user),
                        2, logged_in ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                        -1);
}

static void
user_removed_cb (LightDMUserList *user_list, LightDMUser *user)
{
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (!get_user_iter (lightdm_user_get_name (user), &iter))
        return;

    model = gtk_combo_box_get_model (user_combo);
    gtk_list_store_remove (GTK_LIST_STORE (model), &iter);
}

static void
load_user_list (void)
{
    const GList *items, *item;
    GtkTreeModel *model;
    GtkTreeIter iter;
    gchar *last_user;
    const gchar *selected_user;
    gboolean logged_in = FALSE;

    g_signal_connect (lightdm_user_list_get_instance (), "user-added", G_CALLBACK (user_added_cb), greeter);
    g_signal_connect (lightdm_user_list_get_instance (), "user-changed", G_CALLBACK (user_changed_cb), greeter);
    g_signal_connect (lightdm_user_list_get_instance (), "user-removed", G_CALLBACK (user_removed_cb), NULL);
    model = gtk_combo_box_get_model (user_combo);
    items = lightdm_user_list_get_users (lightdm_user_list_get_instance ());
    for (item = items; item; item = item->next)
    {
        LightDMUser *user = item->data;
        logged_in = lightdm_user_get_logged_in (user);

        gtk_list_store_append (GTK_LIST_STORE (model), &iter);
        gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                            0, lightdm_user_get_name (user),
                            1, lightdm_user_get_display_name (user),
                            2, logged_in ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                            -1);
    }
    if (lightdm_greeter_get_has_guest_account_hint (greeter))
    {
        gtk_list_store_append (GTK_LIST_STORE (model), &iter);
        gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                            0, "*guest",
                            1, _("Guest Session"),
                            2, PANGO_WEIGHT_NORMAL,
                            -1);
    }

    gtk_list_store_append (GTK_LIST_STORE (model), &iter);
    gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                        0, "*other",
                        1, _("Other..."),
                        2, PANGO_WEIGHT_NORMAL,
                        -1);

    last_user = g_key_file_get_value (state, "greeter", "last-user", NULL);

    if (lightdm_greeter_get_select_user_hint (greeter))
        selected_user = lightdm_greeter_get_select_user_hint (greeter);
    else if (lightdm_greeter_get_select_guest_hint (greeter))
        selected_user = "*guest";
    else if (last_user)
        selected_user = last_user;
    else
        selected_user = NULL;

    if (gtk_tree_model_get_iter_first (model, &iter))
    {
        gchar *name;
        gboolean matched = FALSE;
        
        if (selected_user)
        {
            do
            {
                gtk_tree_model_get (model, &iter, 0, &name, -1);
                matched = g_strcmp0 (name, selected_user) == 0;
                g_free (name);
                if (matched)
                {
                    gtk_combo_box_set_active_iter (user_combo, &iter);
                    name = g_strdup(selected_user);
                    set_displayed_user(greeter, name);
                    g_free(name);
                    break;
                }
            } while (gtk_tree_model_iter_next (model, &iter));
        }
        if (!matched)
        {
            gtk_tree_model_get_iter_first (model, &iter);
            gtk_tree_model_get (model, &iter, 0, &name, -1);
            gtk_combo_box_set_active_iter (user_combo, &iter);
            set_displayed_user(greeter, name);
            g_free(name);
        }
        
    }

    g_free (last_user);
}

/* The following code for setting a RetainPermanent background pixmap was taken
   originally from Gnome, with some fixes from MATE. see:
   https://github.com/mate-desktop/mate-desktop/blob/master/libmate-desktop/mate-bg.c */
static cairo_surface_t *
create_root_surface (GdkScreen *screen)
{
    gint number, width, height;
    Display *display;
    Pixmap pixmap;
    cairo_surface_t *surface;

    number = gdk_screen_get_number (screen);
    width = gdk_screen_get_width (screen);
    height = gdk_screen_get_height (screen);

    /* Open a new connection so with Retain Permanent so the pixmap remains when the greeter quits */
    gdk_flush ();
    display = XOpenDisplay (gdk_display_get_name (gdk_screen_get_display (screen)));
    if (!display)
    {
        g_warning ("Failed to create root pixmap");
        return NULL;
    }

    XSetCloseDownMode (display, RetainPermanent);
    pixmap = XCreatePixmap (display, RootWindow (display, number), width, height, DefaultDepth (display, number));
    XCloseDisplay (display);

    /* Convert into a Cairo surface */
    surface = cairo_xlib_surface_create (GDK_SCREEN_XDISPLAY (screen),
                                         pixmap,
                                         GDK_VISUAL_XVISUAL (gdk_screen_get_system_visual (screen)),
                                         width, height);

    return surface;
}

/* Sets the "ESETROOT_PMAP_ID" property to later be used to free the pixmap,
*/
static void
set_root_pixmap_id (GdkScreen *screen,
                         Display *display,
                         Pixmap xpixmap)
{
    Window xroot = RootWindow (display, gdk_screen_get_number (screen));
    char *atom_names[] = {"_XROOTPMAP_ID", "ESETROOT_PMAP_ID"};
    Atom atoms[G_N_ELEMENTS(atom_names)] = {0};

    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data_root, *data_esetroot;

    /* Get atoms for both properties in an array, only if they exist.
     * This method is to avoid multiple round-trips to Xserver
     */
    if (XInternAtoms (display, atom_names, G_N_ELEMENTS(atom_names), True, atoms) &&
        atoms[0] != None && atoms[1] != None)
    {

        XGetWindowProperty (display, xroot, atoms[0], 0L, 1L, False, AnyPropertyType,
                            &type, &format, &nitems, &after, &data_root);
        if (data_root && type == XA_PIXMAP && format == 32 && nitems == 1)
        {
            XGetWindowProperty (display, xroot, atoms[1], 0L, 1L, False, AnyPropertyType,
                                &type, &format, &nitems, &after, &data_esetroot);
            if (data_esetroot && type == XA_PIXMAP && format == 32 && nitems == 1)
            {
                Pixmap xrootpmap = *((Pixmap *) data_root);
                Pixmap esetrootpmap = *((Pixmap *) data_esetroot);
                XFree (data_root);
                XFree (data_esetroot);

                gdk_error_trap_push ();
                if (xrootpmap && xrootpmap == esetrootpmap) {
                    XKillClient (display, xrootpmap);
                }
                if (esetrootpmap && esetrootpmap != xrootpmap) {
                    XKillClient (display, esetrootpmap);
                }

                XSync (display, False);
                gdk_error_trap_pop ();

            }
        }
    }

    /* Get atoms for both properties in an array, create them if needed.
     * This method is to avoid multiple round-trips to Xserver
     */
    if (!XInternAtoms (display, atom_names, G_N_ELEMENTS(atom_names), False, atoms) ||
        atoms[0] == None || atoms[1] == None) {
        g_warning("Could not create atoms needed to set root pixmap id/properties.\n");
        return;
    }

    /* Set new _XROOTMAP_ID and ESETROOT_PMAP_ID properties */
    XChangeProperty (display, xroot, atoms[0], XA_PIXMAP, 32,
                     PropModeReplace, (unsigned char *) &xpixmap, 1);

    XChangeProperty (display, xroot, atoms[1], XA_PIXMAP, 32,
                     PropModeReplace, (unsigned char *) &xpixmap, 1);
}

/**
* set_surface_as_root:
* @screen: the #GdkScreen to change root background on
* @surface: the #cairo_surface_t to set root background from.
* Must be an xlib surface backing a pixmap.
*
* Set the root pixmap, and properties pointing to it. We
* do this atomically with a server grab to make sure that
* we won't leak the pixmap if somebody else it setting
* it at the same time. (This assumes that they follow the
* same conventions we do). @surface should come from a call
* to create_root_surface().
**/
static void
set_surface_as_root (GdkScreen *screen, cairo_surface_t *surface)
{
    g_return_if_fail (cairo_surface_get_type (surface) == CAIRO_SURFACE_TYPE_XLIB);

    /* Desktop background pixmap should be created from dummy X client since most
     * applications will try to kill it with XKillClient later when changing pixmap
     */
    Display *display = GDK_DISPLAY_XDISPLAY (gdk_screen_get_display (screen));
    Pixmap pixmap_id = cairo_xlib_surface_get_drawable (surface);
    Window xroot = RootWindow (display, gdk_screen_get_number (screen));

    XGrabServer (display);

    XSetWindowBackgroundPixmap (display, xroot, pixmap_id);
    set_root_pixmap_id (screen, display, pixmap_id);
    XClearWindow (display, xroot);

    XFlush (display);
    XUngrabServer (display);
}

static void
set_background (GdkPixbuf *new_bg)
{
    GdkRectangle monitor_geometry;
    GdkPixbuf *bg = NULL;
    GSList *iter;
    gint i, num_screens = 1;

    if (new_bg)
        bg = new_bg;
    else
        bg = default_background_pixbuf;

    #if GDK_VERSION_CUR_STABLE < G_ENCODE_VERSION(3, 10)
        num_screens = gdk_display_get_n_screens (gdk_display_get_default ());
    #endif

    /* Set the background */
    for (i = 0; i < num_screens; i++)
    {
        GdkScreen *screen;
        cairo_surface_t *surface;
        cairo_t *c;
        gint monitor;

        screen = gdk_display_get_screen (gdk_display_get_default (), i);
        surface = create_root_surface (screen);
        c = cairo_create (surface);

        for (monitor = 0; monitor < gdk_screen_get_n_monitors (screen); monitor++)
        {
            gdk_screen_get_monitor_geometry (screen, monitor, &monitor_geometry);
            draw_background (c, bg, monitor_geometry.width, monitor_geometry.height);
            iter = g_slist_nth (backgrounds, monitor);
            gtk_widget_queue_draw (GTK_WIDGET (iter->data));
        }

        cairo_destroy (c);

        /* Refresh background */
        gdk_flush ();
        set_surface_as_root (screen, surface);
        cairo_surface_destroy (surface);
    }
    gtk_widget_queue_draw (GTK_WIDGET (login_window));
}

static gboolean
read_position_from_str (const gchar *s, DimensionPosition *x)
{
    DimensionPosition p;
    gchar *end = NULL;
    gchar **parts = g_strsplit(s, ",", 2);
    if (parts[0])
    {
        p.value = g_ascii_strtoll(parts[0], &end, 10);
        p.percentage = end && end[0] == '%';
        p.sign = (p.value < 0 || (p.value == 0 && parts[0][0] == '-')) ? -1 : +1;
        if (p.value < 0)
            p.value *= -1;
        if (g_strcmp0(parts[1], "start") == 0)
            p.anchor = -1;
        else if (g_strcmp0(parts[1], "center") == 0)
            p.anchor = 0;
        else if (g_strcmp0(parts[1], "end") == 0)
            p.anchor = +1;
        else
            p.anchor = p.sign > 0 ? -1 : +1;
        *x = p;
    }
    else
        x = NULL;
    g_strfreev (parts);
    return x != NULL;
}

static GdkFilterReturn
focus_upon_map (GdkXEvent *gxevent, GdkEvent *event, gpointer  data)
{
    XEvent* xevent = (XEvent*)gxevent;
    GdkWindow* keyboard_win = onboard_window ? gtk_widget_get_window (GTK_WIDGET (onboard_window)) : NULL;
    if (xevent->type == MapNotify)
    {
        Window xwin = xevent->xmap.window;
        Window keyboard_xid = 0;
        GdkDisplay* display = gdk_x11_lookup_xdisplay (xevent->xmap.display);
        GdkWindow* win = gdk_x11_window_foreign_new_for_display (display, xwin);
        GdkWindowTypeHint win_type = gdk_window_get_type_hint (win);

        /* Check to see if this window is our onboard window, since we don't want to focus it. */
        if (keyboard_win)
                keyboard_xid = gdk_x11_drawable_get_xid (keyboard_win);

        if (xwin != keyboard_xid
            && win_type != GDK_WINDOW_TYPE_HINT_TOOLTIP
            && win_type != GDK_WINDOW_TYPE_HINT_NOTIFICATION)
        {
            gdk_window_focus (win, GDK_CURRENT_TIME);
            /* Make sure to keep keyboard above */
            if (onboard_window)
            {
                if (keyboard_win)
                    gdk_window_raise (keyboard_win);
            }
        }
    }
    else if (xevent->type == UnmapNotify)
    {
        Window xwin;
        int revert_to;
        XGetInputFocus (xevent->xunmap.display, &xwin, &revert_to);

        if (revert_to == RevertToNone)
        {
            gdk_window_focus (gtk_widget_get_window (GTK_WIDGET (login_window)), GDK_CURRENT_TIME);
            /* Make sure to keep keyboard above */
            if (onboard_window)
            {
                if (keyboard_win)
                    gdk_window_raise (keyboard_win);
            }
        }
    }
    return GDK_FILTER_CONTINUE;
}

int
main (int argc, char **argv)
{
    GKeyFile *config, *userconf;
    GdkRectangle monitor_geometry;
    GtkBuilder *builder;
    GtkCellRenderer *renderer;
    GtkWidget *infobar_compat, *content_area;
    gchar *value, *state_dir;
    GdkColor background_color;
    GError *error = NULL;

    /* Background windows */
    gint monitor, scr;
    gint numScreens = 1;
    GdkScreen *screen;
    GtkWidget *window;

    Display* display;

    #ifdef START_INDICATOR_SERVICES
    GPid indicator_pid = 0, spi_pid = 0;
    #endif

    /* Prevent memory from being swapped out, as we are dealing with passwords */
    mlockall (MCL_CURRENT | MCL_FUTURE);

    /* Disable global menus */
    g_unsetenv ("UBUNTU_MENUPROXY");

    /* Initialize i18n */
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    g_unix_signal_add(SIGTERM, (GSourceFunc)gtk_main_quit, NULL);

    /* init threads */
    gdk_threads_init();

    /* init gtk */
    gtk_init (&argc, &argv);
    
#ifdef HAVE_LIBIDO
    ido_init ();
#endif

    config = g_key_file_new ();
    g_key_file_load_from_file (config, CONFIG_FILE, G_KEY_FILE_NONE, &error);
    if (error && !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Failed to load configuration from %s: %s\n", CONFIG_FILE, error->message);
    g_clear_error (&error);

    state_dir = g_build_filename (g_get_user_cache_dir (), "pi-greeter", NULL);
    g_mkdir_with_parents (state_dir, 0775);
    state_filename = g_build_filename (state_dir, "state", NULL);
    g_free (state_dir);

    state = g_key_file_new ();
    g_key_file_load_from_file (state, state_filename, G_KEY_FILE_NONE, &error);
    if (error && !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Failed to load state from %s: %s\n", state_filename, error->message);
    g_clear_error (&error);

    greeter = lightdm_greeter_new ();
    g_signal_connect (greeter, "show-prompt", G_CALLBACK (show_prompt_cb), NULL);  
    g_signal_connect (greeter, "show-message", G_CALLBACK (show_message_cb), NULL);
    g_signal_connect (greeter, "authentication-complete", G_CALLBACK (authentication_complete_cb), NULL);
    g_signal_connect (greeter, "autologin-timer-expired", G_CALLBACK (lightdm_greeter_authenticate_autologin), NULL);
    if (!lightdm_greeter_connect_sync (greeter, NULL))
        return EXIT_FAILURE;

    /* Set default cursor */
    gdk_window_set_cursor (gdk_get_default_root_window (), gdk_cursor_new (GDK_LEFT_PTR));

    /* Create the pcmanfm config file path */
    gchar *user, *session;
    user = g_key_file_get_value (config, "greeter", "user", NULL);
    session = g_key_file_get_value (config, "greeter", "session", NULL);
    gchar buffer[256];
    sprintf (buffer, "/home/%s/.config/pcmanfm/%s/desktop-items-0.conf", user, session);
    userconf = g_key_file_new ();
    g_key_file_load_from_file (userconf, buffer, G_KEY_FILE_NONE, &error);
    if (error && !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Failed to load user configuration from %s: %s\n", buffer, error->message);
    g_clear_error (&error);

    /* Get background colour from pcmanfm settings */
    value = g_key_file_get_value (userconf, "*", "desktop_bg", NULL);
    if (!value || !gdk_color_parse (value, &background_color))
            gdk_color_parse ("#C0C0C0", &background_color);
    if (value) g_free (value);
    default_background_color = gdk_color_copy (&background_color);

    /* Get background image from pcmanfm settings */
    value = g_key_file_get_value (userconf, "*", "wallpaper", NULL);
    if (value)
    {
        GError *error = NULL;
        g_debug ("Loading background %s", value);
        // Need to add an alpha channel here to make redraw work properly...
        default_background_pixbuf = gdk_pixbuf_add_alpha (gdk_pixbuf_new_from_file (value, &error), FALSE, 0, 0, 0);
        if (!default_background_pixbuf)
            g_warning ("Failed to load background: %s", error->message);
        g_clear_error (&error);
        g_free (value);
    }

    /* Get display mode from pcmanfm settings */
    value = g_key_file_get_value (userconf, "*", "wallpaper_mode", NULL);
    if (value)
    {
        wp_mode = g_strdup (value);
        g_free (value);
    }
    else wp_mode = g_strdup ("color");

    /* Make the greeter behave a bit more like a screensaver if used as un/lock-screen by blanking the screen */
    gchar* end_ptr = NULL;
    int screensaver_timeout = 60;
    value = g_key_file_get_value (config, "greeter", "screensaver-timeout", NULL);
    if (value)
        screensaver_timeout = g_ascii_strtoll (value, &end_ptr, 0);
    g_free (value);
    
    display = gdk_x11_display_get_xdisplay(gdk_display_get_default ());
    if (lightdm_greeter_get_lock_hint (greeter)) {
        XGetScreenSaver(display, &timeout, &interval, &prefer_blanking, &allow_exposures);
        XForceScreenSaver(display, ScreenSaverActive);
        XSetScreenSaver(display, screensaver_timeout, 0, ScreenSaverActive, DefaultExposures);
    }

    /* Set GTK+ settings */
    value = g_key_file_get_value (config, "greeter", "theme-name", NULL);
    if (value)
    {
        g_debug ("Using Gtk+ theme %s", value);
        g_object_set (gtk_settings_get_default (), "gtk-theme-name", value, NULL);
    }
    g_free (value);
    g_object_get (gtk_settings_get_default (), "gtk-theme-name", &default_theme_name, NULL);
    g_debug ("Default Gtk+ theme is '%s'", default_theme_name);

    value = g_key_file_get_value (config, "greeter", "icon-theme-name", NULL);
    if (value)
    {
        g_debug ("Using icon theme %s", value);
        g_object_set (gtk_settings_get_default (), "gtk-icon-theme-name", value, NULL);
    }
    g_free (value);
    g_object_get (gtk_settings_get_default (), "gtk-icon-theme-name", &default_icon_theme_name, NULL);
    g_debug ("Default theme is '%s'", default_icon_theme_name);

    value = g_key_file_get_value (config, "greeter", "font-name", NULL);
    if (value)
    {
        g_debug ("Using font %s", value);
        g_object_set (gtk_settings_get_default (), "gtk-font-name", value, NULL);
    }
    else
    {
        value = g_strdup("Sans 10");
        g_object_set (gtk_settings_get_default (), "gtk-font-name", value, NULL);
    }
    g_object_get (gtk_settings_get_default (), "gtk-font-name", &default_font_name, NULL);  
    //value = g_key_file_get_value (config, "greeter", "xft-dpi", NULL);
    //if (value)
    //    g_object_set (gtk_settings_get_default (), "gtk-xft-dpi", (int) (1024 * atof (value)), NULL);
    //value = g_key_file_get_value (config, "greeter", "xft-antialias", NULL);
    //if (value)
    //    g_object_set (gtk_settings_get_default (), "gtk-xft-antialias", g_strcmp0 (value, "true") == 0, NULL);
    //g_free (value);
    //value = g_key_file_get_value (config, "greeter", "xft-hintstyle", NULL);
    //if (value)
    //    g_object_set (gtk_settings_get_default (), "gtk-xft-hintstyle", value, NULL);
    //g_free (value);
    //value = g_key_file_get_value (config, "greeter", "xft-rgba", NULL);
    //if (value)
    //    g_object_set (gtk_settings_get_default (), "gtk-xft-rgba", value, NULL);
    //g_free (value);
    
    /* Get a11y on screen keyboard command*/
    gint argp;
    value = g_key_file_get_value (config, "greeter", "keyboard", NULL);
    g_debug ("a11y keyboard command is '%s'", value);
    /* Set NULL to blank to avoid warnings */
    if (!value) { value = g_strdup(""); }
    g_shell_parse_argv (value, &argp, &a11y_keyboard_command, NULL);
    g_free (value);

    builder = gtk_builder_new ();
	gtk_builder_add_from_file (builder, GREETER_DATA_DIR "/pi-greeter.glade", NULL);
    
    /* Login window */
    login_window = GTK_WINDOW (gtk_builder_get_object (builder, "login_window"));
    user_image = GTK_IMAGE (gtk_builder_get_object (builder, "user_image"));
    user_combo = GTK_COMBO_BOX (gtk_builder_get_object (builder, "user_combobox"));
    username_entry = GTK_ENTRY (gtk_builder_get_object (builder, "username_entry"));
    password_entry = GTK_ENTRY (gtk_builder_get_object (builder, "password_entry"));

    /* Add InfoBar via code for GTK+2 compatability */
    infobar_compat = GTK_WIDGET(gtk_builder_get_object(builder, "infobar_compat"));
    info_bar = GTK_INFO_BAR (gtk_info_bar_new());
    gtk_info_bar_set_message_type(info_bar, GTK_MESSAGE_ERROR);
    gtk_widget_set_name(GTK_WIDGET(info_bar), "greeter_infobar");
    content_area = gtk_info_bar_get_content_area(info_bar);

    message_label = GTK_LABEL (gtk_builder_get_object (builder, "message_label"));
    g_object_ref(message_label);
    gtk_container_remove(GTK_CONTAINER(infobar_compat), GTK_WIDGET(message_label));
    gtk_container_add(GTK_CONTAINER(content_area), GTK_WIDGET(message_label));
    g_object_unref(message_label);

    gtk_container_add(GTK_CONTAINER(infobar_compat), GTK_WIDGET(info_bar));

    cancel_button = GTK_BUTTON (gtk_builder_get_object (builder, "cancel_button"));
    login_button = GTK_BUTTON (gtk_builder_get_object (builder, "login_button"));

    g_signal_connect (G_OBJECT (login_window), "size-allocate", G_CALLBACK (login_window_size_allocate), NULL);

    /* To maintain compatability with GTK+2, set special properties here */
    gtk_container_set_border_width (GTK_CONTAINER(gtk_builder_get_object (builder, "vbox2")), 18);
    gtk_container_set_border_width (GTK_CONTAINER(gtk_builder_get_object (builder, "content_frame")), 14);
    gtk_container_set_border_width (GTK_CONTAINER(gtk_builder_get_object (builder, "buttonbox_frame")), 8);
    gtk_widget_set_tooltip_text(GTK_WIDGET(password_entry), _("Enter your password"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(username_entry), _("Enter your username"));

#ifdef START_INDICATOR_SERVICES
    init_indicators (config, &indicator_pid, &spi_pid);
#else
    init_indicators (config);
#endif

    value = g_key_file_get_value (config, "greeter", "default-user-image", NULL);
    if (value)
    {
        if (value[0] == '#')
            default_user_icon = g_strdup (value + 1);
        else
        {
            default_user_pixbuf = gdk_pixbuf_new_from_file (value, &error);
            if (!default_user_pixbuf)
            {
                g_warning ("Failed to load default user image: %s", error->message);
                g_clear_error (&error);
            }
        }
        g_free (value);
    }

    /* Users combobox */
    renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (user_combo), renderer, TRUE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (user_combo), renderer, "text", 1);
    //gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (user_combo), renderer, "weight", 2);

    #if GDK_VERSION_CUR_STABLE < G_ENCODE_VERSION(3, 10)
        numScreens = gdk_display_get_n_screens (gdk_display_get_default());
    #endif

    /* Set up the background images */	
    for (scr = 0; scr < numScreens; scr++)
    {
        screen = gdk_display_get_screen (gdk_display_get_default (), scr);
        for (monitor = 0; monitor < gdk_screen_get_n_monitors (screen); monitor++)
        {
            gdk_screen_get_monitor_geometry (screen, monitor, &monitor_geometry);
        
            window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
            gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_DESKTOP);
            gtk_widget_modify_bg(GTK_WIDGET(window), GTK_STATE_NORMAL, &background_color);
            gtk_window_set_screen(GTK_WINDOW(window), screen);
            gtk_window_set_keep_below(GTK_WINDOW(window), TRUE);
            gtk_widget_set_size_request(window, monitor_geometry.width, monitor_geometry.height);
            gtk_window_set_resizable (GTK_WINDOW(window), FALSE);
            gtk_widget_set_app_paintable (GTK_WIDGET(window), TRUE);
            gtk_window_move (GTK_WINDOW(window), monitor_geometry.x, monitor_geometry.y);

            backgrounds = g_slist_prepend(backgrounds, window);
            gtk_widget_show (window);
            g_signal_connect (G_OBJECT (window), "expose-event", G_CALLBACK (background_window_expose), NULL);
            gtk_widget_queue_draw (GTK_WIDGET(window));
        }
    }
    backgrounds = g_slist_reverse(backgrounds);

    if (lightdm_greeter_get_hide_users_hint (greeter))
    {
        /* Set the background to default */
        set_background (NULL);
        start_authentication ("*other");
    }
    else
    {
        /* This also sets the background to user's */
        load_user_list ();
        gtk_widget_show (GTK_WIDGET (cancel_button));
        gtk_widget_show (GTK_WIDGET (user_combo));
    }

    /* Window position */
    /* Default: x-center, y-center */
    main_window_pos = CENTERED_WINDOW_POS;
    value = g_key_file_get_value (config, "greeter", "position", NULL);
    if (value)
    {
        gchar *x = value;
        gchar *y = strchr(value, ' ');
        if (y)
            (y++)[0] = '\0';
        
        if (read_position_from_str (x, &main_window_pos.x))
            /* If there is no y-part then y = x */
            if (!y || !read_position_from_str (y, &main_window_pos.y))
                main_window_pos.y = main_window_pos.x;

        g_free (value);
    }

    gtk_builder_connect_signals(builder, greeter);

    gtk_widget_show (GTK_WIDGET (login_window));
    center_window (login_window,  NULL, &main_window_pos);
    g_signal_connect (GTK_WIDGET (login_window), "size-allocate", G_CALLBACK (center_window), &main_window_pos);

    gtk_widget_show (GTK_WIDGET (login_window));
    gdk_window_focus (gtk_widget_get_window (GTK_WIDGET (login_window)), GDK_CURRENT_TIME);

    /* focus fix (source: unity-greeter) */
    GdkWindow* root_window = gdk_get_default_root_window ();
    gdk_window_set_events (root_window, gdk_window_get_events (root_window) | GDK_SUBSTRUCTURE_MASK);
    gdk_window_add_filter (root_window, focus_upon_map, NULL);

    gdk_threads_enter();
    gtk_main ();
    gdk_threads_leave();

#ifdef START_INDICATOR_SERVICES
    if (indicator_pid)
    {
		kill (indicator_pid, SIGTERM);
		waitpid (indicator_pid, NULL, 0);
    }

    if (spi_pid)
    {
		kill (spi_pid, SIGTERM);
		waitpid (spi_pid, NULL, 0);
    }
#endif

    if (default_background_pixbuf)
        g_object_unref (default_background_pixbuf);
    if (default_background_color)
        gdk_color_free (default_background_color);

    {
	int screen = XDefaultScreen (display);
	Window w = RootWindow (display, screen);
	Atom id = XInternAtom (display, "AT_SPI_BUS", True);
	if (id != None)
	    {
		XDeleteProperty (display, w, id);
		XSync (display, FALSE);
	    }
    }

    return EXIT_SUCCESS;
}
