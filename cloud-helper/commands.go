// commands.go — CLI verb implementations for xnote-cloud-backup.

package main

import (
	"bytes"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"time"
)

func log(format string, args ...any) {
	fmt.Printf("[xnote-cloud-backup] "+format+"\n", args...)
}

// isInitialised returns true if keyfile.json exists.
func isInitialised() bool {
	_, err := os.Stat(keyfilePath())
	return err == nil
}

// loadMasterKey retrieves master_key: from keyring first, falls back to nil.
func loadMasterKey() ([]byte, error) {
	mk, err := KeyringLoad()
	if err != nil {
		return nil, err
	}
	if mk != nil && len(mk) != 32 {
		return nil, fmt.Errorf("keyring returned master key with wrong length %d (expected 32); re-run unlock", len(mk))
	}
	return mk, nil
}

// cmdInit implements the `init` verb.
// Reads passphrase twice, generates master_key + recovery bytes, writes keyfile.json,
// caches master_key in keyring, prints recovery code.
func cmdInit() error {
	if isInitialised() {
		return fmt.Errorf("already initialised (keyfile exists at %s); use 'rotate-passphrase' to change passphrase", keyfilePath())
	}

	fmt.Fprintf(os.Stderr, "\n  WARNING: This will encrypt your XNote backups with a passphrase.\n")
	fmt.Fprintf(os.Stderr, "  If you lose BOTH your passphrase AND your recovery code, your\n")
	fmt.Fprintf(os.Stderr, "  encrypted backups will be UNRECOVERABLE (the local plaintext\n")
	fmt.Fprintf(os.Stderr, "  ~/.config/xnote remains accessible while on this device).\n\n")
	fmt.Fprintf(os.Stderr, "  The local ~/.config/xnote is plaintext at rest; E2EE only\n")
	fmt.Fprintf(os.Stderr, "  protects the encrypted snapshots and (future) cloud copies.\n\n")

	passphrase, err := readPassphraseTwice("Enter backup passphrase: ", "Confirm passphrase: ")
	if err != nil {
		return err
	}

	// Generate master_key (32 random bytes)
	masterKey, err := randBytes(32)
	if err != nil {
		return fmt.Errorf("generate master_key: %w", err)
	}

	// Generate recovery bytes (32 random bytes = 256-bit)
	recoveryBytes, err := randBytes(32)
	if err != nil {
		return fmt.Errorf("generate recovery bytes: %w", err)
	}

	fmt.Fprintf(os.Stderr, "\n  Deriving key (Argon2id, ~1 GiB RAM, may take a moment)...\n")

	kf, err := BuildKeyfile(masterKey, passphrase, recoveryBytes)
	if err != nil {
		return fmt.Errorf("build keyfile: %w", err)
	}

	if err := os.MkdirAll(stateDir(), 0700); err != nil {
		return fmt.Errorf("create state dir: %w", err)
	}
	if err := SaveKeyfile(kf); err != nil {
		return fmt.Errorf("save keyfile: %w", err)
	}

	// Cache master_key in keyring
	if err := KeyringStore(masterKey); err != nil {
		fmt.Fprintf(os.Stderr, "[xnote-cloud-backup] warning: keyring store failed: %v\n", err)
		fmt.Fprintf(os.Stderr, "[xnote-cloud-backup] Run 'xnote-cloud-backup unlock' after login to cache key.\n")
	}

	// Print recovery code — shown ONCE
	recoveryCode := RecoveryBytesToCode(recoveryBytes)
	fmt.Printf("\n")
	fmt.Printf("  ============================================================\n")
	fmt.Printf("  RECOVERY CODE (write this down — shown ONLY ONCE):\n\n")
	fmt.Printf("  %s\n\n", recoveryCode)
	fmt.Printf("  Store it somewhere safe and SEPARATE from your passphrase.\n")
	fmt.Printf("  ============================================================\n\n")

	log("initialised. Keyfile: %s", keyfilePath())
	return nil
}

