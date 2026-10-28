// xnote-cloud-backup — Phase A: encrypted local snapshots.
//
// Replaces the bash helper of the same name (scripts/xnote-cloud-backup).
// The GTK app (src/xpad-backup.c) spawns this by name via g_find_program_in_path;
// no changes there are needed.
//
// CLI:
//
//	xnote-cloud-backup init              — interactive: passphrase x2, print recovery code, cache key
//	xnote-cloud-backup backup            — default; fails if not initialised
//	xnote-cloud-backup restore <id|latest> [--force]
//	xnote-cloud-backup list
//	xnote-cloud-backup remote-list       — list complete off-box recovery kits
//	xnote-cloud-backup import <id|latest> [--force] — fetch/validate recovery state
//	xnote-cloud-backup status
//	xnote-cloud-backup unlock [--recovery] — re-cache master_key (from the
//	                                       passphrase, or from the recovery code
//	                                       with --recovery if the passphrase is lost)
//	xnote-cloud-backup rotate-passphrase — rewrap master_key under new passphrase
//	xnote-cloud-backup show-recovery     — show recovery-slot status and fingerprint
//
// The old bash commands "sync" is accepted as an alias for "backup".
package main

import (
	"fmt"
	"os"
)

func main() {
	cmd := "backup"
	if len(os.Args) > 1 {
		cmd = os.Args[1]
	}

	var err error
	switch cmd {
	case "init":
		err = withStateLock(cmdInit)
	case "backup":
		err = withStateLock(cmdBackup)
	case "sync":
		err = withStateLock(cmdSync)
	case "restore":
		args := os.Args[2:]
		err = withStateLock(func() error { return cmdRestore(args) })
	case "list":
		err = cmdList()
	case "remote-list":
		err = cmdRemoteList()
	case "import":
		args := os.Args[2:]
		err = withStateLock(func() error { return cmdImport(args) })
	case "status":
		err = cmdStatus(os.Args[2:]...)
	case "set-target":
		args := os.Args[2:]
		err = withStateLock(func() error { return cmdSetTarget(args) })
	case "connect":
		args := os.Args[2:]
		err = withStateLock(func() error { return cmdConnect(args) })
	case "disconnect":
		args := os.Args[2:]
		err = withStateLock(func() error { return cmdDisconnect(args) })
	case "unlock":
		err = runUnlock(os.Args[2:])
	case "rotate-passphrase":
		err = withStateLock(cmdRotatePassphrase)
	case "rotate-key":
		err = withStateLock(cmdRotateKey)
	case "show-recovery":
		err = cmdShowRecovery()
	case "test-recovery":
		args := os.Args[2:]
		err = withStateLock(func() error { return cmdTestRecovery(args) })
	case "import-keys":
		args := os.Args[2:]
		err = withStateLock(func() error { return cmdImportKeys(args) })
	case "export-keys":
		args := os.Args[2:]
		err = withStateLock(func() error { return cmdExportKeys(args) })
	case "set-crypto":
		args := os.Args[2:]
		err = withStateLock(func() error { return cmdSetCrypto(args) })
	case "set-mirror":
		args := os.Args[2:]
		err = withStateLock(func() error { return cmdSetMirror(args) })
	default:
		fmt.Fprintf(os.Stderr, "usage: xnote-cloud-backup [init|backup|sync|restore <id|latest>|list|remote-list|import <id|latest>|status [--json|--porcelain]|set-target <local|gdrive|dropbox|custom> [remote]|set-mirror <remote|none>|set-crypto <classic|hybrid>|connect <gdrive|dropbox>|disconnect <gdrive|dropbox>|unlock|rotate-passphrase|rotate-key|show-recovery|test-recovery [latest|<id>] [--remote]|export-keys <dest-dir>]\n")
		os.Exit(2)
	}

	if err != nil {
		fmt.Fprintf(os.Stderr, "[xnote-cloud-backup] error: %v\n", err)
		os.Exit(1)
	}
}

func runUnlock(args []string) error {
	return withStateLock(func() error { return cmdUnlock(args) })
}
