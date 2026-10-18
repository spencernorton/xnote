# Security policy

## Reporting a vulnerability

Please report vulnerabilities privately through GitHub:
**[Report a vulnerability](https://github.com/spencernorton/xnote/security/advisories/new)**.
Do not open a public issue, and do not attach real notes, keyfiles, recovery
codes or backup snapshots — the affected version, a description and a minimal
reproduction with disposable test data are enough.

There is no e-mail address for security reports; the advisory form is the
only channel, and it is the one that is monitored. You will get an
acknowledgement within a week. Fixes ship as a tagged release; the advisory
is published once the release is out, and credits you unless you ask
otherwise.

Ordinary bugs with no security impact belong in the
[public issue tracker](https://github.com/spencernorton/xnote/issues/new/choose).

## Supported versions

Only the latest tagged release is supported. XNote has no LTS line.

## Scope

In scope: this repository's code and the artefacts it ships — the `xnote`
application, the `xnote-cloud-backup` helper and the Debian package.
Out of scope: the XNote Placement extension (report to
[its own advisory form](https://github.com/spencernorton/xnote-placement/security/advisories/new)),
the storage providers a backup remote points at, and `rclone` itself.

## What XNote does with your notes

Understanding the trust model helps you judge what is and is not a finding:

- **Live notes are plaintext.** Each note is a pair of files under
  `~/.config/xnote` (`content-*` for the text, `info-*` for size, colour and
  font), written private to your user. The directory is created with mode
  0700. Protect it with the same care as the notes themselves; XNote does not
  encrypt it.
- **The application never uses the network.** `xnote` talks only to your
  session D-Bus (for the tray icon) and to its own single-instance Unix socket,
  `~/.config/xnote/server`. Nothing is sent anywhere, and there is no
  telemetry.
- **Backups are opt-in and encrypted.** The separate `xnote-cloud-backup`
  helper keeps snapshots under `~/.local/share/xnote-backup`, encrypted with
  XChaCha20-Poly1305 under a master key that is wrapped by a passphrase
  (Argon2id) and, independently, by a recovery code. The unwrapped key is
  cached in your libsecret keyring through `secret-tool` so routine backups do
  not prompt. A configured remote (an `rsync` path or an `rclone` remote)
  receives only complete encrypted kits and nothing there is ever deleted, but
  the provider still sees device name, timestamps, sizes, item count and
  cadence.
- **Restore is deliberately cautious.** Snapshots are authenticated chunk by
  chunk, a truncated or tampered file is refused, restore refuses to run while
  XNote is running or to roll back past the current generation without
  `--force`, and archives never contain or follow symlinks.

Anything that breaks one of those statements is a security bug: note contents
or keys leaving the machine other than as an encrypted kit to the remote you
configured; a snapshot that decrypts under the wrong passphrase or accepts a
modified file; `restore` or `import` writing outside the XNote directories;
note or key files created readable by other users. Please report it.