// cmdBackup implements the `backup` verb (always creates a new snapshot generation).
func cmdBackup() error {
	return runBackup(false)
}

// cmdSync implements the `sync` verb (used by the systemd timer and XNote's
// debounced auto-save trigger: skips minting a duplicate snapshot generation
// when the deterministic note archive matches the validated local head, while
// still pushing any unsynced head snapshot to the configured cloud remote).
func cmdSync() error {
	return runBackup(true)
}

func runBackup(skipIfUnchanged bool) error {
	if !isInitialised() {
		return fmt.Errorf("not initialised; run 'xnote-cloud-backup init' first")
	}
	if _, err := LoadKeyfile(); err != nil {
		return fmt.Errorf("refusing backup with invalid keyfile: %w", err)
	}
	if _, err := os.Stat(unlockRequiredPath()); err == nil {
		return fmt.Errorf("imported recovery state must be unlocked before backup; run 'xnote-cloud-backup unlock' or 'unlock --recovery'")
	} else if !os.IsNotExist(err) {
		return fmt.Errorf("check imported recovery lock: %w", err)
	}

	cfg, err := LoadConfig()
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}
	configD := configDir()
	if err := validateBackupSource(configD); err != nil {
		return err
	}

	masterKey, err := loadMasterKey()
	if err != nil {
		return fmt.Errorf("keyring: %w", err)
	}
	if masterKey == nil {
		return fmt.Errorf("master_key not in keyring; run 'xnote-cloud-backup unlock' to cache it")
	}

	if err := os.MkdirAll(snapshotsDir(), 0700); err != nil {
		return fmt.Errorf("mkdir snapshots: %w", err)
	}

	// Load head for anti-rollback state
	head, err := LoadHead()
	if err != nil {
		return fmt.Errorf("load head: %w", err)
	}

	// Seed generation from max(head.Generation, max across existing snapshot headers) + 1.
	// This prevents a deleted head.json from resetting the counter and causing SnapshotID collisions.
	nextGen := head.Generation + 1
	snapIDs, _ := ListSnapshots()
	if len(snapIDs) > 0 {
		for _, sid := range snapIDs {
			sf, ferr := os.Open(SnapshotPath(sid))
			if ferr != nil {
				continue
			}
			sh, herr := ReadHeader(sf)
			sf.Close()
			if herr != nil {
				continue
			}
			if sh.Generation+1 > nextGen {
				nextGen = sh.Generation + 1
			}
		}
	}

	// --- Build tar in memory (notes are text files, total << RAM) ---
	// The whole tar is held in memory; stream it through io.Pipe if notes grow huge.
	var tarBuf bytes.Buffer
	nFiles, noteFiles, err := WriteTarWithNoteCount(&tarBuf, configD)
	if err != nil {
		return fmt.Errorf("tar: %w", err)
	}
	if noteFiles == 0 {
		return fmt.Errorf("refusing backup: archive contains no content-* note files")
	}

	if skipIfUnchanged && len(snapIDs) > 0 {
		latestID := snapIDs[len(snapIDs)-1]
		if snapshotMatchesTar(latestID, head, masterKey, tarBuf.Bytes()) {
			if cfg.CloudRemote == "" {
				log("notes unchanged since %s (gen=%d); skipping duplicate snapshot", latestID, head.Generation)
				return nil
			}
			host, err := remoteHost(cfg, hostname())
			if err != nil {
				return err
			}
			if status, err := loadRemoteStatus(); err == nil &&
				status.LastSuccessSnapshotID == latestID &&
				status.Device == host &&
				status.LastError == "" {
				log("notes unchanged since %s (gen=%d); local and cloud backup up to date", latestID, head.Generation)
				return nil
			}
			return pushSnapshotToRemote(cfg, latestID, host)
		}
	}

	// Generate DEK
	dek, wrappedDEK, err := generateDEK(masterKey)
	if err != nil {
		return fmt.Errorf("generate DEK: %w", err)
	}

	// Build snapshot header
	hdr := &SnapshotHeader{
		Generation:       nextGen,
		ParentCipherHash: [32]byte{},
		WrappedDEK:       wrappedDEK,
	}
	copy(hdr.ParentCipherHash[:], head.HeadHash)

	// Write snapshot to a temp file first, then rename atomically.
	snapID := SnapshotID(hdr.Generation)
	snapPath := SnapshotPath(snapID)
	tmpPath := snapPath + ".tmp"

	// Generate base_nonce before constructing the header, so the full header
	// (including base_nonce) can be used as AAD for chunk 0. This avoids a
	// two-pass encrypt: the nonce is chosen up front, goes into the header,
	// and is also passed to EncryptStream as the base nonce.
	baseNonceBytes, err := randBytes(xchacha20NonceLen)
	if err != nil {
		return fmt.Errorf("generate base nonce: %w", err)
	}
	copy(hdr.BaseNonce[:], baseNonceBytes)

	// Serialise the complete header to use as AAD.
	var headerBuf bytes.Buffer
	headerAAD, err := WriteHeader(&headerBuf, hdr)
	if err != nil {
		return fmt.Errorf("serialise header: %w", err)
	}

	// Encrypt stream with the pre-committed base nonce.
	var bodyBuf bytes.Buffer
	retNonce, err := EncryptStreamWithNonce(&bodyBuf, bytes.NewReader(tarBuf.Bytes()), dek, headerAAD, baseNonceBytes)
	if err != nil {
		return fmt.Errorf("encrypt stream: %w", err)
	}
	// Sanity: returned nonce must match the one we committed to in the header.
	if !bytes.Equal(retNonce, baseNonceBytes) {
		return fmt.Errorf("internal error: nonce mismatch after encryption")
	}

	// Write final file: header || encrypted body
	tmpFile, err := os.OpenFile(tmpPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		return fmt.Errorf("create tmp snapshot: %w", err)
	}
	defer func() { os.Remove(tmpPath) }()

	if _, err := tmpFile.Write(headerBuf.Bytes()); err != nil {
		tmpFile.Close()
		return fmt.Errorf("write header: %w", err)
	}
	if _, err := tmpFile.Write(bodyBuf.Bytes()); err != nil {
		tmpFile.Close()
		return fmt.Errorf("write body: %w", err)
	}
	tmpFile.Close()

	// Hash the entire ciphertext file for anti-rollback
	cipherHash, err := hashFile(tmpPath)
	if err != nil {
		return fmt.Errorf("hash snapshot: %w", err)
	}

	// Finalise: hard-link tmp → final. Link fails if snapPath already exists,
	// so a concurrent backup computing the same generation cannot silently
	// clobber an existing snapshot (the deferred cleanup removes the tmp link).
	if err := os.Link(tmpPath, snapPath); err != nil {
		return fmt.Errorf("finalise snapshot (already exists?): %w", err)
	}

	// Update head
	head.Generation = hdr.Generation
	head.HeadHash = cipherHash[:]
	if err := SaveHead(head); err != nil {
		return fmt.Errorf("save head: %w", err)
	}

	// Rotate old snapshots
	if err := RotateSnapshots(cfg.KeepSnapshots); err != nil {
		log("warning: snapshot rotation failed: %v", err)
	}

	log("snapshot %s (gen=%d, files=%d) saved locally", snapID, hdr.Generation, nFiles)

	// Append-only off-box push. A configured remote is part of backup success:
	// systemd must report a failure when the local snapshot is safe but the
	// recovery kit did not reach Dropbox/iCloud/NAS.
	if cfg.CloudRemote != "" {
		host, err := remoteHost(cfg, hostname())
		if err != nil {
			return err
		}
		if err := pushSnapshotToRemote(cfg, snapID, host); err != nil {
			return err
		}
	}

	return nil
}

