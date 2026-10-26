# XNote development guide

How the tree is laid out, how to build and test it, the rules that CI
enforces, and how a change becomes a release. [CONTRIBUTING.md](../CONTRIBUTING.md)
is the short form for a first pull request; this is the long form. What the
application does from the user's side is in the [user guide](user-guide.md).

## Repository layout

```
configure.ac, Makefile.am, autogen.sh   autotools; AC_INIT holds the version
src/            the GTK 4 application (C), one program: xnote
cloud-helper/   xnote-cloud-backup, the encrypted backup helper (Go, vendored deps)
tests/          GLib C tests, the Wayland smoke test, the public-release check
data/           xnote.desktop.in and xnote.appdata.xml.in (gettext-merged at make time)
doc/            man pages xnote.1 and xnote-cloud-backup.1, the in-app help text
docs/           this guide, the user guide, OPERATIONS.md, RELEASING.md
images/         the two SVG icons (app icon and symbolic tray icon)
po/             gettext catalogue; po/LINGUAS lists the 28 translations that are built
screenshots/    the README's captures, from an isolated profile with invented notes
debian/         Debian packaging (control, rules, maintainer scripts, systemd user units)
scripts/        build-deb.sh and check-deb.sh (the package build and checks), the privacy check and its test
ci/             install-go-toolchain.sh, the checksummed Go toolchain installer
```

Root files: `README.md`, `CHANGELOG.md` (the release line), `NOTICE`
(provenance and third-party licences), `AUTHORS`, `COPYING` (GPL-3.0-or-later),
`SECURITY.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SUPPORT.md`.

### `src/` — the application

