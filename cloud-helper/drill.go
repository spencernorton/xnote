// drill.go — test recovery, key escrow export, true key rotation, and the
// crypto/mirror settings verbs. Test recovery never writes to the live
// notes directory: it decrypts into memory and verifies hashes first.

package main

import (
	"archive/tar"
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

type DrillStatus struct {
	LastDrillUTC string `json:"last_drill_utc,omitempty"`
	SnapshotID   string `json:"snapshot_id,omitempty"`
	Result       string `json:"result,omitempty"`
	Detail       string `json:"detail,omitempty"`
}

func drillStatusPath() string { return filepath.Join(stateDir(), "drill-status.json") }

func saveDrillStatus(snapID string, drillErr error) {
	st := DrillStatus{LastDrillUTC: time.Now().UTC().Format(time.RFC3339), SnapshotID: snapID, Result: "passed"}
	if drillErr != nil {
		st.Result = "failed"
		st.Detail = drillErr.Error()
	}
	data, err := json.MarshalIndent(st, "", "  ")
	if err != nil {
		return
	}
	data = append(data, '\n')
	_ = os.WriteFile(drillStatusPath()+".tmp", data, 0600)
	_ = os.Rename(drillStatusPath()+".tmp", drillStatusPath())
}

func loadDrillStatus() *DrillStatus {
	data, err := os.ReadFile(drillStatusPath())
	if err != nil {
		return &DrillStatus{}
	}
	var st DrillStatus
	if err := json.Unmarshal(data, &st); err != nil {
		return &DrillStatus{}
	}
	return &st
}

func countTarNotes(tarBytes []byte) (files, notes int, err error) {
	tr := tar.NewReader(bytes.NewReader(tarBytes))
	for {
		hdr, rerr := tr.Next()
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			return 0, 0, fmt.Errorf("read tar: %w", rerr)
		}
		files++
		if strings.HasPrefix(filepath.Base(hdr.Name), "content-") {
			notes++
		}
	}
	return files, notes, nil
}

// decryptSnapshotBytes fully authenticates and decrypts one snapshot file.
func decryptSnapshotBytes(snapPath string, masterKey []byte) (tarBytes []byte, hdr *SnapshotHeader, err error) {
	f, err := os.Open(snapPath)
	if err != nil {
		return nil, nil, fmt.Errorf("open snapshot: %w", err)
	}
	defer f.Close()
	hdr, err = ReadHeader(f)
	if err != nil {
		return nil, nil, fmt.Errorf("read header: %w", err)
	}
	dek, err := unwrapSnapshotDEK(masterKey, hdr)
	if err != nil {
		return nil, nil, fmt.Errorf("unwrap DEK: %w", err)
	}
	defer zero(dek)
	var buf bytes.Buffer
	if err := DecryptStream(&buf, f, dek, hdr.BaseNonce[:], hdr.HeaderBytes); err != nil {
		return nil, nil, fmt.Errorf("decrypt: %w", err)
	}
	return buf.Bytes(), hdr, nil
}