func pushSnapshotToRemote(cfg Config, snapID, host string) error {
	kitDir, err := buildRecoveryKit(snapID, host)
	if err != nil {
		_ = recordRemoteAttempt(snapID, host, err)
		return fmt.Errorf("local snapshot is safe, but recovery-kit staging failed: %w", err)
	}
	defer os.RemoveAll(kitDir)
	pushErr := pushRecoveryKit(cfg, kitDir, host)
	statusErr := recordRemoteAttempt(snapID, host, pushErr)
	if pushErr != nil {
		return fmt.Errorf("local snapshot is safe, but cloud push failed: %w", pushErr)
	}
	if statusErr != nil {
		return fmt.Errorf("cloud push succeeded, but recording success status failed: %w", statusErr)
	}
	log("cloud recovery kit OK -> %s/%s/kits/%s", cfg.CloudRemote, host, snapID)
	return nil
}

func snapshotMatchesTar(snapID string, head *HeadState, masterKey, tarBytes []byte) bool {
	if head == nil || head.Generation == 0 || len(head.HeadHash) != 32 {
		return false
	}
	snapPath := SnapshotPath(snapID)
	hash, err := hashFile(snapPath)
	if err != nil || !bytes.Equal(head.HeadHash, hash[:]) {
		return false
	}
	f, err := os.Open(snapPath)
	if err != nil {
		return false
	}
	defer f.Close()
	hdr, err := ReadHeader(f)
	if err != nil || hdr.Generation != head.Generation {
		return false
	}
	dek, err := unwrapDEK(masterKey, hdr.WrappedDEK)
	if err != nil {
		return false
	}
	defer zero(dek)
	var prevTar bytes.Buffer
	if err := DecryptStream(&prevTar, f, dek, hdr.BaseNonce[:], hdr.HeaderBytes); err != nil {
		return false
	}
	return bytes.Equal(prevTar.Bytes(), tarBytes)
}

