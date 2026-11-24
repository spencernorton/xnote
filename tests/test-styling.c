/*
 * test-styling.c — unit tests for pango_font_description_to_css().
 *
 * Verifies point vs absolute-pixel font sizes so per-note fonts saved by
 * GTK 4's Pango context ("Sans 21.333px") round-trip without 1.33x inflation
 * and format with a locale-independent decimal point.
 */

#include <locale.h>
#include <gtk/gtk.h>
#include "xpad-styling-helpers.h"

static void
test_point_size (void)
{
	PangoFontDescription *desc = pango_font_description_from_string ("Sans 16");
	gchar *css = pango_font_description_to_css (desc);
	g_assert_nonnull (strstr (css, "font-family: Sans;"));
	g_assert_nonnull (strstr (css, "font-size: 16pt;"));
	g_free (css);
	pango_font_description_free (desc);
}

static void
test_absolute_px_size (void)
{
	PangoFontDescription *desc = pango_font_description_from_string ("Sans 21.333px");
	g_assert_true (pango_font_description_get_size_is_absolute (desc));
	gchar *css = pango_font_description_to_css (desc);
	g_assert_nonnull (strstr (css, "font-size: 21.333px;"));
	g_assert_null (strstr (css, "pt;"));
	g_free (css);
	pango_font_description_free (desc);
}

static void
test_fractional_point_size (void)
{
	PangoFontDescription *desc = pango_font_description_from_string ("Monospace Bold Italic 10.5");
	g_assert_false (pango_font_description_get_size_is_absolute (desc));
	gchar *css = pango_font_description_to_css (desc);
	g_assert_nonnull (strstr (css, "font-family: Monospace;"));
	g_assert_nonnull (strstr (css, "font-weight: 700;"));
	g_assert_nonnull (strstr (css, "font-style: italic;"));
	g_assert_nonnull (strstr (css, "font-size: 10.5pt;"));
	g_free (css);
	pango_font_description_free (desc);
}

static void
test_integer_px_size (void)
{
	PangoFontDescription *desc = pango_font_description_from_string ("Sans 14px");
	gchar *css = pango_font_description_to_css (desc);
	g_assert_nonnull (strstr (css, "font-size: 14px;"));
	g_free (css);
	pango_font_description_free (desc);
}

static void
test_null_desc (void)
{
	gchar *css = pango_font_description_to_css (NULL);
	g_assert_cmpstr (css, ==, "{  }");
	g_free (css);
}

int
main (int argc, char *argv[])
{
	setlocale (LC_ALL, "");
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/styling/point_size", test_point_size);
	g_test_add_func ("/styling/absolute_px_size", test_absolute_px_size);
	g_test_add_func ("/styling/fractional_point_size", test_fractional_point_size);
	g_test_add_func ("/styling/integer_px_size", test_integer_px_size);
	g_test_add_func ("/styling/null_desc", test_null_desc);

	return g_test_run ();
}