// cmdTestRecovery implements `test-recovery [latest|<id>] [--remote]`.
// It proves the newest backup really opens, without touching live notes.
func cmdTestRecovery(args []string) error {
	requested := "latest"
	useRemote := false
	for _, a := range args {
		if a == "--remote" {
			useRemote = true
		} else if requested == "latest" {
			requested = a
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

	var snapPath, snapID string
	if useRemote {
		cfg, lerr := LoadConfig()
		if lerr != nil {
			return lerr
		}
		if cfg.CloudRemote == "" {
			return fmt.Errorf("--remote needs a configured cloud remote")
		}
		host, herr := remoteHost(cfg, hostname())
		if herr != nil {
			return herr
		}
		fetchDir, terr := os.MkdirTemp(stateDir(), ".drill-fetch-")
		if terr != nil {
			return terr
		}
		defer os.RemoveAll(fetchDir)
		id, ferr := fetchRecoveryKit(cfg, requested, host, fetchDir)
		if ferr != nil {
			saveDrillStatus(requested, ferr)
			return fmt.Errorf("test recovery (remote): %w", ferr)
		}
		snapID = id
		manifest, verr := validateRecoveryKit(fetchDir)
		if verr != nil {
			saveDrillStatus(snapID, verr)
			return fmt.Errorf("test recovery (remote): %w", verr)
		}
		snapPath = filepath.Join(fetchDir, filepath.FromSlash(manifest.SnapshotFile))
	} else {
		ids, lerr := ListSnapshots()
		if lerr != nil || len(ids) == 0 {
			return fmt.Errorf("no snapshots found")
		}
		snapID = ids[len(ids)-1]
		if requested != "latest" {
			if err := validateSnapshotID(requested); err != nil {
				return err
			}
			snapID = requested
		}
		snapPath = SnapshotPath(snapID)
		if head, herr := LoadHead(); herr == nil && head != nil {
			if gen, gerr := snapshotGenerationFromID(snapID); gerr == nil && gen == head.Generation {
				actual, herr2 := hashFile(snapPath)
				if herr2 != nil || !bytes.Equal(actual[:], head.HeadHash) {
					ferr := fmt.Errorf("snapshot integrity check failed: on-disk hash does not match head.json")
					saveDrillStatus(snapID, ferr)
					return fmt.Errorf("test recovery: %w", ferr)
				}
			}
		}
	}

	tarBytes, hdr, derr := decryptSnapshotBytes(snapPath, masterKey)
	if derr != nil {
		saveDrillStatus(snapID, derr)
		return fmt.Errorf("test recovery: %w", derr)
	}
	if len(tarBytes) == 0 {
		ferr := fmt.Errorf("decrypted archive is empty")
		saveDrillStatus(snapID, ferr)
		return fmt.Errorf("test recovery: %w", ferr)
	}
	files, notes, cerr := countTarNotes(tarBytes)
	if cerr != nil {
		saveDrillStatus(snapID, cerr)
		return fmt.Errorf("test recovery: %w", cerr)
	}
	if notes == 0 {
		ferr := fmt.Errorf("decrypted archive contains no notes")
		saveDrillStatus(snapID, ferr)
		return fmt.Errorf("test recovery: %w", ferr)
	}
	saveDrillStatus(snapID, nil)
	mode := "classic"
	if hdr.PQ {
		mode = "hybrid (ML-KEM-1024)"
	}
	log("TEST RECOVERY PASSED snapshot=%s gen=%d crypto=%s files=%d notes=%d", snapID, hdr.Generation, mode, files, notes)
	fmt.Println("Your newest backup decrypts and verifies. Live notes were not touched.")
	return nil
}

// cmdExportKeys implements `export-keys <dest-dir>`: the download-keys
// escrow bundle. It contains keyfile.json and the PQ keys, never a
// plaintext master key or recovery code.
func cmdExportKeys(args []string) error {
	if len(args) != 1 || strings.TrimSpace(args[0]) == "" {
		return fmt.Errorf("usage: export-keys <dest-dir>")
	}
	if !isInitialised() {
		return fmt.Errorf("not initialised")
	}
	dest := args[0]
	if err := os.MkdirAll(dest, 0700); err != nil {
		return fmt.Errorf("create export dir: %w", err)
	}
	entries, err := os.ReadDir(dest)
	if err != nil {
		return err
	}
	if len(entries) != 0 {
		return fmt.Errorf("export destination %s must be empty", dest)
	}
	if err := copyRegularFile(keyfilePath(), filepath.Join(dest, "keyfile.json")); err != nil {
		return err
	}
	hasPQ := pqKeysExist()
	if hasPQ {
		if err := copyRegularFile(pqSecretPath(), filepath.Join(dest, "pq-secret")); err != nil {
			return err
		}
		if err := copyRegularFile(pqPublicPath(), filepath.Join(dest, "pq-public")); err != nil {
			return err
		}
	}
	cfg, _ := LoadConfig()
	readme := "XNote backup escrow export\n" +
		"Keep this folder OFF this machine, separate from your passphrase.\n" +
		"keyfile.json unwraps only with your passphrase or recovery code.\n"
	if hasPQ {
		readme += fmt.Sprintf("PQ fingerprint: %s (ML-KEM-1024). Hybrid snapshots need pq-secret plus the master key.\n", pqFingerprint())
	}
	readme += fmt.Sprintf("Crypto mode at export: %s\n", cfg.CryptoMode) +
		"Restore: install XNote, import the newest recovery kit, then unlock and restore.\n" +
		"This export never contains a plaintext master key or your recovery code.\n"
	if err := os.WriteFile(filepath.Join(dest, "README.txt"), []byte(readme), 0600); err != nil {
		return err
	}
	var sums strings.Builder
	for _, name := range []string{"keyfile.json", "pq-secret", "pq-public", "README.txt"} {
		p := filepath.Join(dest, name)
		if _, err := os.Stat(p); err != nil {
			continue
		}
		h, err := hashFile(p)
		if err != nil {
			return err
		}
		fmt.Fprintf(&sums, "%x  %s\n", h, name)
	}
	if err := os.WriteFile(filepath.Join(dest, "SHA256SUMS"), []byte(sums.String()), 0600); err != nil {
		return err
	}
	log("escrow export written to %s (store it off this machine)", dest)
	return nil
}

// cmdSetCrypto implements `set-crypto <classic|hybrid>`.
func cmdSetCrypto(args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: set-crypto <classic|hybrid>")
	}
	mode := strings.TrimSpace(args[0])
	if mode != CryptoClassic && mode != CryptoHybrid {
		return fmt.Errorf("unknown crypto mode %q (expected classic or hybrid)", mode)
	}
	cfg, err := LoadConfig()
	if err != nil {
		return err
	}
	if mode == CryptoHybrid {
		if err := pqEnsureKeys(); err != nil {
			return err
		}
	}
	cfg.CryptoMode = mode
	if err := SaveConfig(cfg); err != nil {
		return err
	}
	log("crypto mode updated -> %s (existing snapshots stay readable)", mode)
	return nil
}

// cmdSetMirror implements `set-mirror <remote|none>`.
func cmdSetMirror(args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: set-mirror <remote|none>")
	}
	cfg, err := LoadConfig()
	if err != nil {
		return err
	}
	spec := strings.TrimSpace(args[0])
	if spec == "none" || spec == "" {
		cfg.CloudMirror = ""
	} else {
		if _, err := parseRemoteTarget(spec); err != nil {
			return err
		}
		cfg.CloudMirror = spec
	}
	if err := SaveConfig(cfg); err != nil {
		return err
	}
	log("mirror remote updated -> %s", cfg.CloudMirror)
	return nil
}