// cmdRestore implements `restore <id|latest> [--force]`.
func cmdRestore(args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: restore <snapshot-id|latest> [--force]")
	}
	snapID := args[0]
	force := false
	for _, a := range args[1:] {
		if a == "--force" {
			force = true
		} else {
			return fmt.Errorf("unknown option: %s", a)
		}
	}

	if !isInitialised() {
		return fmt.Errorf("not initialised")
	}
	masterKey, err := loadMasterKey()
	if err != nil {
		return fmt.Errorf("keyring: %w", err)
	}
	if masterKey == nil {
		return fmt.Errorf("master_key not in keyring; run 'unlock' first")
	}

	// Refuse while xnote runs (check server socket)
	if !force && xnoteIsRunning() {
		return fmt.Errorf("xnote is running — close it first (or pass --force)")
	}

	// Resolve snapshot
	if snapID == "latest" {
		ids, err := ListSnapshots()
		if err != nil || len(ids) == 0 {
			return fmt.Errorf("no snapshots found")
		}
		snapID = ids[len(ids)-1]
	}
	if err := validateSnapshotID(snapID); err != nil {
		return err
	}
	snapPath := SnapshotPath(snapID)

	// Rollback guard: check generation
	head, err := LoadHead()
	if err != nil {
		return fmt.Errorf("load head: %w", err)
	}

	// Open snapshot file
	f, err := os.Open(snapPath)
	if err != nil {
		return fmt.Errorf("open snapshot %s: %w", snapID, err)
	}
	defer f.Close()

	// Read header
	hdr, err := ReadHeader(f)
	if err != nil {
		return fmt.Errorf("read header: %w", err)
	}

	// Rollback guard
	if !force && head.Generation > 0 && hdr.Generation < head.Generation {
		return fmt.Errorf("rollback rejected: snapshot gen=%d < head gen=%d (pass --force to override)", hdr.Generation, head.Generation)
	}

	// Verify head.HeadHash matches the actual on-disk snapshot ciphertext,
	// so a swapped-but-higher-generation snapshot is detected.
	if !force && head.Generation > 0 && bytes.Equal(head.HeadHash, make([]byte, len(head.HeadHash))) == false {
		// Only verify when head points at this specific snapshot (latest restore chain).
		// We verify that the file's sha256 matches head.HeadHash when restoring the
		// snapshot that was the head at time of last backup.
		if hdr.Generation == head.Generation {
			actualHash, hashErr := hashFile(snapPath)
			if hashErr != nil {
				return fmt.Errorf("cannot hash snapshot for integrity check: %w", hashErr)
			}
			if !bytes.Equal(actualHash[:], head.HeadHash) {
				return fmt.Errorf("snapshot integrity check failed: on-disk hash does not match head.json record (possible swap attack)")
			}
		}
	}

	// Unwrap DEK
	dek, err := unwrapDEK(masterKey, hdr.WrappedDEK)
	if err != nil {
		return fmt.Errorf("unwrap DEK: %w", err)
	}

	// Decrypt stream into tar buffer
	var tarBuf bytes.Buffer
	if err := DecryptStream(&tarBuf, f, dek, hdr.BaseNonce[:], hdr.HeaderBytes); err != nil {
		return fmt.Errorf("decrypt: %w", err)
	}

	// Refuse empty archive
	if tarBuf.Len() == 0 {
		return fmt.Errorf("refusing: decrypted archive is empty")
	}

	// Extract to temp dir first, then atomically swap into destination.
	// This means: (1) stale files in configDir not present in snapshot are removed,
	// (2) on extract failure the original is left untouched.
	configD := configDir()

	// Pre-restore safety copy. A partial safety copy is not safe, so no restore
	// proceeds after a walk, read, write, or close failure (even with --force).
	if _, statErr := os.Stat(configD); statErr == nil {
		keep := filepath.Join(stateDir(), "pre-restore-"+time.Now().UTC().Format("20060102T150405Z"))
		if err := copyDirStrict(configD, keep); err != nil {
			return fmt.Errorf("pre-restore copy failed: %w", err)
		}
		log("current notes preserved at %s", keep)
	} else if !os.IsNotExist(statErr) {
		return fmt.Errorf("inspect live config before pre-restore copy: %w", statErr)
	}

	// Extract into a fresh temp sibling dir.
	tmpDest := configD + ".restore-tmp"
	if err := os.RemoveAll(tmpDest); err != nil {
		return fmt.Errorf("clear restore tmp: %w", err)
	}
	if err := os.MkdirAll(tmpDest, 0700); err != nil {
		return fmt.Errorf("mkdir restore tmp: %w", err)
	}
	if err := ExtractTar(bytes.NewReader(tarBuf.Bytes()), tmpDest); err != nil {
		os.RemoveAll(tmpDest) // clean up; original untouched
		return fmt.Errorf("extract: %w", err)
	}

	// Atomic swap: rename live aside, rename temp into place.
	oldDest := configD + ".restore-old"
	os.RemoveAll(oldDest)
	if _, statErr := os.Stat(configD); statErr == nil {
		if err := os.Rename(configD, oldDest); err != nil {
			os.RemoveAll(tmpDest)
			return fmt.Errorf("rename live dir aside: %w", err)
		}
	}
	if err := os.Rename(tmpDest, configD); err != nil {
		if rbErr := os.Rename(oldDest, configD); rbErr != nil {
			return fmt.Errorf("CRITICAL: restore failed AND rollback failed — your notes are preserved at %s; move them back to %s manually (restore err: %v; rollback err: %w)", oldDest, configD, err, rbErr)
		}
		return fmt.Errorf("rename restored dir into place (rolled back to original): %w", err)
	}
	os.RemoveAll(oldDest)

	log("restored snapshot %s (gen=%d) → %s", snapID, hdr.Generation, configD)
	log("start xnote again with: systemd-run --user --collect --unit=xnote-$RANDOM xnote")
	return nil
}

