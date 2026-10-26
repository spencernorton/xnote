# XNote user guide

Everything the README's pictures do not say. XNote keeps each note as its
own small window: sized and coloured on its own, saved as you type, stored
as a plain file under `~/.config/xnote`. Nothing leaves the machine unless
you set up the optional backup helper. The two manual pages,
[`doc/xnote.1`](../doc/xnote.1) and
[`doc/xnote-cloud-backup.1`](../doc/xnote-cloud-backup.1), are the terse
form of the command-line and backup sections below.

## First run

**What appears.** The first time XNote starts with no configuration
directory it does three things: it turns on *Start XNote automatically after
login*, it opens the Help window, and it creates one empty note. Whether
that note is visible straight away depends on the tray: with the tray enabled
(the default) and *Display pads* left at *Restore to previous state*, the new
note is created hidden and you open it from the tray icon's menu; with the
tray disabled it is shown at once. `xnote --new` always opens a visible note.

**Where notes live.** Everything is in `~/.config/xnote` (or
`$XDG_CONFIG_HOME/xnote`): one `default-style` file for the global settings,
and for each note a `content-XXXXXX` file with the text and an
`info-XXXXXX` file with its size, colour, font and hidden state. The
directory is created with mode 0700. There is also a `server` socket that a
second `xnote` command uses to talk to the running one.

**Migration from Xpad.** If `~/.config/xnote` does not exist but
`~/.config/xpad` does, XNote renames the old directory to the new name on
first start (or copies it across and removes the old one when the two are on
different filesystems). Every note, colour and setting carries over. XNote
refuses to start if an Xpad instance is still running, with the message
"An existing xpad instance is running. Please close xpad before starting
XNote so your notes can be migrated safely." Close Xpad and start XNote
again. A legacy `~/.xpad` directory is renamed too (a plain rename, no
cross-filesystem copy). A stale
`~/.config/autostart/xpad.desktop` link is removed and replaced by
`xnote.desktop` so autostart keeps working. Nothing is migrated if
`~/.config/xnote` already exists, even when it is empty; see
Troubleshooting.

## Notes

**Creating.** `Ctrl+N`, the *New* button on the toolbar, *New* in the
right-click menu, or run `xnote` again from a terminal or launcher while it
is already running. A new note takes a random colour from the sticky-note
palette (Preferences → Layout → *Give new pads a random sticky-note color*,
on by default) and the default size from Preferences → Layout → *New pad
size* (300 × 300 pixels until you change it). The first line of the text is
the note's title, shown in the window title and in the *Notes* menus.

**Closing and hiding.** *Close* (`Ctrl+W`, the menu, or the *Close* toolbar
button) hides the note and remembers that it is hidden; it comes back with
*Show All* from any note's menu or the tray menu, from the numbered list in
the same menus, or with `xnote --show`. *Close All* hides every note at once
when a tray icon is present. Without a tray icon, closing the last visible
note quits XNote instead (see Tray); the note is not recorded as hidden, so
it is back next time you start.

**Deleting.** *Delete* (`Shift+Delete`, the menu, or the *Delete* toolbar
button) removes the note and its two files for good. XNote asks "Delete
this pad?" first, with *Cancel* and *Delete*, unless the note is empty or
you have turned off Preferences → Other → *Confirm pad deletion*.

**Reload.** `F5` or *Reload* re-reads the note from disk. If a note's file
could not be read when XNote started, the note is left unloaded and will not
be saved over until a reload succeeds, so a transient read error never
erases what is on disk.

### The toolbar

Hover a note and a rounded toolbar fades in at the bottom centre; move the
pointer away and it fades out about a second later (Preferences → View →
*Autohide toolbar*). It stays while one of its menus is open. Turn *Show
toolbar* off to remove it entirely.

Out of the box the toolbar holds two items: *New* (the plus) and *Color*
(the dot). Right-click the toolbar to add or remove buttons: the popover
lists one *Add … button* row for every button not yet present, plus *Remove
All Buttons* and *Remove Last Button*. The full set:

| Button | Tooltip |
|---|---|
| Clear | Clear Pad Contents |
| Close | Close and Save Pad |
| Color | Change Sticky-Note Color |
| Copy | Copy to Clipboard |
| Cut | Cut to Clipboard |
| Delete | Delete Pad |
| Find | Find text |
| New | Open New Pad |
| Paste | Paste from Clipboard |
| Preferences | Edit Preferences |
| Properties | Edit Pad Properties |
| Redo | Redo |
| Quit (Close All) | Close All Pads |
| Undo | Undo |
| Separator | — |

