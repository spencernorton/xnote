# Changelog

## 3.3.3 — 2026-10-29

- Fix the tray menu fading in row by row after a note is edited: dbusmenu
  row IDs are now positional and stable across rebuilds, so the menu host
  keeps its actors and updates changed labels in place instead of
  destroying every row and re-adding them one idle at a time.

## 3.3.2 — 2026-10-28

- Keep unchanged tray menu rows cached so the menu opens completely instead of growing row by row.
- Preserve PQ secrets when repairing missing public keys and reject mismatched pairs.
- Add validated escrow-key import and an export requirement before hybrid backups.
- Keep backups locked across interrupted import/rotation and failed keyring updates; verify and sync recovery state before unlocking.
- Give rotated snapshots fresh cloud-kit identities and bind cloud receipts to ciphertext and destinations. Retry incomplete mirror delivery.
- Use the actual Preferences parent window for key downloads, and create a fresh export folder for each download.

## 3.3.1 — 2026-10-28

- Fix Google Drive/Dropbox sign-in validation when the rclone configuration
  file is encrypted: the helper now asks `rclone config show` (which honours
  `RCLONE_PASSWORD_COMMAND`) instead of parsing the encrypted file as text,
  which always reported a successful sign-in as missing.

## 3.3.0 — 2026-10-28

- Add post-quantum hybrid backups: Preferences gains an **Encryption
  algorithm** picker (Classic Argon2id + XChaCha20-Poly1305, or Hybrid with
  an ML-KEM-1024 envelope so new snapshots need both the master key and the
  PQ secret). Classic snapshots stay readable in either mode. New helper
  verbs: `set-crypto`, plus `pq-secret`/`pq-public` key files with a SHA-256
  fingerprint in `status`.
- Back up to two providers at once: `CLOUD_MIRROR` (Preferences: **Mirror
  remote**) receives the same recovery kit as the primary target, and the
  backup fails unless both pushes succeed.
- Prove backups open: **Test Recovery** in Preferences (helper:
  `test-recovery [--remote]`) decrypts and verifies the newest snapshot
  without touching live notes, records the drill result, and a new monthly
  `xnote-cloud-backup-test-recovery.timer` repeats it automatically.
- Download and rotate keys from Preferences: **Download Keys…** writes an
  escrow export (`export-keys`: keyfile + PQ keys + checksums, never a
  plaintext master key); **Rotate Keys…** performs true master-key rotation
  (`rotate-key`: every snapshot re-encrypted and verified, new recovery
  code issued) instead of only rewrapping the passphrase.

## 3.2.6 — 2026-10-27

- Fix: at login, restored notes across workspaces no longer make the dock icon
  bounce or jump across workspaces when clicked. `xpad_app_load_pads` now maps
  saved notes with `gtk_widget_set_visible` instead of `gtk_window_present`,
  `xpad_pad_show` no longer fires a redundant `gtk_window_present` (`xdg_activation_v1`)
  inside the `"map"` signal handler, and `xnote.desktop` sets
  `StartupNotify=false`. Interactive note creation and tray/menu selection
  still present and focus the target note.

## 3.2.5 — 2026-10-27

- Add a **Backup** page to XNote Preferences (`src/xpad-preferences.c`) for
  selecting the backup destination (**Local Only**, **Google Drive**,
  **Dropbox**, or **Custom Folder or Remote**), authorizing Google Drive or
  Dropbox in the browser via `rclone`, setting up or unlocking end-to-end
  encryption (Argon2id + XChaCha20-Poly1305) over stdin pipes with a copyable
  one-time recovery code dialog, running **Back Up Now**, and toggling the
  hourly `xnote-cloud-backup.timer` background unit.
- Extend `xnote-cloud-backup` (`cloud-helper/`) with `status --json` /
  `status --porcelain`, `set-target <local|gdrive|dropbox|custom>`,
  `connect <gdrive|dropbox>`, and `disconnect <gdrive|dropbox>` commands plus
  atomic `SaveConfig` persistence for `~/.config/xnote/cloud-backup.conf`.

## 3.2.4 — 2026-10-26