// cmdRotateKey implements `rotate-key`: true master-key rotation. Every
// snapshot is re-encrypted under a new master key in a staging directory
// and verified before the swap; a new recovery code is issued.
func cmdRotateKey() error {
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
	oldMaster, err := UnwrapMasterKeyFromPassphrase(kf, passphrase)
	if err != nil {
		return err
	}
	defer zero(oldMaster)

	cfg, err := LoadConfig()
	if err != nil {
		return err
	}
	ids, err := ListSnapshots()
	if err != nil {
		return err
	}

	newMaster, err := randBytes(32)
	if err != nil {
		return err
	}
	defer zero(newMaster)
	newRecovery, err := randBytes(32)
	if err != nil {
		return err
	}

	stageDir, err := os.MkdirTemp(stateDir(), ".rotate-new-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stageDir)
	stageSnaps := filepath.Join(stageDir, "snapshots")
	if err := os.MkdirAll(stageSnaps, 0700); err != nil {
		return err
	}

	var parent [32]byte
	var lastGen uint64
	for _, id := range ids {
		tarBytes, oldHdr, derr := decryptSnapshotBytes(SnapshotPath(id), oldMaster)
		if derr != nil {
			return fmt.Errorf("rotate: decrypt %s: %w", id, derr)
		}
		dek, wrapped, kemCT, gerr := generateSnapshotDEK(newMaster, cfg.CryptoMode)
		if gerr != nil {
			return fmt.Errorf("rotate: wrap %s: %w", id, gerr)
		}
		hdr := &SnapshotHeader{Generation: oldHdr.Generation, ParentCipherHash: parent, WrappedDEK: wrapped, PQ: cfg.CryptoMode == CryptoHybrid, KemCT: kemCT}
		nonce, nerr := randBytes(xchacha20NonceLen)
		if nerr != nil {
			return nerr
		}
		copy(hdr.BaseNonce[:], nonce)
		var headerBuf bytes.Buffer
		headerAAD, herr := WriteHeader(&headerBuf, hdr)
		if herr != nil {
			return herr
		}
		var body bytes.Buffer
		if _, eerr := EncryptStreamWithNonce(&body, bytes.NewReader(tarBytes), dek, headerAAD, nonce); eerr != nil {
			return fmt.Errorf("rotate: encrypt %s: %w", id, eerr)
		}
		zero(dek)
		out := filepath.Join(stageSnaps, id+".xsnap")
		tmp := out + ".tmp"
		if err := os.WriteFile(tmp, append(headerBuf.Bytes(), body.Bytes()...), 0600); err != nil {
			return err
		}
		if err := os.Rename(tmp, out); err != nil {
			return err
		}
		// Verify the staged copy opens under the new master before swapping.
		if _, _, verr := decryptSnapshotBytes(out, newMaster); verr != nil {
			return fmt.Errorf("rotate: verify %s: %w", id, verr)
		}
		h, herr := hashFile(out)
		if herr != nil {
			return herr
		}
		parent = h
		lastGen = oldHdr.Generation
	}

	newKF, err := BuildKeyfile(newMaster, passphrase, newRecovery)
	if err != nil {
		return fmt.Errorf("rotate: build keyfile: %w", err)
	}

	backupDir := filepath.Join(stateDir(), "pre-rotate-"+time.Now().UTC().Format("20060102T150405Z"))
	if err := os.MkdirAll(backupDir, 0700); err != nil {
		return err
	}
	for _, item := range []string{"keyfile.json", "head.json", "snapshots"} {
		old := filepath.Join(stateDir(), item)
		if _, serr := os.Lstat(old); serr == nil {
			if rerr := os.Rename(old, filepath.Join(backupDir, item)); rerr != nil {
				return fmt.Errorf("rotate: preserve %s: %w", item, rerr)
			}
		}
	}
	rollback := func() {
		_ = os.Remove(keyfilePath())
		_ = os.Remove(headPath())
		_ = os.RemoveAll(snapshotsDir())
		for _, item := range []string{"keyfile.json", "head.json", "snapshots"} {
			p := filepath.Join(backupDir, item)
			if _, serr := os.Lstat(p); serr == nil {
				_ = os.Rename(p, filepath.Join(stateDir(), item))
			}
		}
	}
	if len(ids) > 0 {
		if err := os.Rename(stageSnaps, snapshotsDir()); err != nil {
			rollback()
			return fmt.Errorf("rotate: install snapshots: %w", err)
		}
		head := &HeadState{Generation: lastGen, HeadHash: parent[:]}
		if err := SaveHead(head); err != nil {
			rollback()
			return fmt.Errorf("rotate: save head: %w", err)
		}
	}
	if err := SaveKeyfile(newKF); err != nil {
		rollback()
		return fmt.Errorf("rotate: save keyfile: %w", err)
	}
	if err := KeyringStore(newMaster); err != nil {
		log("warning: keyring store failed: %v (run unlock)", err)
	}
	code := RecoveryBytesToCode(newRecovery)
	fmt.Printf("\n  ============================================================\n")
	fmt.Printf("  NEW RECOVERY CODE (write this down — shown ONLY ONCE):\n\n  %s\n", code)
	fmt.Printf("  ============================================================\n\n")
	log("master key rotated; %d snapshots re-encrypted; previous key material preserved at %s (delete after a verified backup)", len(ids), backupDir)
	fmt.Println("Run a backup now: old cloud kits use the previous key and must be replaced.")
	return nil
}