The layout is saved as the `buttons` line in `default-style` and applies to
every note.

### Colour

There are two ways to colour a note, and they do different things.

**The colour dot.** The *Color* toolbar item is a small swatch in the note's
current background colour. Click it and a popover shows the six palette
colours: Canary Yellow, Pink, Mint Green, Sky Blue, Peach and Lilac. Picking
one sets **that note's** background, gives it the fixed dark sticky-note ink
colour, and marks the note as no longer following the global colours. The
same palette is what *random colour* draws from.

**Global colours.** Preferences → Layout → *Colors* sets the colours for
every note that still follows the global style: *Use colors from theme*
(the GTK theme decides) or *Use these colors* with a *Text* and a
*Background* colour button. A note that has been given its own colour keeps
it; to make it follow the global colours again, open its properties dialog
(right-click → *Edit* → *Layout*, or the *Properties* toolbar button) and
choose *Use colors from XNote preferences*. The same dialog has *Use font
from XNote preferences* / *Use this font:* for a per-note font.

### Moving and resizing

Hold `Ctrl` and drag with the left mouse button anywhere on the note to move
it; hold `Ctrl` and drag with the right button to resize it. The small grip
in the bottom-right corner (bottom-left in right-to-left locales) fades in
with the toolbar and resizes with a plain left-button drag. The size is
remembered per note; the position is not (see Placement on Wayland).

### Formatting

`Ctrl+B` bold, `Ctrl+I` italic, `Ctrl+U` underline. Strikethrough is in the
right-click menu when text is selected (it has no shortcut). Formatting is
stored inside the note's `content-` file, so it survives restarts and
backups. `Ctrl+Z` undoes, `Ctrl+Y` redoes.

### Search

`Ctrl+F` (or *Edit* → *Find*, or the *Find* toolbar button) opens a search
bar at the top of the note. Matches are found as you type; `Enter` or the
down-arrow button jumps to the next match, the up-arrow button to the
previous one, and the search wraps around at the end of the note. `Escape` or the close button
hides the bar.

### Read-only notes

Preferences → Other → *Make pads read-only (CTRL-J)*, or `Ctrl+J` on any
note, locks every note against accidental edits. While locked, a single
left-click on the text drags the note, and a double-click unlocks that note
for editing until the keyboard focus leaves it.

### The right-click menu

Right-click anywhere on a note (or press `Menu` or `Shift+F10`). With no
text selected the menu is:

- *New*, *Delete*, *Reload*, *Close*
- *Edit* → *Undo*, *Redo*, *Paste*, *Find*, *Layout* (the per-note font and
  colour dialog)
- *Notes* → *Show All*, *Close All*, then a live numbered list of every
  note by title (`1.` to `9.` also work as mnemonics; long titles are cut
  at 20 characters)
- *Help* → *Help*, *About*
- *Preferences*

With text selected the menu is *Cut*, *Copy*, *Paste*; *Bold*, *Italic*,
*Underline*, *Strikethrough*; *Close*.

## Preferences

Open Preferences from any note's right-click menu, from the tray menu, or
with the *Preferences* toolbar button. The dialog has five pages. Every
change applies immediately and is written to `default-style`.

**View.**
*Toolbar*: *Show toolbar* (on) and *Autohide toolbar* (on; greyed out while
the toolbar is off).
*Notes*: *Show scrollbar* (on) and *Show window decorations* (off; on gives
every note a normal title bar).

**Layout.**
*Font*: *Use font from theme* or *Use this font* with a font button (the
compiled-in default is Sans 9).
*Colors*: *Use colors from theme* or *Use these colors* with *Text* and
*Background* buttons, then *Give new pads a random sticky-note color* (on).
*New pad size*: *Height* and *Width* in pixels, 10 to 99999 in steps of 10.

**Startup.**
*Start XNote automatically after login*: creates or removes a link
`~/.config/autostart/xnote.desktop` pointing at the installed desktop file;
the switch reads its state back from that link.
*Open a new empty pad*: open a fresh note on every start (off).
*Delay in seconds*: 0 to 14; XNote sleeps that long before doing anything
else, which gives a slow tray host time to appear.
*Display pads*: *Open all pads*, *Hide all pads*, or *Restore to previous
state* (the default). This row is greyed out while the tray is disabled,
and in that case XNote behaves as *Open all pads* whatever is stored, so
that it can never start with nothing to click on.