// cmdList implements the `list` verb.
func cmdList() error {
	ids, err := ListSnapshots()
	if err != nil {
		return err
	}
	if len(ids) == 0 {
		fmt.Println("No local snapshots.")
		return nil
	}
	head, _ := LoadHead()
	fmt.Printf("Local snapshots (%s):\n", snapshotsDir())
	for _, id := range ids {
		marker := ""
		if head != nil && len(ids) > 0 && id == ids[len(ids)-1] {
			marker = " (latest)"
		}
		fmt.Printf("  %s%s\n", id, marker)
	}
	if head != nil && head.Generation > 0 {
		fmt.Printf("Head: generation=%d\n", head.Generation)
	}
	return nil
}

// cmdRemoteList lists only complete remote kits (manifest.json is the remote
// completion marker).
func cmdRemoteList() error {
	cfg, err := LoadConfig()
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}
	host, err := remoteHost(cfg, hostname())
	if err != nil {
		return err
	}
	ids, err := listRemoteKits(cfg, host)
	if err != nil {
		return err
	}
	if len(ids) == 0 {
		fmt.Printf("No complete remote recovery kits for %s.\n", host)
		return nil
	}
	fmt.Printf("Remote recovery kits for %s:\n", host)
	for i, id := range ids {
		marker := ""
		if i == len(ids)-1 {
			marker = " (latest)"
		}
		fmt.Printf("  %s%s\n", id, marker)
	}
	return nil
}