Every file keeps the `xpad-` prefix and every type the `Xpad` prefix; see
[The `xpad_` prefix rule](#the-xpad_-prefix-rule). All ten GObject types are
declared with `G_DEFINE_TYPE_WITH_PRIVATE`.

| File | Responsibility |
|---|---|
| `xpad-app.[ch]` | `main()` and the application shell: gettext/GTK/libadwaita init, config-dir creation and the `~/.config/xpad` → `~/.config/xnote` migration, the single-instance Unix socket (`server`) that forwards `--new`, `--show`, `--hide`, `--toggle`, `--quit` and `--new-from-file` to a running instance, loading the saved pads, the "no tray and nothing visible" startup check, error dialogs, quit |
| `xpad-pad.[ch]` | One note window (`GtkWindow`): composes the text view, scrollbar, search bar, the floating toolbar and resize grip in revealers, the right-click menus (`GMenu` + `GSimpleActionGroup`), keyboard shortcuts, clipboard; owns loading and saving of its `content-*` and `info-*` files, the delayed-save entry points, colour, title sync, close/delete, Ctrl-drag move/resize hand-off to the compositor, the About dialog |
| `xpad-pad-group.[ch]` | The process-wide list of open pads with add/remove signals, show/close/toggle-all, a title-sorted listing for menus, visible-pad count and save-all-unsaved |
| `xpad-pad-properties.[ch]` | The per-pad properties dialog (`GtkDialog`): follow-global versus custom font and colours |
| `xpad-periodic.[ch]` | The save scheduler: a single 4-second `g_timeout` tick that runs queued save-content / save-info callbacks, one slot per pad and signal, so repeated edits coalesce into one write |
| `xpad-backup.[ch]` | Debounces backup triggers (45 s) and spawns `xnote-cloud-backup sync` from `PATH` or `XPAD_CLOUD_BACKUP_HELPER`; flushed at shutdown. The GTK process never touches storage or the network itself |
| `xpad-settings.[ch]` | `XpadSettings`: every global option as a GObject property, persisted to the plain `default-style` file through a declarative registry table (bool/uint options) with colours, font, toolbar buttons and autostart special-cased; the autostart symlink into `~/.config/autostart`; `xpad_settings_get_effective_startup_display()` |
| `xpad-preferences.[ch]` | The Preferences dialog (`AdwPreferencesDialog`, five pages), mostly `g_object_bind_property` bindings onto `XpadSettings` |
| `xpad-text-buffer.[ch]` | `GtkSourceBuffer` subclass: the bold/italic/underline/strikethrough tags, undo/redo freeze and thaw, and the on-disk serialisation (segments delimited by U+E000 with a U+E001 escape) |
| `xpad-text-view.[ch]` | `GtkSourceView` subclass for the note text: follow-global versus per-pad font and colours applied through a `GtkCssProvider`; the read-only click behaviour |
| `xpad-toolbar.[ch]` | The floating toolbar as a horizontal `GtkBox`: buttons built from the settings' button list, enable/disable for undo/redo/cut/copy/paste, the colour swatch and palette popover, the right-click add/remove popover |
| `xpad-search-bar.[ch]` | The in-note find bar: a `GtkWidget` composing `GtkSearchBar` + `GtkSearchEntry` over a `GtkSourceSearchContext` (`GtkSearchBar` is final in GTK 4, so it is composed, not subclassed) |
| `xpad-grip-tool-item.[ch]` | The resize grip: an 18 × 18 `GtkDrawingArea` that paints a corner handle and starts an interactive resize on press |
| `xpad-tray.[ch]` | The tray icon, implemented directly against `org.kde.StatusNotifierItem` and `com.canonical.dbusmenu` over GDBus (introspection XML inline): owns `org.kde.StatusNotifierItem-<pid>-1`, registers with the watcher and re-registers when it reappears, rebuilds the menu. No AppIndicator library; the only dependency is gio |
| `xpad-styling-helpers.[ch]` | Theme colour lookups and `PangoFontDescription` → CSS conversion |
| `fio.[ch]` | File I/O for the config directory: whole-file read/write, the `key value` line format, unique `prefixXXXXXX` names via `g_mkstemp`, the `info-*` names in the byte order pads open in, a read-error-aware loader. Writes go through `g_file_replace()` with an explicit close, so a failed write surfaces and the pad keeps its dirty flag |
| `help.[ch]` | The Help window: renders `$(datadir)/xnote/help/xnote-user-help.txt` (Pango markup) |
| `constants.[ch]` | Default pad size, font and colours, and the six-entry sticky-note palette |
| `prefix.[ch]` | BinReloc (third-party, LGPL; see `src/COPYING.LESSER`) for relocatable prefix lookup. Not XNote code; do not edit |

### `cloud-helper/` — the backup helper

Module `github.com/spencercnorton/xnote/cloud-helper`, `go 1.26.8`,
dependencies vendored; it builds and tests offline.

| File | Responsibility |
|---|---|
| `main.go` | Command dispatch: `init`, `backup` (`sync` alias), `restore`, `list`, `remote-list`, `import`, `status`, `unlock`, `rotate-passphrase`, `show-recovery` |
| `commands.go` | The implementation of each command |
| `archive.go` | Deterministic tar of the config dir (allowlisted files only) and safe extraction |
| `crypto.go` | The key hierarchy: Argon2id KEK, XChaCha20-Poly1305 wrapping, per-snapshot DEK, HKDF-SHA256, chunked streaming AEAD. Marked "implement exactly"; do not change parameters |
| `keyfile.go` | `keyfile.json`: the master key wrapped under the passphrase and under the recovery code; recovery-code encoding |
| `keyring.go` | Caching the master key in the login keyring by shelling out to `secret-tool` |
| `config.go` | `cloud-backup.conf` parsing and the XDG directories |
| `snapshot.go` | Snapshot header, `head.json` generation counter, snapshot ids, listing, rotation |
| `recovery.go` | Self-checking recovery kits (`manifest.json`) and their validation on import |
| `remote.go` | Append-only transport: filesystem path via `rsync`, or an rclone `name:path` |
| `lock.go`, `status.go`, `terminal.go` | The state lock, the non-secret `remote-status.json`, no-echo passphrase input |
| `*_test.go` | Round-trip, wrong-passphrase, tamper, truncation, recovery-slot, rollback, symlink and remote tests |

## Building from source

Dependencies (from `configure.ac`): GTK 4 ≥ 4.10, GLib/GIO ≥ 2.66,
libadwaita ≥ 1.5, GtkSourceView ≥ 5.0, Pango ≥ 1.32, GdkPixbuf ≥ 2.28,
gettext 0.21. Go is optional: without a `go` executable `configure` skips
the helper and the C build proceeds. With one, `go.mod` requires 1.26.8 and
the build runs with `GOTOOLCHAIN=local GOPROXY=off -mod=vendor`, so an older
Go fails instead of downloading anything. `ci/install-go-toolchain.sh`
installs the exact checksummed release under `/opt/xnote-go-1.26.8`.

```sh
sudo apt install build-essential autoconf automake libtool intltool gettext \
  autopoint pkg-config autoconf-archive libgtk-4-dev libadwaita-1-dev \
  libgtksourceview-5-dev libglib2.0-dev libpango1.0-dev libgdk-pixbuf-2.0-dev

NOCONFIGURE=1 ./autogen.sh        # autoreconf --force --install
./configure --prefix=/usr --enable-debug=most
make -j"$(nproc)"
sudo make install
```

`--enable-debug=most` adds `-Wall -Werror -Wno-deprecated-declarations`
(plus `-std=c99 -g -O0` and the GLib deprecation guards), the flags CI
builds with; `yes` and `no` are the other values. `autogen.sh` is
`autoreconf --force --install` and ignores `NOCONFIGURE`. `make install`
puts `xnote` and (when built) `xnote-cloud-backup` in `$(bindir)`, the
desktop file and AppStream metadata under `$(datadir)`, the icons under
`$(datadir)/icons/hicolor`, the help text under `$(datadir)/xnote/help`, and
the man pages under `$(mandir)`. The desktop file and AppStream metadata are
generated from `data/*.in` by `msgfmt --desktop` / `--xml` at make time (the
metadata installs under its component id, `tech.norvi.xnote.metainfo.xml`,
from the `xnote.appdata.xml.in` template), so
`make -C data xnote.desktop tech.norvi.xnote.metainfo.xml` is enough to
validate them.

`./src/xnote` runs from the build tree. The Help window and the tray icon
look up installed files (the help text under `$(datadir)`, the icon by
name), so those two need `make install`.

## Tests

Four entry points; CI runs all four on every pull request and release tag.

**`make check`** builds and runs the five GLib tests in `tests/`:

| Binary | What it proves |
|---|---|
| `test-escape` | The U+E000/U+E001 escape scheme in `xpad-text-buffer.c` (GLib only) |
| `test-fio` | The `key value` file format round-trips |
| `test-settings` | The settings registry round-trips every bool/uint option and pins the exact set of on-disk keys |
| `test-autostart` | Enabling autostart creates a symlink, and a `mkdir` failure reports an error instead of crashing |
| `test-tray-lifecycle` | On a private `GTestDBus` bus with a pad-group double: rapid tray toggles and the no-tray reachability guard |

Failures are in `tests/test-suite.log`.

**`make check-go`** (or `cd cloud-helper && go test -mod=vendor ./...`)
runs the helper's tests. It is a separate target, not part of `make check`,
and exists only when `configure` found Go. The remote tests use `rsync`.

**`tests/wayland-smoke.sh /path/to/xnote`** is the end-to-end test: a
headless weston (`--backend=headless`, 1280 × 1024) under fresh `XDG_*`
directories with `DISPLAY` unset, run inside `dbus-run-session`. It starts
`xnote --new`, fails on any GTK `CRITICAL` or `WARNING`, drives the
instance only through its own command line (`--new-from-file`, `--show`,
`--quit`), polls for the `content-*` file the debounced save produces, then
restarts with `--hide` and proves that exits cleanly because there is no
tray. No synthetic input, by design: Wayland has no XTEST, and `uinput`
injection is seat-global. Needs `weston` and `dbus`; CI installs the build
first so the on-disk layout is the shipping one.

**`python3 tests/public-check.py`** is the release-surface gate: the
version in `configure.ac` and the AppStream releases, the public issue URL,
the AppStream id and licence, SVG-only icons, the public Go module path and
`go 1.26.8`, the NOTICE baseline, the licence texts and their locations, no
compiled helper in the tree, the offline build flags in `Makefile.am`, the
vendored `LICENSE`/`PATENTS` files, and that the README points at GitHub.
It is dependency-free and runs from the tree it is in.

CI also lints, and any finding fails the build: `shellcheck` on the smoke
script, `scripts/build-deb.sh` and `scripts/check-deb.sh`, `cppcheck` over
`src/` (warning, performance and portability checks, honouring
`cppcheck-suppress` comments), `desktop-file-validate` on the generated
desktop file, `appstreamcli validate --no-net` on the generated AppStream
file, and `go vet` on the helper.

## The `xpad_` prefix rule

The product, binary, package, gettext domain and config directory are
`xnote`. The source files (`src/xpad-*.c`), the C symbols (`Xpad*`,
`xpad_*`, `XPAD_TYPE_*`), the CSS class names (`.XpadToolbar` and friends)
and the environment variables (`XPAD_*`) keep the `xpad` prefix from the
upstream project. The rename in 2.0.0 stopped deliberately at the boundaries
users see, so that nothing coupled to those names at runtime broke. Keep it
that way: new code in `src/` uses the `xpad_` prefix, and pull requests that
rename existing symbols will be declined.

## The no-X11 rule

XNote is native Wayland since 3.0.0. There is no X11 code path, no
`GDK_BACKEND` pin, and nothing that talks to a window manager: pad
placement, "on all workspaces" and taskbar hints are the compositor's
business. CI enforces the linkage half of this mechanically: after the
build it runs

```sh
readelf -d src/xnote | grep NEEDED | grep -qE 'libX11|libXi|libSM|libICE'
```

and fails if anything matches. `readelf -d` rather than `ldd`, because
`ldd` prints the transitive closure and `libgtk-4` legitimately pulls in
both backends; only XNote's own `DT_NEEDED` entries are policed. An
innocent-looking `#include` that drags an X library back in will not pass.

## Packaging

`debian/` is the Debian packaging: `control` (source and binary `xnote`,
`Architecture: any`, `Conflicts`/`Replaces`/`Provides: xpad`, Recommends
`gnome-shell-extension-appindicator`, `libsecret-tools`, `rclone`, `rsync`),
`rules` (dh with `hardening=+all`, the offline Go flags exported,
`dh_installsystemduser --no-enable`), `install`, `copyright`, the
maintainer scripts (`xnote.postinst` is only `#DEBHELPER#`; `xnote.prerm`
disables the old `xpad-cloud-backup.timer` on upgrade), and the two systemd
user units `xnote-cloud-backup.service` / `.timer`. Source format is
`3.0 (native)`.

`scripts/build-deb.sh [outdir]` builds `xnote_<version>_<arch>.deb` and the
source tarball `xnote_<version>.tar.gz`. It reads the version from
`configure.ac`'s `AC_INIT`, generates `debian/changelog` for the build (and
puts a tracked one back afterwards), tars the tree reproducibly under
`SOURCE_DATE_EPOCH` (the tree as found, so start from a clean checkout),
runs `dpkg-buildpackage -us -uc -b`, and then runs `scripts/check-deb.sh` on
the package. It uses whatever `go` is first on `PATH` (put
`/opt/xnote-go-1.26.8/bin` there after running `ci/install-go-toolchain.sh`)
and needs the build dependencies from `debian/control` and
`dpkg-buildpackage`. Linux only.