**Tray.**
*Enable tray icon* (on).
*Tray left mouse click behavior*: *Do Nothing*, *Toggle Show All* (the
default), *List of Pads*, or *New Pad*. Greyed out while the tray is
disabled. *List of Pads* takes effect the next time XNote starts, because
it is announced to the tray host when the icon registers.

**Other.**
*Make pads read-only (CTRL-J)* (off), *Confirm pad deletion* (on), and
*Enable line numbering* (off; shows line numbers down the left edge of every
note).

## Tray

XNote's tray icon is a StatusNotifierItem: it registers over D-Bus as
`org.kde.StatusNotifierItem-<pid>-1` with the session's
StatusNotifierWatcher, offers a `com.canonical.dbusmenu` menu, and uses the
`xnote-symbolic` icon so it follows the panel's colours. If the watcher
disappears and comes back (a panel restart, for example), XNote registers
again on its own. No AppIndicator library is involved.

**GNOME needs an extension.** GNOME Shell has no StatusNotifierWatcher of
its own. Install the AppIndicator and KStatusNotifierItem support extension
(Ubuntu packages it as `gnome-shell-extension-appindicator`, which the
XNote package recommends). Without it there is simply no tray icon, and the
two rules below apply.

**The menu.** *New*; *Show All* and *Close All* (enabled only when notes
exist); the numbered list of notes; *Preferences*, *Help*, *Quit*.

**Clicks.** A left click does what Preferences → Tray says (toggle
show/hide all by default; toggling shows every note if any is hidden and
hides them all otherwise). A middle click always creates a new note.

**Two rules when there is no tray icon.** XNote treats "no tray icon" as
"the notes are the only way to reach me", so:

1. Closing the last visible note quits XNote rather than hiding the note.
   The note is not recorded as hidden and is back on the next start.
2. At start, once the tray question has settled (XNote polls for up to
   three seconds), if there is no tray icon and no visible note: with saved
   notes that are all hidden, it shows them all; with no saved notes at
   all, it quits.