- `xnote-cloud-backup` enforces a strict four-file encrypted remote kit gate
  (`snapshot.xnote-enc`, `manifest.json`, `README-RESTORE.txt`, `RESTORE.sh`)
  before any upload, verifies the `XNOTE\x00\x01\x01` header and wrapped DEK,
  and automatically falls back from `rclone check --one-way --checksum` to
  `rclone cryptcheck --one-way` (or `rclone check --one-way --download`) when
  backing up to Dropbox, Google Drive or `crypt`-wrapped rclone remotes that
  lack a common hash.
- `xnote-cloud-backup sync` compares the deterministic note archive against the
  validated local head snapshot and skips minting duplicate snapshot
  generations when notes are unchanged, while still pushing any unsynced head
  snapshot to the configured remote. Orphaned `.2026*` directories in
  `remote-staging/` are swept under `backup.lock`.
- `xpad_backup_run` activates `systemctl --user start --no-block
  xnote-cloud-backup.service` when initialized on a systemd desktop so user
  unit drop-ins apply and session logout does not interrupt an in-flight sync.
- Fix the `pad-removed` class signal offset in `XpadPadGroup`, format `va_list`
  arguments in `xpad_periodic_error_exit`, fix `GValue` getters and finalize
  leaks in `XpadSettings`, `XpadPreferences`, `XpadPadProperties` and
  `XpadPad`, replace accumulated per-widget `GtkCssProvider` instances in
  `XpadTextView`, and fix the `@theme_fg_color` CSS fallback name.
- Explain the backup helper’s keyring prerequisite before its initialization commands.

- `tests/test-settings` keeps its settings file in each test's own temporary
  directory. It used the shared `/tmp/default-style`, so two `make check`
  runs at once on one machine could read each other's settings and fail, a
  `/tmp/default-style` left by another user failed the test, and the file
  stayed behind.
- CI runs the linters the development guide lists: `shellcheck` on the smoke
  test and `scripts/build-deb.sh`, `cppcheck` over `src/`,
  `desktop-file-validate` and `appstreamcli validate` on the generated
  metadata, and `go vet` on the backup helper.
- The development guide, the contributing guide and the pull-request
  template describe how a change lands now: the pull request merges here and
  a pushed `vX.Y.Z` tag publishes the release. They still called this
  repository a release mirror.
- `tests/wayland-smoke.sh` keeps the services its session bus starts (the
  portals, gvfsd, the accessibility bus) in the test's own directories and
  compositor. Run from a desktop, they used the live session's runtime
  directory: the document portal took over the running one's mount, and the
  accessibility bus deleted the live socket when the test ended. The script
  now also refuses a session bus that anything else is on.
- CI runs `tests/public-check.py`, the release-surface check, on every pull
  request and release tag. It used to be run by hand, and 3.2.1 and 3.2.2
  shipped with it failing and without their AppStream release entries.
- CI lints the Debian package it builds (`lintian --fail-on error,warning`)
  and runs the new `scripts/check-deb.sh` on it, which fails if installing
  the package would enable or start the opt-in backup timer, or if the
  backup helper was built by a Go older than `go.mod` requires.
  `scripts/build-deb.sh` runs the same checks.
- `src/xpad-backup.c` and `src/xpad-backup.h` name Spencer Norton as their
  copyright holder, as `debian/copyright` and the README do, and five
  comments in the backup helper, the preferences window and the settings
  table read as plain comments without a leftover tag prefix.

## 3.2.3 — 2026-09-29

- Notes open in the order of their `info-` file names (byte order), not in
  whatever order the filesystem lists them. XNote Placement hands out the
  saved places of notes that share a first line, several empty notes
  typically, in that same order, so with XNote Placement 1.1.5 or later each
  such note keeps its own place rather than possibly another's.

## 3.2.2 — 2026-09-27

- Build Debian packages on Ubuntu 26.04 and verify the installed package
  with a real headless Wayland compositor on the target distribution.


This file records the public XNote release line. The complete upstream Xpad
history remains available from the canonical Launchpad repository.

## 3.2.1 — 2026-09-27