`scripts/check-deb.sh <deb>` checks what lintian cannot know about: the
`postinst` must carry the guard that `dh_installsystemduser --no-enable`
writes and must not call `deb-systemd-invoke … start`, so installing the
package never enables or starts the opt-in backup timer, and the packaged
helper must have been built by a Go at least as new as `go.mod` requires.

CI does not use `build-deb.sh`. It runs `dpkg-buildpackage -us -uc -b -d`
itself in the Ubuntu 26.04 container (`-d` skips the build-dependency check,
which asks for the distribution's older `golang-go`; CI builds with the
checksummed 1.26.8 instead), fails on any lintian error or warning
(`lintian --fail-on error,warning`; `debian/xnote.lintian-overrides` holds
the accepted ones), runs `scripts/check-deb.sh`, then installs the `.deb`
and runs the smoke test against the installed binary. A release tag
publishes that `.deb` on the GitHub Release. The Norvi APT archive at
<https://apt.norvitech.com> (suite `resolute`, `amd64`) publishes on
its own schedule; its front page documents how to add it. The package is
built on 26.04 and links against 26.04's libraries, so it is filed under
that release's suite only.

## The release model

**GitHub is the development home.** Branch from `main` and open a pull
request into it; it merges here once CI and the privacy check pass and the
code owner approves it. The history is the upstream Xpad history up to the
baseline commit named in `NOTICE`, then XNote's own.