Rule 2 is why `xnote --hide` exits immediately on a desktop without a tray
host: `--hide` closes every note, a `--new` note included, and closing the
last one with nothing in the tray quits. It also means
that turning the tray off in Preferences while every note is hidden first
reveals the notes (or creates one if there are none) so that nothing is left
unreachable.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| `Ctrl+N` | New note |
| `Ctrl+W` | Close (hide) this note |
| `Shift+Delete` | Delete this note |
| `F5` | Reload this note from disk |
| `Ctrl+Q` | Quit XNote |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+B` / `Ctrl+I` / `Ctrl+U` | Bold / italic / underline |
| `Ctrl+F` | Find in this note |
| `Enter` (in the search bar) | Next match |
| `Escape` (in the search bar) | Close the search bar |
| `Ctrl+J` | Toggle read-only for all notes |
| `Menu` or `Shift+F10` | Open the note's right-click menu |
| `F1` | Help |
| `Ctrl` + left drag | Move the note |
| `Ctrl` + right drag | Resize the note |

Cut, copy, paste and select-all are the ordinary GTK text keys.

## Command line

`xnote [OPTIONS]`. Two options act on the command itself:

| Option | Effect |
|---|---|
| `-v`, `--version` | Show the version and quit |
| `-?`, `--help` | Show usage and quit (`-h` is taken by `--hide`) |
| `-N`, `--no-new` | Do not create a new note on startup when there are no saved notes |

The rest are passed to the running instance if there is one, over the
`server` socket in the configuration directory; otherwise they apply to the
instance being started:

| Option | Effect |
|---|---|
| `-n`, `--new` | Open a new note even if notes already exist |
| `-s`, `--show` | Show all notes |
| `-h`, `--hide` | Hide all notes (with no tray icon, XNote then quits; `--new` does not change that) |
| `-t`, `--toggle` | Show all notes if any is hidden, otherwise hide them all |
| `-f FILE`, `--new-from-file=FILE` | Open a new note with the contents of FILE (may be repeated) |
| `-q`, `--quit` | Save, close every note and exit |

Running plain `xnote` while XNote is already running is the same as
`xnote --new`. Anything the running instance prints is echoed back to the
terminal you ran the command from. `--quit`, `SIGINT` and `SIGTERM` all
save pending changes and flush a pending backup before exiting. Without a
display, `xnote` still answers `--version` and `--help` and forwards the
remote options; anything else prints "XNote is a graphical program. Please
run it from your desktop." and exits.

## Backup and recovery

Live notes are plain text in `~/.config/xnote`; protect that directory as
you would the notes. The optional `xnote-cloud-backup` helper (built when a
Go toolchain is present, and installed by the package) keeps encrypted,
authenticated snapshots of that directory under
`~/.local/share/xnote-backup` and can append complete recovery kits to a
remote. XNote itself never talks to the network: after every save it waits
45 seconds for further edits and then runs `xnote-cloud-backup sync` in the
background; a pending run is started immediately on quit. If the helper is
not installed, XNote logs "xnote-cloud-backup not found in PATH; cloud note
backup is disabled." once and carries on.

### Setting up

Before running `init`, install `secret-tool` (the `libsecret-tools` package). The backup helper needs it to store the master key in your login keyring.

```sh
xnote-cloud-backup init
xnote-cloud-backup backup
xnote-cloud-backup status
```

`init` warns you that losing **both** the passphrase and the recovery code
makes the encrypted backups unrecoverable (the plain notes on this machine
are unaffected), asks for a passphrase twice, derives the key (Argon2id
with about 1 GiB of memory, so it takes a moment), caches the unwrapped
master key in the login keyring through `secret-tool`, and prints the
**recovery code once**: 32 random bytes as base32 in eight-character groups.
Write it down and keep it apart from the passphrase. `init` refuses to run
twice; use `rotate-passphrase` to change the passphrase.

`secret-tool` (package `libsecret-tools`, a recommendation of the `.deb`)
is required: it is the only place the helper keeps the unwrapped key, so
without it `init` cannot cache the key, `unlock` fails the same way, and
`backup` never gets past "master_key not in keyring". Install it before
`init`.

### Commands

| Command | What it does |
|---|---|
| `init` | Create the wrapped master key and print the one-time recovery code |
| `backup` (default; `sync` is an alias used by the app and the timer) | Snapshot the notes locally and, if a remote is configured, push a recovery kit |
| `list` | List local snapshots |
| `status` | Initialised or not, snapshot count, head generation, remote, last remote attempt, success and error |
| `restore <id\|latest> [--force]` | Restore a local snapshot into `~/.config/xnote`; close XNote first |
| `remote-list` | List complete recovery kits on the remote |
| `import <id\|latest> [--force]` | Download a kit and install its keyfile, head and snapshot as the local state |
| `unlock [--recovery]` | Cache the master key again from the passphrase, or from the recovery code |
| `rotate-passphrase` | Wrap the same master key under a new passphrase |
| `show-recovery` | Show whether a recovery slot exists and a fingerprint of its salt (the code itself cannot be shown again) |

State-changing commands take a lock in the state directory; a second one
started at the same time fails with "another XNote backup/recovery
operation is already running". Errors are printed as
`[xnote-cloud-backup] error: …` with exit status 1; an unknown command
prints the usage and exits 2.

### What a snapshot contains and how it is encrypted

A snapshot is a deterministic archive of `~/.config/xnote` restricted to
`content-*`, `info-*`, `default-style` and `*.conf` files. It is refused
when there is no note to back up or a single note exceeds 100 MiB. Each
snapshot is written to `snapshots/<id>.xsnap` with the id
`YYYYMMDDTHHMMSSZ-g<generation>` (UTC time plus a 20-digit generation
counter kept in `head.json`), and the oldest are removed beyond
`KEEP_SNAPSHOTS` (60 by default).

The key hierarchy: a random 32-byte master key; a key-encryption key
derived from the passphrase with Argon2id (time 4, memory 1 GiB, four
threads); the master key wrapped with XChaCha20-Poly1305 in `keyfile.json`,
once under the passphrase and once, independently, under the recovery code;
a fresh data key per snapshot, wrapped under an HKDF-SHA256 derivation of
the master key; and the archive itself encrypted in 64 KiB
XChaCha20-Poly1305 chunks with a final-chunk marker, so truncation and
tampering are detected. The generation counter in the snapshot header
stops a restore from silently rolling back to an older snapshot unless you
pass `--force`.

### Remotes

Configuration lives in `~/.config/xnote/cloud-backup.conf`, one `KEY=value`
per line, `#` for comments:

| Key | Meaning |
|---|---|
| `KEEP_SNAPSHOTS` | Local snapshots to keep (default 60) |
| `CLOUD_REMOTE` | Empty (the default) means local-only. A path starting with `/`, `~` or `.` is a mounted filesystem, copied with `rsync`. `name:path` is an rclone remote and needs `rclone` plus a one-time `rclone config`. A bare word is rejected so that a misspelt remote name cannot become a false local success |
| `CLOUD_DEVICE` | Directory name for this machine on the remote (default: the hostname). Set it to the original name when recovering on a differently named machine |
| `CLOUD_TIMEOUT` | Seconds allowed for each transfer command (default 120) |

Each successful backup appends one complete kit at
`<remote>/<device>/kits/<snapshot-id>/` holding `manifest.json`,
`keyfile.json`, `head.json` and `snapshots/<snapshot-id>.xsnap`. The
manifest is written last and is the completion marker; nothing on the
remote is ever deleted by the helper, so retention there is yours to
arrange and should keep several complete kits. The note contents are
encrypted, but the provider can see the device name, timestamps, sizes,
the number of kits and how often you back up.

### The hourly timer

The package installs a systemd user timer, disabled by default. After
`init` and a successful manual `backup`:

```sh
systemctl --user enable --now xnote-cloud-backup.timer
```

It runs `xnote-cloud-backup sync` every hour with up to five minutes of
random delay, at low CPU and I/O priority, and runs a slot it missed
while the machine was off as soon as the timer is next active. The in-app trigger after each save works
regardless of the timer; the timer is the safety net for edits made while
XNote was not running, or when a push failed.

### Restoring on this machine

```sh
xnote --quit
xnote-cloud-backup list
xnote-cloud-backup restore latest
xnote
```

`restore` refuses to run while an XNote instance answers on its `server`
socket (a stale socket file left by a crash does not count) and refuses to
go back to a snapshot older than the current head generation; `--force`
overrides both, for when you know no other writer is active.

### Recovering on a new machine

Install XNote, and rclone if the remote is an rclone one (then `rclone
config` to sign in). Set `CLOUD_REMOTE` and `CLOUD_DEVICE` in
`cloud-backup.conf` to the original values, then:

```sh
xnote-cloud-backup remote-list
xnote-cloud-backup import latest
xnote-cloud-backup unlock            # or: unlock --recovery
xnote-cloud-backup restore latest
```

`import` refuses to overwrite existing local backup state unless you pass
`--force`, in which case the old state is kept under a `pre-import-*`
directory. After an import the cached key is cleared and new backups are
blocked until you `unlock` with the passphrase, or `unlock --recovery` with
the recovery code if the passphrase is lost.

## Environment variables

| Variable | Effect |
|---|---|
| `XPAD_GDK_BACKEND` | If set and non-empty, copied into `GDK_BACKEND` before GTK starts. Off by default; CI uses it to pin a backend inside a headless compositor |
| `XPAD_GSK_RENDERER` | Likewise copied into `GSK_RENDERER`; the escape hatch if a particular GPU needs a different renderer |
| `XPAD_CLOUD_BACKUP_HELPER` | Path to the backup helper to run instead of `xnote-cloud-backup` from `PATH` |
| `XDG_CONFIG_HOME` | Notes and settings live in `$XDG_CONFIG_HOME/xnote` (default `~/.config/xnote`); honoured by XNote and the helper |
| `XDG_DATA_HOME` | The helper's state lives in `$XDG_DATA_HOME/xnote-backup` (default `~/.local/share/xnote-backup`) |

## Placement on Wayland

On Wayland an application cannot place its own windows, cannot ask where
they are, and cannot mark them "on every workspace" or "not in the task
switcher": the compositor decides all of that. So since 3.0.0 XNote does not
try. The size of a note is remembered; the position is not, and notes appear
in the task switcher like any other window. The old preferences for hiding
notes from the taskbar and task switcher are gone for the same reason. The
`x` and `y` lines in each `info-` file are kept as they were so that an
older XNote reads them unchanged, but nothing writes to them any more.