// cmdImport downloads and validates a complete recovery kit, then installs its
// keyfile/head/snapshot into local state. Existing unlock + restore commands
// complete either the passphrase or recovery-code recovery flow.
func cmdImport(args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: import <snapshot-id|latest> [--force]")
	}
	requested := args[0]
	force := false
	for _, arg := range args[1:] {
		if arg == "--force" {
			force = true
		} else {
			return fmt.Errorf("unknown option: %s", arg)
		}
	}

	cfg, err := LoadConfig()
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}
	host, err := remoteHost(cfg, hostname())
	if err != nil {
		return err
	}
	if err := os.MkdirAll(stateDir(), 0700); err != nil {
		return err
	}
	fetchDir, err := os.MkdirTemp(stateDir(), ".remote-fetch-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(fetchDir)
	id, err := fetchRecoveryKit(cfg, requested, host, fetchDir)
	if err != nil {
		return err
	}
	preservedAt, err := installRecoveryKit(fetchDir, force)
	if err != nil {
		return err
	}
	log("imported recovery kit %s", id)
	if preservedAt != "" {
		log("previous local recovery state preserved at %s", preservedAt)
	}
	if err := KeyringDelete(); err != nil {
		return fmt.Errorf("recovery state was imported, but the stale cached master key could not be cleared; clear the XNote backup key from the keyring before backup: %w", err)
	}
	log("next: run 'xnote-cloud-backup unlock' (or 'unlock --recovery'), then 'restore latest'")
	return nil
}

// cmdUnlock re-caches master_key from passphrase on a new device.
// cmdUnlock re-caches master_key in the keyring. By default it derives from the
// passphrase; with --recovery it derives from the recovery code instead, which
// is the whole point of the recovery slot generated at init — the ONLY way back
// in if the passphrase is lost (previously the recovery slot was unreachable, so
// the disaster-recovery feature was dead code).
func cmdUnlock(args []string) error {
	if !isInitialised() {
		return fmt.Errorf("not initialised")
	}
	kf, err := LoadKeyfile()
	if err != nil {
		return err
	}

	useRecovery := false
	for _, a := range args {
		if a == "--recovery" {
			useRecovery = true
		}
	}

	var masterKey []byte
	if useRecovery {
		code, cerr := readPassphrase("Enter recovery code: ")
		if cerr != nil {
			return cerr
		}
		recoveryBytes, cerr := RecoveryCodeToBytes(string(code))
		if cerr != nil {
			return fmt.Errorf("invalid recovery code: %w", cerr)
		}
		fmt.Fprintf(os.Stderr, "Deriving key (Argon2id, ~1 GiB RAM)...\n")
		masterKey, cerr = UnwrapMasterKeyFromRecovery(kf, recoveryBytes)
		if cerr != nil {
			return cerr
		}
	} else {
		passphrase, perr := readPassphrase("Enter backup passphrase: ")
		if perr != nil {
			return perr
		}
		fmt.Fprintf(os.Stderr, "Deriving key (Argon2id, ~1 GiB RAM)...\n")
		masterKey, perr = UnwrapMasterKeyFromPassphrase(kf, passphrase)
		if perr != nil {
			return perr
		}
	}

	if err := KeyringStore(masterKey); err != nil {
		return fmt.Errorf("keyring store: %w", err)
	}
	if err := os.Remove(unlockRequiredPath()); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("master_key cached, but could not clear imported-state unlock requirement: %w", err)
	}
	if useRecovery {
		log("master_key cached in keyring (via recovery code)")
	} else {
		log("master_key cached in keyring")
	}
	return nil
}