**The privacy check** (`.github/workflows/privacy.yml`) scans the whole
history for secrets with gitleaks and runs `scripts/check_public_content.py`,
which fails a change that adds a personal path, a private URL or network
address, or a private file such as a key or a database;
`scripts/test_public_content.py` tests the checker.

**A release is a tag.** Pushing `vX.Y.Z` runs
`.github/workflows/release.yml`: CI and the privacy check again, a proof
that the tag is on `main` and matches `AC_INIT`, then a GitHub Release with
the `.deb`, the source archive and `SHA256SUMS.txt`. The checklist is in
[RELEASING.md](RELEASING.md).

**Where the version lives.** `configure.ac`'s `AC_INIT` is the version's
home. Two other places must agree, and `tests/public-check.py` fails when
they do not: `VERSION` in `tests/public-check.py` itself and a
`<release version="…">` entry in `data/xnote.appdata.xml.in`; by convention
the newest version heading of `CHANGELOG.md` matches too. `debian/changelog`
is not tracked: CI and `scripts/build-deb.sh` generate it from `AC_INIT` at
build time.

**`CHANGELOG.md`** records the release line, newest first, one heading per
version with the date. A pull request adds its entry under `## Unreleased`
at the top, and the release moves those entries under its version heading.
The AppStream `<releases>` list carries a one-paragraph summary of each
release for software centres.

