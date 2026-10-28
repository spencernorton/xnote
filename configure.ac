# Process this file with autoconf to produce a configure script.

# Sets up autoconf.
AC_INIT([XNote],[3.3.2],[https://github.com/spencernorton/xnote/issues])
AC_CONFIG_SRCDIR(src/xpad-app.c)
AC_CONFIG_HEADERS([config.h:config.h.in])
AC_CONFIG_MACRO_DIRS([m4])

# Checks for programs.
AC_USE_SYSTEM_EXTENSIONS
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET

# Optional Go toolchain for the E2EE backup helper.
# If absent, the C build proceeds normally but xnote-cloud-backup is not built.
AC_CHECK_PROG([GO], [go], [go])
AM_CONDITIONAL([HAVE_GO], [test -n "$GO"])

# Sets up automake.
AM_INIT_AUTOMAKE([1.9 foreign no-dist-gzip dist-bzip2])
AM_BINRELOC


# Checks for GTK 4 + libadwaita.
# GTK 4 floor 4.10 gives us GtkWidget child-iteration, set_draw_func, the
# modern event-controller model, and AdwMessageDialog. libadwaita 1.5 is the
# Adw floor: Preferences is an AdwPreferencesDialog (AdwDialog is 1.5). Accessibility is built into GTK 4 (GtkAccessible),
# so the old ATK / atk-bridge checks are gone.
PKG_CHECK_MODULES(GTK, gtk4 >= 4.10 gio-2.0 >= 2.66 libadwaita-1 >= 1.5)
AM_CONDITIONAL(GTK, test -n "$GTK_LIBS")
AC_SUBST(GTK_LIBS)
AC_SUBST(GTK_CFLAGS)

# GtkSourceView 5 is the GTK 4 series of the source-editing widget.
PKG_CHECK_MODULES(GTK_SOURCE_VIEW, gtksourceview-5 >= 5.0)
AC_SUBST(GTK_SOURCE_VIEW_LIBS)
AC_SUBST(GTK_SOURCE_VIEW_CFLAGS)

# GLib floor 2.66 for the modern GMenu / GDBus surface used by the menus
# and the StatusNotifierItem tray.
PKG_CHECK_MODULES(GLIB, glib-2.0 >= 2.66)
AC_SUBST(GLIB_LIBS)
AC_SUBST(GLIB_CFLAGS)

PKG_CHECK_MODULES(PANGO, pango >= 1.32)
AC_SUBST(PANGO_LIBS)
AC_SUBST(PANGO_CFLAGS)

PKG_CHECK_MODULES(PIXBUF, gdk-pixbuf-2.0 >= 2.28)
AC_SUBST(PIXBUF_LIBS)
AC_SUBST(PIXBUF_CFLAGS)

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_SIZE_T

# i18n / text translations
# The choice for version of gettext is only based on what was installed on the developers desktop (2021).
# If another version is more convenient, go for it.
AM_GNU_GETTEXT_VERSION([0.21])
AM_GNU_GETTEXT([external])

GETTEXT_PACKAGE="$PACKAGE"
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE, "$GETTEXT_PACKAGE", [gettext domain])

# Tray / system status icon.
#
# GTK 4 dropped GtkStatusIcon and there is no GTK-4 AppIndicator binding, so
# the tray is now implemented directly against the freedesktop
# StatusNotifierItem D-Bus protocol via GDBus (see xpad-tray.c). That needs no
# extra build-time dependency beyond gio-2.0, which GTK already pulls in.
# The old ayatana-appindicator3 check is therefore gone.

# Sets up a few prefix-relative directories
if test "x${prefix}" = "xNONE"; then
  PREFIX="${ac_default_prefix}"
else
  PREFIX="${prefix}"
fi

# Define the help path and filename
HELP_DIR="$PACKAGE/help"
HELP_FILE_NAME="xnote-user-help.txt"
AC_SUBST(HELP_DIR)
AC_SUBST(HELP_FILE_NAME)

#Define the locale path
LOCALE_DIR="locale"
AC_SUBST(LOCALE_DIR)

#Define the icon path
ICON_PATH="icons"
AC_SUBST(THEME_DIR)

# Determines if we're debugging.
AC_ARG_ENABLE(debug,
  AS_HELP_STRING([--enable-debug=[no/yes]],[turn on debugging [default=no]]),
  enable_debug="$enableval",
  enable_debug=no)
if test "$enable_debug" = "yes"; then
  DEBUG_CFLAGS="-std=c99 -Wall -Werror -g -O0 -DG_DISABLE_DEPRECATED -DGDK_DISABLE_DEPRECATED -DGDK_PIXBUF_DISABLE_DEPRECATED -DGTK_DISABLE_DEPRECATED -DGTK_MULTIHEAD_SAFE -DGTK_DISABLE_SINGLE_INCLUDES -DGSEAL_ENABLE"
else
  if test "$enable_debug" = "most"; then
    DEBUG_CFLAGS="-std=c99 -Wall -Werror -g -O0 -DG_DISABLE_DEPRECATED -DGDK_DISABLE_DEPRECATED -DGDK_PIXBUF_DISABLE_DEPRECATED -DGTK_MULTIHEAD_SAFE -DGTK_DISABLE_SINGLE_INCLUDES -DGSEAL_ENABLE -Wno-deprecated-declarations"
  else
    DEBUG_CFLAGS="-Wno-deprecated-declarations"
  fi
fi
AC_SUBST(DEBUG_CFLAGS)

# Sets up output files.
AC_CONFIG_FILES([
Makefile
data/Makefile
doc/Makefile
images/Makefile
po/Makefile.in
src/Makefile
tests/Makefile
])

AC_OUTPUT