// cmdRotatePassphrase rewraps master_key under a new passphrase.
func cmdRotatePassphrase() error {
	if !isInitialised() {
		return fmt.Errorf("not initialised")
	}
	kf, err := LoadKeyfile()
	if err != nil {
		return err
	}
	passphrase, err := readPassphrase("Current passphrase: ")
	if err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "Deriving key (Argon2id, ~1 GiB RAM)...\n")
	masterKey, err := UnwrapMasterKeyFromPassphrase(kf, passphrase)
	if err != nil {
		return err
	}
	newPassphrase, err := readPassphraseTwice("New passphrase: ", "Confirm new passphrase: ")
	if err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "Deriving new key (Argon2id, ~1 GiB RAM)...\n")
	if err := RewrapPassphrase(kf, masterKey, newPassphrase); err != nil {
		return err
	}
	if err := SaveKeyfile(kf); err != nil {
		return err
	}
	// Re-cache with same master_key
	if err := KeyringStore(masterKey); err != nil {
		log("warning: keyring store failed: %v", err)
	}
	log("passphrase rotated")
	return nil
}

// cmdShowRecovery reports the configured recovery slot and its salt fingerprint.
// It cannot reconstruct the one-time recovery code shown during init.
func cmdShowRecovery() error {
	if !isInitialised() {
		return fmt.Errorf("not initialised")
	}
	// We cannot reconstruct the original recovery code from keyfile.json alone
	// (Argon2id is one-way). We can only verify that a provided recovery code
	// works or not.
	fmt.Fprintf(os.Stderr, "Note: The original recovery code cannot be re-displayed from the keyfile\n")
	fmt.Fprintf(os.Stderr, "(it was generated at init and shown once). You can VERIFY a recovery\n")
	fmt.Fprintf(os.Stderr, "code by using 'unlock --recovery'.\n\n")
	kf, err := LoadKeyfile()
	if err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "Recovery slot status: configured\n")
	fmt.Fprintf(os.Stderr, "Recovery slot salt fingerprint (sha256 prefix):\n")
	saltHash := sha256.Sum256(kf.RecoveryParams.Salt)
	fmt.Printf("  recovery_salt_sha256_prefix: %x\n", saltHash[:8])
	return nil
}

// --- helpers ---

// xnoteIsRunning reports whether a live XNote answers on its server socket.
// Only a successful connect counts: XNote never unlinks the socket file on a
// crash, so an existence check would refuse every restore after one (and,
// before 3.1.0, after every normal quit too).
func xnoteIsRunning() bool {
	conn, err := net.Dial("unix", filepath.Join(configDir(), "server"))
	if err != nil {
		return false
	}
	conn.Close()
	return true
}

// readPassphrase reads a passphrase from stdin (no echo via terminal raw mode).
// Falls back to plain read if not a terminal.
func readPassphrase(prompt string) ([]byte, error) {
	fmt.Fprint(os.Stderr, prompt)
	return readLineNoEcho()
}