## For coding agents

If you are an AI agent asked to work on XNote, orient in this order:

1. Read this file, then the [layout table](#src--the-application). It tells
   you which file owns which behaviour; there is no other map.
2. Build and test exactly as [Building from source](#building-from-source)
   and [Tests](#tests) say: `NOCONFIGURE=1 ./autogen.sh && ./configure
   --enable-debug=most && make -j"$(nproc)" && make check`, plus
   `make check-go` if you touched `cloud-helper/`, plus the smoke test if
   you touched startup, saving, the tray or the command line. CI runs all
   of these with `-Werror`; a warning is a failure.
3. Where things live:
   - **UI** (windows, menus, toolbar, preferences, tray): `src/xpad-pad.c`,
     `src/xpad-toolbar.c`, `src/xpad-preferences.c`, `src/xpad-tray.c`,
     `src/xpad-search-bar.c`, `src/xpad-pad-properties.c`.
   - **Storage** (what is written to `~/.config/xnote` and when):
     `src/fio.c` (the writes), `src/xpad-periodic.c` (the 4-second tick),
     `src/xpad-settings.c` (`default-style`), `src/xpad-pad.c` (`content-*`
     and `info-*`), `src/xpad-text-buffer.c` (the text serialisation).
   - **Backup** (everything encrypted or off-box): `cloud-helper/`, with
     `src/xpad-backup.c` as the only bridge from the application.
   - **Startup, single instance, migration, command line**: `src/xpad-app.c`.
4. Respect the two hard rules: `xpad_` names stay, and nothing may link
   X11. Do not change the crypto parameters in `cloud-helper/crypto.go`
   or the on-disk key names in `src/xpad-settings.c`'s registry; existing
   users' files depend on both.
5. User-visible behaviour is documented in three places that must stay in
   step: [docs/user-guide.md](user-guide.md), `doc/xnote.1` /
   `doc/xnote-cloud-backup.1`, and `doc/xnote-user-help.txt` (the in-app
   help). If you change behaviour, change the documentation in the same
   pull request.
6. Pull requests merge here once CI and the privacy check pass and the code
   owner approves (see [The release model](#the-release-model)). Keep one
   concern per pull request, add its entry under `## Unreleased` in
   `CHANGELOG.md`, and fill in the template.