- Establish GitHub pull requests as the development workflow, with privacy checks.
- Add deployment, configuration, security, upgrade and recovery documentation.

## 3.2.0 — 2026-09-21

- The package recommends `gnome-shell-extension-xnote-placement`, so
  `apt install xnote` brings the placement extension along; the README says
  how to enable it and what it does.
- README rewritten to the NorviTech Suite house style: suite, release and
  install badges, an encrypted-backup callout (Dropbox or any rclone remote,
  the recovery code, the hourly timer), the placement callout, a data-location
  table with what leaves the machine, and the suite footer. Community files
  aligned with the shared templates (DCO sign-off, an out-of-scope list, a
  security scope section).
- The GitHub CI workflow can be run by hand (`workflow_dispatch`).

## 3.1.0 — 2026-09-20

- Debian package built from the audited public tree (`scripts/build-deb.sh`,
  `debian/` in the release) and published to the APT archive on every
  release tag; `sudo apt install xnote` on Ubuntu 26.04.
- Documentation: `docs/user-guide.md` (every setting, shortcut, command and
  the backup helper end to end), `docs/development.md` (source layout, tests,
  packaging, the release model, a section for coding agents), a rewritten
  in-app help (`F1`), and the `xnote-cloud-backup(1)` page now lists `sync`
  and `show-recovery`.
- Community files: contributing guide, code of conduct, support and security
  policies, issue forms, pull-request template, a CI workflow on GitHub and
  the Sponsor button.
- Licence text for `src/prefix.c` moved to `src/COPYING.LESSER` so the
  repository is labelled GPL-3.0 rather than LGPL-3.0; `COPYING` unchanged.
- `xnote-cloud-backup restore` no longer refuses after a normal quit: only a
  live XNote answering on its socket counts as running, and XNote removes
  the socket when it quits.
- "Open a new empty pad" applies at startup only; `xnote --show`, `--hide`
  and `--toggle` no longer open an extra note when it is enabled.
- The application icon is the orange note with the folded corner again, as a
  vector (`images/hicolor/scalable/apps/xnote.svg`), so it is crisp at every
  size and the same on the GitHub page and the desktop.
- The AppStream metadata installs as `tech.norvi.xnote.metainfo.xml`, names
  the screenshots and the help URL, and validates cleanly; libadwaita 1.5 is
  the declared minimum (it always was the real one).

## 3.0.6 — 2026-09-20

- README: leads with the name and what a note is, shows a recording of a
  session (typing, `Ctrl+N`, bold, resize) as the hero, a desktop and a
  hovered-toolbar capture, and one paragraph per feature; the palette and
  Preferences captures stay. Captures from an isolated profile with invented
  notes. The two GIFs and the bare 300 px note are gone.

## 3.0.5 — 2026-09-19

- Rename the public README to `README.md` so GitHub renders its screenshots
  and animations on the repository overview page.

## 3.0.4 — 2026-09-19

- Added privacy-reviewed screenshots and short animations covering notes,
  preferences, color selection, and note resizing.

## 3.0.3 — 2026-09-19

- Prepared the first audited public source release.
- Added public provenance, privacy, security, and contribution documentation.
- Hardened backup configuration and restore validation.
- Require Go 1.26.8 or newer for the backup helper so restore is never built
  with the vulnerable `archive/tar` implementation described by GO-2026-4869.
- Kept the encrypted-backup timer disabled after package installation until
  the user explicitly initializes and enables it.
- Made generated source archives complete and reproducible, including the
  installed user-help document, committed translation template, and explicit
  Go source manifest; retained the checksummed vendored Go modules for offline
  source and Debian builds; and support read-only, out-of-tree distribution
  builds.
- Replaced the undocumented raster icon with a GPL-covered derivative of the
  upstream Xpad vector artwork.

## 3.0.2 — 2026-08-03

- Made encrypted-backup initialization safe to drive through a pipe.

## 3.0.0 — 2026-07-29

- Completed the native Wayland port and removed X11-specific application code.

## 2.0.0 — 2026-07-14

- Renamed the application and package from Xpad to XNote.
- Added automatic migration from the legacy Xpad configuration directory.
