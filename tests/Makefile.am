# tests/Makefile.am — GLib test suite for xpad
#
# test-escape:   verifies the U+E000/E001 escape scheme in xpad-text-buffer.c.
#                Only links GLib (no GtkSourceBuffer/XpadPad needed).
#
# test-fio:      verifies fio key/value write→read round-trip and the
#                byte order of the info-* names pads open in.
#                Links GLib + GIO (fio.c uses GFile/GIO).
#
# test-settings: verifies the declarative option registry round-trips every
#                plain bool/uint setting through fio save→load, and that the
#                registry table contains exactly the expected fio key strings.
#                Links GTK (needed for GdkRGBA in XpadSettings), plus
#                xpad-settings.c, fio.c, and constants.c.
#
# test-autostart: verifies enabling autostart creates a package-backed symlink,
#                 legacy wait-systray changes cannot replace it with a regular
#                 file, and mkdir failures report an error without crashing.
#
# test-tray-lifecycle: uses a private session bus and a pad-group double to
#                      exercise queued callbacks across rapid tray toggles and
#                      the no-headless reachability guard.

AM_CFLAGS = \
	@GTK_CFLAGS@ \
	@GTK_SOURCE_VIEW_CFLAGS@ \
	@DEBUG_CFLAGS@ \
	-I$(top_srcdir)/src \
	-I$(top_builddir)

check_PROGRAMS = test-escape test-fio test-settings test-autostart test-tray-lifecycle test-styling

TESTS = test-escape test-fio test-settings test-autostart test-tray-lifecycle test-styling

# test-escape: compile only xpad-text-buffer.c (escape helpers) + GLib.
test_escape_SOURCES = test-escape.c ../src/xpad-text-buffer.c
test_escape_CFLAGS  = $(AM_CFLAGS)
test_escape_LDADD   = @GTK_LIBS@ @GTK_SOURCE_VIEW_LIBS@

# test-fio: compile fio.c with stubs for xpad_app_*.
test_fio_SOURCES = test-fio.c ../src/fio.c
test_fio_CFLAGS  = $(AM_CFLAGS)
test_fio_LDADD   = @GTK_LIBS@

# test-settings: compile xpad-settings.c + its deps (fio.c, constants.c).
# xpad-settings.c uses GdkRGBA so links GTK.
test_settings_SOURCES = test-settings.c \
	../src/xpad-settings.c \
	../src/fio.c \
	../src/constants.c
test_settings_CFLAGS  = $(AM_CFLAGS) -DDATADIR=\"/tmp\"
test_settings_LDADD   = @GTK_LIBS@

# test-autostart: compile the same settings core with isolated XDG paths.
test_autostart_SOURCES = test-autostart.c \
	../src/xpad-settings.c \
	../src/fio.c \
	../src/constants.c
test_autostart_CFLAGS  = $(AM_CFLAGS) -DDATADIR=\"/tmp\" -DXPAD_SETTINGS_TESTING
test_autostart_LDADD   = @GTK_LIBS@

# test-tray-lifecycle: compile the tray and settings cores against a minimal
# process-global pad-group double; GTestDBus supplies the session bus.
test_tray_lifecycle_SOURCES = test-tray-lifecycle.c \
	../src/xpad-tray.c \
	../src/xpad-settings.c \
	../src/fio.c \
	../src/constants.c
test_tray_lifecycle_CFLAGS  = $(AM_CFLAGS) -DDATADIR=\"/tmp\"
test_tray_lifecycle_LDADD   = @GTK_LIBS@

# test-styling: verifies pango_font_description_to_css() px vs pt handling.
test_styling_SOURCES = test-styling.c \
	../src/xpad-styling-helpers.c
test_styling_CFLAGS  = $(AM_CFLAGS)
test_styling_LDADD   = @GTK_LIBS@