// readPassphraseTwice reads a passphrase twice and returns it if they match.
func readPassphraseTwice(prompt1, prompt2 string) ([]byte, error) {
	pw1, err := readPassphrase(prompt1)
	if err != nil {
		return nil, err
	}
	pw2, err := readPassphrase(prompt2)
	if err != nil {
		return nil, err
	}
	if !hmacEqual(pw1, pw2) {
		return nil, fmt.Errorf("passphrases do not match")
	}
	if len(pw1) == 0 {
		return nil, fmt.Errorf("passphrase must not be empty")
	}
	return pw1, nil
}

// hashFile computes sha256 of a file.
func hashFile(path string) ([32]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return [32]byte{}, err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return [32]byte{}, err
	}
	var out [32]byte
	copy(out[:], h.Sum(nil))
	return out, nil
}

// copyDir recursively copies src to dst (best-effort, ignores symlinks/sockets).
func copyDir(src, dst string) error {
	return filepath.Walk(src, func(path string, fi os.FileInfo, err error) error {
		if err != nil {
			return nil // skip unreadable
		}
		rel, _ := filepath.Rel(src, path)
		dstPath := filepath.Join(dst, rel)
		if fi.IsDir() {
			return os.MkdirAll(dstPath, 0700)
		}
		if !fi.Mode().IsRegular() {
			return nil // skip sockets etc
		}
		return copyFile(path, dstPath)
	})
}

type walkDirFunc func(root string, walkFn filepath.WalkFunc) error
type copyRegularFileFunc func(src, dst string) error

// copyDirStrict recursively copies regular files and directories while
// propagating every traversal and file-copy failure. Restore uses this variant
// because proceeding with an incomplete pre-restore copy could destroy the only
// intact copy of a note. copyDir retains its historical best-effort semantics
// for any callers that intentionally use it.
func copyDirStrict(src, dst string) error {
	return copyDirStrictWith(src, dst, filepath.Walk, copyFileStrict)
}

func copyDirStrictWith(src, dst string, walk walkDirFunc, copyRegular copyRegularFileFunc) error {
	return walk(src, func(path string, fi os.FileInfo, walkErr error) error {
		if walkErr != nil {
			return fmt.Errorf("walk %s: %w", path, walkErr)
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return fmt.Errorf("relative path for %s: %w", path, err)
		}
		dstPath := filepath.Join(dst, rel)
		if fi.IsDir() {
			if err := os.MkdirAll(dstPath, 0700); err != nil {
				return fmt.Errorf("create directory %s: %w", dstPath, err)
			}
			return nil
		}
		if !fi.Mode().IsRegular() {
			return fmt.Errorf("refusing non-regular entry %s (%s) in pre-restore copy", path, fi.Mode())
		}
		if err := copyRegular(path, dstPath); err != nil {
			return fmt.Errorf("copy %s to %s: %w", path, dstPath, err)
		}
		return nil
	})
}

func copyFileStrict(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return fmt.Errorf("open source: %w", err)
	}

	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		openErr := fmt.Errorf("open destination: %w", err)
		if closeErr := in.Close(); closeErr != nil {
			return errors.Join(openErr, fmt.Errorf("close source: %w", closeErr))
		}
		return openErr
	}
	return copyContentsStrict(out, in)
}

func copyContentsStrict(dst io.WriteCloser, src io.ReadCloser) error {
	_, copyErr := io.Copy(dst, src)
	srcCloseErr := src.Close()
	dstCloseErr := dst.Close()

	var errs []error
	if copyErr != nil {
		errs = append(errs, fmt.Errorf("copy contents: %w", copyErr))
	}
	if srcCloseErr != nil {
		errs = append(errs, fmt.Errorf("close source: %w", srcCloseErr))
	}
	if dstCloseErr != nil {
		errs = append(errs, fmt.Errorf("close destination: %w", dstCloseErr))
	}
	return errors.Join(errs...)
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		return err
	}
	_, err = io.Copy(out, in)
	out.Close()
	return err
}
