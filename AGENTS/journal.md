# Development journal

## 2026-10-28 — Preserve unchanged tray menus

The tray rebuilt every row on each AboutToShow, forcing AppIndicator to repopulate the visible menu through staggered idle callbacks. Compare complete row models (including pad identity and enabled state), preserve unchanged rows/IDs/revision, and rebuild only on a real change. The private-bus regression checks repeat opens, cached action dispatch, enabled-state invalidation and rejection of retired IDs. C build and all five test executables pass.
