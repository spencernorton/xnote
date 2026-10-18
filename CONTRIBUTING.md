# Contributing to XNote

Thanks for your interest. XNote is a small project with one maintainer, so
the process is deliberately light — but a few things are fixed.

## How changes land

GitHub is the development home. Branch from `main` and open a pull request
into `main`. Build, test and privacy checks must pass before merge. Changes
ship in tagged releases; see [the release guide](docs/RELEASING.md).

Use a GitHub noreply address for commit authorship if you prefer to keep
your personal address private. Review your diff and commit messages before
pushing: public history, logs and uploaded screenshots are public data.

## Before you start

- **Bugs** — open a [bug report](https://github.com/spencernorton/xnote/issues/new/choose).
  A report with reproduction steps, versions and the terminal output is
  usually fixed faster than a pull request that arrives without one.
- **Features** — open a feature request first. XNote has firm opinions about
  what a note is (an independent window, saved as you type, never sent
  anywhere by the application itself); an idea that cuts across them needs a
  conversation before code.
- **Security** — never in a public issue. Use
  [private vulnerability reporting](https://github.com/spencernorton/xnote/security/advisories/new);
  see [SECURITY.md](SECURITY.md).

## Working on the code

XNote is C on GTK 4, libadwaita and GtkSourceView 5, built with autotools.
The optional backup helper, `xnote-cloud-backup`, is Go. On Ubuntu:

```bash
sudo apt install build-essential autoconf automake libtool intltool gettext \
  autopoint pkg-config autoconf-archive libgtk-4-dev libadwaita-1-dev \
  libgtksourceview-5-dev libglib2.0-dev libpango1.0-dev libgdk-pixbuf-2.0-dev

NOCONFIGURE=1 ./autogen.sh
./configure --enable-debug=most   # -Wall -Werror; this is what CI builds with
make -j"$(nproc)"
./src/xnote                       # runs from the build tree; the tray icon
                                  # and F1 help need `make install`
make check                        # the GLib test suite under tests/
```

The Go helper is built only when `configure` finds a `go` executable.
`go.mod` requires Go 1.26.8, and the build fails closed with anything older
rather than downloading a toolchain; the C build never depends on it. Its
tests are a separate target, not part of `make check`:

```bash
make check-go
# equivalently:
cd cloud-helper && GOTOOLCHAIN=local GOPROXY=off go test -mod=vendor ./...
```

The end-to-end smoke test runs the binary on a real, headless Wayland
compositor and drives it through its own command line. It needs `weston` and
`dbus`, and takes the path of the `xnote` to test:

```bash
sudo apt install weston dbus
dbus-run-session -- tests/wayland-smoke.sh /usr/bin/xnote
```

CI also runs `python3 tests/public-check.py`, which checks the public
surface (version, licences, vendored modules); it needs only Python 3, so
run it yourself after touching any of those.

### Conventions

- **`xpad_` stays.** The product, binary, package and config directory are
  `xnote`; the source files (`src/xpad-*.c`), C symbols (`Xpad*`, `xpad_*`,
  `XPAD_TYPE_*`) and CSS class names (`.XpadToolbar`) keep the `xpad` prefix
  from the upstream project. This is deliberate — the rename stopped at the
  boundaries users see — so please do not propose renaming them.
- **Never link X11 directly.** XNote is native Wayland. CI runs
  `readelf -d src/xnote` and fails the build if `libX11`, `libXi`, `libSM` or
  `libICE` appears in `NEEDED`; an include that pulls one in will not pass.
- **GTK warnings are failures.** The smoke test treats any GTK `CRITICAL` or
  `WARNING` at startup as a failed run, so check the terminal output of your
  build before opening a pull request.
- **Do not reach for the network from the application.** Anything that
  leaves the machine belongs in the backup helper, and only as an encrypted
  kit; see [SECURITY.md](SECURITY.md).
- Keep a change to one concern. A pull request that fixes a bug and
  reformats a file is two pull requests.
- Commits carry a `Signed-off-by:` line (`git commit -s`, the Developer
  Certificate of Origin). There is no CLA.
- No secrets, hostnames, personal data or screenshots of a real desktop in
  the diff — the Privacy check rejects the ones it recognises, and review
  sends back the rest.
- Tests: a bug fix carries a regression test; a feature carries the smallest
  test that fails without it. C tests live in `tests/` (one `test-*.c` per
  area, registered in `tests/Makefile.am`); Go tests sit next to the code in
  `cloud-helper/`.
- Translations live in `po/`. Strings shown to users go through `_()` so they
  reach the catalogue.

### Packaging

`debian/` holds the Debian packaging and `scripts/build-deb.sh` builds the
same `.deb` the APT repository publishes, from a clean tree, with whatever
`go` is first on `PATH` — put a Go at least as new as `go.mod` asks for
there first (`. ci/install-go-toolchain.sh` gives you the checksummed
1.26.8 the release builds use). A change
that touches installed paths, the systemd user units or the helper's command
line should be checked against that build as well as `make install`.

## Out of scope

So nobody wastes an evening on it, XNote will not accept:

- telemetry, analytics, or any network use by the application itself — only
  the opt-in backup helper ever talks to a remote, and only with ciphertext
- a sync service, accounts, or sharing notes between people
- placing windows itself — Wayland forbids it, and that is the
  [XNote Placement](https://github.com/spencernorton/xnote-placement) extension's job
- renaming the `xpad_` files and symbols

## Pull request checklist

The template asks for what changed, why, and how it was tested, plus a
confirmation that the diff carries no secrets, machine names or personal
paths. Fill it in — it is what the reviewer reads first.

## Licence

By contributing you agree that your contribution is licensed under the
[GNU General Public License, version 3 or later](COPYING) that covers the
project. The one exception is the BinReloc code in `src/prefix.c` and
`src/prefix.h`, which is third-party and stays under the
[GNU Lesser General Public License, version 3 or later](src/COPYING.LESSER);
see [NOTICE](NOTICE).