If you use GNOME and want each note back on the workspace and monitor you
left it on, the companion GNOME Shell extension
[XNote Placement](https://github.com/spencernorton/xnote-placement) does
that from the compositor side, where it is allowed. XNote opens its notes in
the order of their `info-` file names, the order in which the extension
tells notes that share a first line apart.

## Troubleshooting

**No tray icon on GNOME.** GNOME Shell needs the AppIndicator and
KStatusNotifierItem support extension; XNote has nothing to register with
otherwise. Until it is installed, closing the last visible note quits XNote
(see Tray). On other desktops make sure a StatusNotifierWatcher (a panel
with tray support) is running.

**XNote quits as soon as it starts.** There is no tray icon, no visible
note and no saved note, so the startup rule quits rather than run
unreachable. Start it with `xnote --new`, or turn the tray off in
Preferences → Tray so that startup always opens the notes, or install the
tray extension.

**`xnote --hide` exits immediately.** Same rule: with no tray icon there
would be nothing to click, and `--new` does not help because `--hide`
closes that note too. Use `--hide` only where a tray icon exists.

**XNote started but no note appeared.** With the tray enabled and *Display
pads* at *Restore to previous state*, notes that were hidden when you last
quit stay hidden, and on a first run the new note is created hidden. Open
them from the tray menu, or run `xnote --show` (or `xnote --new`).

**My Xpad notes did not come across.** Migration only happens when
`~/.config/xnote` does not exist at all. If XNote has already run once (or
something created the directory), quit XNote, move or remove the
`~/.config/xnote` directory, and start XNote again with `~/.config/xpad`
still in place. If the migration failed with "XNote cannot migrate notes",
the partial copy has been deleted; move the files by hand as the message
says.

**"An existing xpad instance is running."** XNote found a live listener on
`~/.config/xpad/server`. Quit Xpad (a stale socket from a crashed Xpad is
ignored, only a live one blocks) and start XNote again.

**"Could not write to file …".** A save failed (a full disk, or a
permission problem). XNote shows the error, keeps the note marked as
unsaved, and writes it again on your next edit to that note or when XNote
quits, so fix the cause and the note will be written; nothing is silently
dropped.

**A note is empty and refuses to save.** Its `content-` file could not be
read at startup; XNote leaves it unloaded rather than overwrite it. Fix the
file's permissions and press `F5` on that note.

**`backup` fails with "not initialised; run 'xnote-cloud-backup init' first".** The helper
has never been initialised on this machine; see Setting up.

**`backup` fails with "master_key not in keyring".** The key is not cached
in this login session. Run `xnote-cloud-backup unlock`; if `unlock` itself
fails with "secret-tool not found", install `libsecret-tools` first.

**`backup` fails with "imported recovery state must be unlocked before backup".**
A recovery kit was just imported; `unlock` (with the passphrase, or
`unlock --recovery` with the recovery code) caches its key.

**`restore` says XNote is running.** Quit XNote first (`xnote --quit`);
the helper connects to the `server` socket and only a live instance
answers. Use `--force` only if you are sure nothing else is writing to
`~/.config/xnote`.

**Windows have no title bar.** That is the default; turn on Preferences →
View → *Show window decorations*.

## Where your data lives

| Path | What it holds |
|---|---|
| `~/.config/xnote/default-style` | Global settings, one `key value` per line; `NULL` for a colour or font means "follow the theme" |
| `~/.config/xnote/content-XXXXXX` | One note's text, with bold/italic/underline/strikethrough markup inline |
| `~/.config/xnote/info-XXXXXX` | That note's size, colours, font, hidden state and the name of its content file |
| `~/.config/xnote/server` | The single-instance socket a second `xnote` command connects to |
| `~/.config/xnote/cloud-backup.conf` | Backup helper configuration (optional) |
| `~/.config/autostart/xnote.desktop` | A link to the installed desktop file while autostart is on |
| `~/.local/share/xnote-backup/keyfile.json` | The master key, wrapped under the passphrase and under the recovery code |
| `~/.local/share/xnote-backup/head.json` | The snapshot generation counter (anti-rollback) |
| `~/.local/share/xnote-backup/snapshots/*.xsnap` | Encrypted local snapshots |
| `~/.local/share/xnote-backup/backup.lock`, `remote-status.json` | The operation lock and the non-secret record of the last remote attempt |
| `<remote>/<device>/kits/<snapshot-id>/` | One complete recovery kit per backup on the configured remote |

Saves are written to a temporary file and renamed over the original, so a
crash mid-write leaves the previous version intact. Edits are flushed to
disk on a four-second tick; deleting and quitting save at once, and closing
records the hidden state on the next tick.
