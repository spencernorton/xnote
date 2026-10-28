package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseRemoteTarget(t *testing.T) {
	t.Setenv("HOME", "/tmp/xnote-test-home")
	valid := []struct {
		value  string
		isPath bool
	}{
		{"/mnt/backup/xnote", true},
		{"~/backups/xnote", true},
		{"./scratch", true},
		{"dropbox:xnote-backup", false},
		{"gdrive:", false},
	}
	for _, tc := range valid {
		target, err := parseRemoteTarget(tc.value)
		if err != nil {
			t.Errorf("parseRemoteTarget(%q): %v", tc.value, err)
			continue
		}
		if target.isPath != tc.isPath {
			t.Errorf("parseRemoteTarget(%q).isPath=%v, want %v", tc.value, target.isPath, tc.isPath)
		}
	}
	for _, value := range []string{"", "dropbxo", "/", "bad name:x", "dropbox:a/../b"} {
		if _, err := parseRemoteTarget(value); err == nil {
			t.Errorf("parseRemoteTarget(%q) unexpectedly succeeded", value)
		}
	}
}

func TestPathDestinationSymlinkIntoStateIsRejected(t *testing.T) {
	setupScratchHome(t)
	link := filepath.Join(t.TempDir(), "not-really-off-box")
	if err := os.Symlink(stateDir(), link); err != nil {
		t.Fatal(err)
	}
	dest := filepath.Join(link, "testhost", "kits", "20260723T120000Z-g00000000000000000001")
	if err := validatePathDestination(dest); err == nil {
		t.Fatal("symlinked destination inside local backup state was accepted")
	}
}

func fastTestKeyfile(t *testing.T, masterKey, passphrase, recoveryBytes []byte) *Keyfile {
	t.Helper()
	const (
		testTime    = uint32(1)
		testMemory  = uint32(8 * 1024)
		testThreads = uint8(1)
		testKeyLen  = uint32(32)
	)
	buildSlot := func(secret []byte) (ArgonParams, []byte) {
		salt := mustRandBytes(t, argon2SaltLen)
		params := ArgonParams{
			Time: testTime, Memory: testMemory, Threads: testThreads,
			KeyLen: testKeyLen, Salt: salt,
		}
		kek := deriveKEKWithParams(secret, salt, testTime, testMemory, testThreads, testKeyLen)
		defer zero(kek)
		wrapped, err := wrapKey(kek, masterKey)
		if err != nil {
			t.Fatal(err)
		}
		return params, wrapped
	}
	passParams, passWrapped := buildSlot(passphrase)
	recoveryParams, recoveryWrapped := buildSlot(recoveryBytes)
	return &Keyfile{
		Version:             KeyfileVersion,
		PassphraseParams:    passParams,
		WrappedMasterKey:    passWrapped,
		RecoveryParams:      recoveryParams,
		WrappedMasterKeyRec: recoveryWrapped,
	}
}

type recoveryFixture struct {
	remoteRoot    string
	device        string
	snapshotID    string
	passphrase    []byte
	recoveryBytes []byte
	masterKey     []byte
	originalFiles map[string][]byte
}

func prepareRecoveryFixture(t *testing.T) recoveryFixture {
	t.Helper()
	cfgDir, _ := setupScratchHome(t)
	writeTestNotes(t, cfgDir)
	original := readDirFiles(t, cfgDir)
	passphrase := []byte("test recovery passphrase")
	recoveryBytes := mustRandBytes(t, 32)
	masterKey := mustRandBytes(t, 32)
	if err := SaveKeyfile(fastTestKeyfile(t, masterKey, passphrase, recoveryBytes)); err != nil {
		t.Fatal(err)
	}
	if err := doBackupWithKey(masterKey); err != nil {
		t.Fatal(err)
	}
	ids, err := ListSnapshots()
	if err != nil || len(ids) != 1 {
		t.Fatalf("expected one snapshot, got %v (err=%v)", ids, err)
	}
	return recoveryFixture{
		remoteRoot:    t.TempDir(),
		device:        "testhost",
		snapshotID:    ids[0],
		passphrase:    passphrase,
		recoveryBytes: recoveryBytes,
		masterKey:     masterKey,
		originalFiles: original,
	}
}

func pushFixture(t *testing.T, fixture recoveryFixture) Config {
	t.Helper()
	kitDir, err := buildRecoveryKit(fixture.snapshotID, fixture.device)
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(kitDir)
	cfg := Config{
		CloudRemote:     fixture.remoteRoot,
		CloudTimeoutSec: 30,
	}
	if err := pushRecoveryKit(cfg, kitDir, fixture.device); err != nil {
		t.Fatal(err)
	}
	return cfg
}

func TestRecoveryKitPathPushIsCompleteAppendOnlyAndImmutable(t *testing.T) {
	fixture := prepareRecoveryFixture(t)
	cfg := pushFixture(t, fixture)

	remoteKit := filepath.Join(
		fixture.remoteRoot, fixture.device, "kits", fixture.snapshotID,
	)
	if _, err := validateRecoveryKit(remoteKit); err != nil {
		t.Fatalf("remote kit invalid: %v", err)
	}
	for _, rel := range []string{
		recoveryManifestName,
		"keyfile.json",
		"head.json",
		recoverySnapshotRel(fixture.snapshotID),
	} {
		if _, err := os.Stat(filepath.Join(remoteKit, rel)); err != nil {
			t.Errorf("missing remote recovery-kit file %s: %v", rel, err)
		}
	}

	// Extra remote history is never deleted by another push.
	sentinel := filepath.Join(remoteKit, "provider-history-marker")
	if err := os.WriteFile(sentinel, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	kitDir, err := buildRecoveryKit(fixture.snapshotID, fixture.device)
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(kitDir)
	if err := pushRecoveryKit(cfg, kitDir, fixture.device); err != nil {
		t.Fatalf("idempotent push: %v", err)
	}
	if data, err := os.ReadFile(sentinel); err != nil || string(data) != "keep" {
		t.Fatalf("append-only push deleted/changed remote history: data=%q err=%v", data, err)
	}

	// Existing differing ciphertext is never overwritten, and the mismatch
	// makes the push fail instead of publishing a false success.
	remoteSnapshot := filepath.Join(remoteKit, recoverySnapshotRel(fixture.snapshotID))
	if err := os.WriteFile(remoteSnapshot, []byte("tampered"), 0600); err != nil {
		t.Fatal(err)
	}
	err = pushRecoveryKit(cfg, kitDir, fixture.device)
	if err == nil || !strings.Contains(err.Error(), "verification") {
		t.Fatalf("tampered immutable remote should fail verification, got %v", err)
	}
	if data, _ := os.ReadFile(remoteSnapshot); string(data) != "tampered" {
		t.Fatal("immutable push overwrote an existing remote object")
	}
}

func TestManifestIsCompletionMarker(t *testing.T) {
	root := t.TempDir()
	cfg := Config{CloudRemote: root, CloudTimeoutSec: 30}
	device := "testhost"
	id := "20260723T120000Z-g00000000000000000001"
	partial := filepath.Join(root, device, "kits", id)
	if err := os.MkdirAll(partial, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(partial, "head.json"), []byte("{}"), 0600); err != nil {
		t.Fatal(err)
	}
	ids, err := listRemoteKits(cfg, device)
	if err != nil {
		t.Fatal(err)
	}
	if len(ids) != 0 {
		t.Fatalf("partial kit without manifest was listed: %v", ids)
	}
}

func TestManifestGenerationMustMatchSnapshotIDSuffix(t *testing.T) {
	fixture := prepareRecoveryFixture(t)
	kitDir, err := buildRecoveryKit(fixture.snapshotID, fixture.device)
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(kitDir)
	path := filepath.Join(kitDir, recoveryManifestName)
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var manifest RecoveryManifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		t.Fatal(err)
	}
	manifest.Generation++
	data, err = json.Marshal(manifest)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := validateRecoveryKit(kitDir); err == nil ||
		!strings.Contains(err.Error(), "does not match snapshot ID") {
		t.Fatalf("manifest/ID generation mismatch accepted: %v", err)
	}
}

func TestRcloneRecoveryKitRoundTrip(t *testing.T) {
	if _, err := exec.LookPath("rclone"); err != nil {
		t.Skip("rclone not installed")
	}
	fixture := prepareRecoveryFixture(t)
	rcloneRoot := t.TempDir()
	rcloneConfig := filepath.Join(t.TempDir(), "rclone.conf")
	configText := "[localtest]\ntype = alias\nremote = " + rcloneRoot + "\n"
	if err := os.WriteFile(rcloneConfig, []byte(configText), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("RCLONE_CONFIG", rcloneConfig)

	kitDir, err := buildRecoveryKit(fixture.snapshotID, fixture.device)
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(kitDir)
	cfg := Config{CloudRemote: "localtest:xnote", CloudTimeoutSec: 30}
	if err := pushRecoveryKit(cfg, kitDir, fixture.device); err != nil {
		t.Fatalf("rclone push: %v", err)
	}
	ids, err := listRemoteKits(cfg, fixture.device)
	if err != nil {
		t.Fatalf("rclone list: %v", err)
	}
	if len(ids) != 1 || ids[0] != fixture.snapshotID {
		t.Fatalf("rclone list=%v, want %s", ids, fixture.snapshotID)
	}
	fetchDir := t.TempDir()
	id, err := fetchRecoveryKit(cfg, "latest", fixture.device, fetchDir)
	if err != nil {
		t.Fatalf("rclone fetch: %v", err)
	}
	if id != fixture.snapshotID {
		t.Fatalf("rclone fetched %s, want %s", id, fixture.snapshotID)
	}
	if _, err := validateRecoveryKit(fetchDir); err != nil {
		t.Fatalf("rclone-fetched recovery kit invalid: %v", err)
	}
}

func TestFetchIgnoresUnexpectedRemoteObjects(t *testing.T) {
	fixture := prepareRecoveryFixture(t)
	cfg := pushFixture(t, fixture)
	remoteKit := filepath.Join(fixture.remoteRoot, fixture.device, "kits", fixture.snapshotID)
	extra := filepath.Join(remoteKit, "attacker-controlled-extra.bin")
	f, err := os.Create(extra)
	if err != nil {
		t.Fatal(err)
	}
	if err := f.Truncate(2 * maxRecoverySnapshotSize); err != nil {
		f.Close()
		t.Fatal(err)
	}
	f.Close()

	fetchDir := t.TempDir()
	if _, err := fetchRecoveryKit(cfg, "latest", fixture.device, fetchDir); err != nil {
		t.Fatalf("selective fetch failed because of unrelated object: %v", err)
	}
	if _, err := os.Lstat(filepath.Join(fetchDir, filepath.Base(extra))); !os.IsNotExist(err) {
		t.Fatalf("unexpected remote object was downloaded, err=%v", err)
	}
}

func TestFetchRejectsOversizedExpectedObjectBeforeCopy(t *testing.T) {
	fixture := prepareRecoveryFixture(t)
	cfg := pushFixture(t, fixture)
	remoteSnapshot := filepath.Join(
		fixture.remoteRoot, fixture.device, "kits", fixture.snapshotID,
		recoverySnapshotRel(fixture.snapshotID),
	)
	if err := os.Truncate(remoteSnapshot, maxRecoverySnapshotSize+1); err != nil {
		t.Fatal(err)
	}
	fetchDir := t.TempDir()
	_, err := fetchRecoveryKit(cfg, fixture.snapshotID, fixture.device, fetchDir)
	if err == nil || !strings.Contains(err.Error(), "size limit") {
		t.Fatalf("oversized expected object should be rejected, got %v", err)
	}
	if _, statErr := os.Lstat(filepath.Join(fetchDir, recoverySnapshotRel(fixture.snapshotID))); !os.IsNotExist(statErr) {
		t.Fatalf("oversized snapshot was copied, err=%v", statErr)
	}
}

func TestRecoveryKitRejectsSymlinkedSnapshotsDirectory(t *testing.T) {
	fixture := prepareRecoveryFixture(t)
	cfg := pushFixture(t, fixture)
	remoteKit := filepath.Join(fixture.remoteRoot, fixture.device, "kits", fixture.snapshotID)
	realSnapshots := filepath.Join(t.TempDir(), "snapshots")
	if err := os.Rename(filepath.Join(remoteKit, "snapshots"), realSnapshots); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(realSnapshots, filepath.Join(remoteKit, "snapshots")); err != nil {
		t.Fatal(err)
	}
	if _, err := validateRecoveryKit(remoteKit); err == nil {
		t.Fatal("kit with symlinked snapshots/ directory was accepted")
	}
	fetchDir := t.TempDir()
	if _, err := fetchRecoveryKit(cfg, fixture.snapshotID, fixture.device, fetchDir); err == nil {
		t.Fatal("fetch followed a symlinked remote snapshots/ directory")
	}
}

func TestScratchDiskLossRecoveryViaPassphraseAndRecoveryCode(t *testing.T) {
	for _, method := range []string{"passphrase", "recovery-code"} {
		t.Run(method, func(t *testing.T) {
			fixture := prepareRecoveryFixture(t)
			cfg := pushFixture(t, fixture)

			// Simulate total local backup-state and note loss. The remote root is
			// independent of these scratch XDG directories.
			if err := os.RemoveAll(stateDir()); err != nil {
				t.Fatal(err)
			}
			if err := os.RemoveAll(configDir()); err != nil {
				t.Fatal(err)
			}
			if err := os.MkdirAll(stateDir(), 0700); err != nil {
				t.Fatal(err)
			}
			fetchDir, err := os.MkdirTemp(stateDir(), ".test-fetch-")
			if err != nil {
				t.Fatal(err)
			}
			defer os.RemoveAll(fetchDir)
			id, err := fetchRecoveryKit(cfg, "latest", fixture.device, fetchDir)
			if err != nil {
				t.Fatalf("fetch recovery kit: %v", err)
			}
			if id != fixture.snapshotID {
				t.Fatalf("fetched %s, want %s", id, fixture.snapshotID)
			}
			if _, err := installRecoveryKit(fetchDir, false); err != nil {
				t.Fatalf("install recovery kit: %v", err)
			}

			keyfile, err := LoadKeyfile()
			if err != nil {
				t.Fatal(err)
			}
			var recoveredMasterKey []byte
			if method == "passphrase" {
				recoveredMasterKey, err = UnwrapMasterKeyFromPassphrase(keyfile, fixture.passphrase)
			} else {
				recoveredMasterKey, err = UnwrapMasterKeyFromRecovery(keyfile, fixture.recoveryBytes)
			}
			if err != nil {
				t.Fatalf("unlock via %s: %v", method, err)
			}
			if !bytes.Equal(recoveredMasterKey, fixture.masterKey) {
				t.Fatal("recovered master key mismatch")
			}
			if err := doRestoreWithKey(recoveredMasterKey, "latest", true); err != nil {
				t.Fatalf("restore after remote import: %v", err)
			}
			got := readDirFiles(t, configDir())
			if len(got) != len(fixture.originalFiles) {
				t.Fatalf("restored file count=%d, want %d", len(got), len(fixture.originalFiles))
			}
			for name, want := range fixture.originalFiles {
				if !bytes.Equal(got[name], want) {
					t.Errorf("restored file %s differs", name)
				}
			}
		})
	}
}

func TestImportRefusesExistingStateUnlessForcedAndPreservesIt(t *testing.T) {
	fixture := prepareRecoveryFixture(t)
	cfg := pushFixture(t, fixture)
	fetchDir := t.TempDir()
	if _, err := fetchRecoveryKit(cfg, "latest", fixture.device, fetchDir); err != nil {
		t.Fatal(err)
	}
	if _, err := installRecoveryKit(fetchDir, false); err == nil {
		t.Fatal("import should refuse existing local recovery state without --force")
	}
	preservedAt, err := installRecoveryKit(fetchDir, true)
	if err != nil {
		t.Fatalf("forced import: %v", err)
	}
	if preservedAt == "" {
		t.Fatal("forced import did not report preserved prior state")
	}
	for _, rel := range []string{
		"keyfile.json",
		"head.json",
		recoverySnapshotRel(fixture.snapshotID),
	} {
		if _, err := os.Stat(filepath.Join(preservedAt, rel)); err != nil {
			t.Errorf("forced import did not preserve %s: %v", rel, err)
		}
	}
	if _, err := validateRecoveryKit(fetchDir); err != nil {
		t.Fatalf("source kit changed during import: %v", err)
	}
	if _, err := os.Stat(SnapshotPath(fixture.snapshotID)); err != nil {
		t.Fatalf("imported snapshot missing: %v", err)
	}
	if _, err := os.Stat(unlockRequiredPath()); err != nil {
		t.Fatalf("import did not block backup pending explicit unlock: %v", err)
	}
	if err := cmdBackup(); err == nil || !strings.Contains(err.Error(), "must be unlocked") {
		t.Fatalf("backup should reject stale keyring after import, got %v", err)
	}
}

func TestHostileArgonParamsRejectedBeforeDerivation(t *testing.T) {
	base := fastTestKeyfile(t, mustRandBytes(t, 32), []byte("passphrase"), mustRandBytes(t, 32))
	mutations := []struct {
		name string
		fn   func(*ArgonParams)
	}{
		{"zero-time", func(p *ArgonParams) { p.Time = 0 }},
		{"huge-time", func(p *ArgonParams) { p.Time = maxImportedArgonTime + 1 }},
		{"tiny-memory", func(p *ArgonParams) { p.Memory = 1 }},
		{"huge-memory", func(p *ArgonParams) { p.Memory = ^uint32(0) }},
		{"zero-threads", func(p *ArgonParams) { p.Threads = 0 }},
		{"huge-threads", func(p *ArgonParams) { p.Threads = maxImportedArgonThreads + 1 }},
		{"huge-keylen", func(p *ArgonParams) { p.KeyLen = ^uint32(0) }},
		{"missing-salt", func(p *ArgonParams) { p.Salt = nil }},
	}
	clone := func() *Keyfile {
		data, err := json.Marshal(base)
		if err != nil {
			t.Fatal(err)
		}
		var keyfile Keyfile
		if err := json.Unmarshal(data, &keyfile); err != nil {
			t.Fatal(err)
		}
		return &keyfile
	}
	callWithoutPanic := func(tb testing.TB, name string, fn func() error) (err error) {
		tb.Helper()
		defer func() {
			if recovered := recover(); recovered != nil {
				tb.Fatalf("%s panicked: %v", name, recovered)
			}
		}()
		return fn()
	}
	for _, mutation := range mutations {
		t.Run(mutation.name+"/passphrase", func(t *testing.T) {
			keyfile := clone()
			mutation.fn(&keyfile.PassphraseParams)
			if err := ValidateKeyfile(keyfile); err == nil {
				t.Fatal("hostile passphrase params validated")
			}
			err := callWithoutPanic(t, mutation.name, func() error {
				_, err := UnwrapMasterKeyFromPassphrase(keyfile, []byte("passphrase"))
				return err
			})
			if err == nil {
				t.Fatal("hostile passphrase params reached Argon2")
			}
		})
		t.Run(mutation.name+"/recovery", func(t *testing.T) {
			keyfile := clone()
			mutation.fn(&keyfile.RecoveryParams)
			if err := ValidateKeyfile(keyfile); err == nil {
				t.Fatal("hostile recovery params validated")
			}
			err := callWithoutPanic(t, mutation.name, func() error {
				_, err := UnwrapMasterKeyFromRecovery(keyfile, mustRandBytes(t, 32))
				return err
			})
			if err == nil {
				t.Fatal("hostile recovery params reached Argon2")
			}
		})
	}

	setupScratchHome(t)
	hostile := clone()
	hostile.PassphraseParams.Memory = ^uint32(0)
	data, err := json.Marshal(hostile)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(stateDir(), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyfilePath(), data, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadKeyfile(); err == nil {
		t.Fatal("LoadKeyfile accepted hostile Argon2 parameters")
	}
}

func TestRemoteStatusKeepsSuccessSnapshotSeparateFromLaterFailure(t *testing.T) {
	setupScratchHome(t)
	successID := "20260723T120000Z-g00000000000000000001"
	failedID := "20260723T130000Z-g00000000000000000002"
	if err := os.MkdirAll(snapshotsDir(), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(SnapshotPath(successID), []byte("synthetic ciphertext"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := recordRemoteAttempt(successID, "testhost", nil); err != nil {
		t.Fatal(err)
	}
	if err := recordRemoteAttempt(failedID, "testhost", errors.New("provider offline")); err != nil {
		t.Fatal(err)
	}
	status, err := loadRemoteStatus()
	if err != nil {
		t.Fatal(err)
	}
	if status.LastSuccessSnapshotID != successID || status.LastAttemptSnapshotID != failedID {
		t.Fatalf("status conflated success and attempt IDs: %+v", status)
	}
	if status.LastError == "" {
		t.Fatal("later remote failure was not recorded")
	}
}

func TestKeyringDeleteIsIdempotentWhenNoItemExists(t *testing.T) {
	binDir := t.TempDir()
	secretTool := filepath.Join(binDir, "secret-tool")
	if err := os.WriteFile(secretTool, []byte("#!/bin/sh\nexit 1\n"), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", binDir)
	if err := KeyringDelete(); err != nil {
		t.Fatalf("absent keyring item should be a successful no-op: %v", err)
	}
}

func TestBackupSourceGuardRejectsMissingEmptyAndConfigOnly(t *testing.T) {
	cases := []struct {
		name  string
		setup func(string) error
	}{
		{"missing", func(dir string) error { return os.RemoveAll(dir) }},
		{"empty", func(dir string) error { return nil }},
		{"config-only", func(dir string) error {
			return os.WriteFile(filepath.Join(dir, "cloud-backup.conf"), []byte("CLOUD_REMOTE=\n"), 0600)
		}},
		{"symlink-content", func(dir string) error {
			target := filepath.Join(t.TempDir(), "content-real")
			if err := os.WriteFile(target, []byte("note"), 0600); err != nil {
				return err
			}
			return os.Symlink(target, filepath.Join(dir, "content-link"))
		}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			cfgDir, _ := setupScratchHome(t)
			if err := SaveKeyfile(fastTestKeyfile(t, mustRandBytes(t, 32), []byte("pw"), mustRandBytes(t, 32))); err != nil {
				t.Fatal(err)
			}
			if err := tc.setup(cfgDir); err != nil {
				t.Fatal(err)
			}
			err := cmdBackup()
			if err == nil || !strings.Contains(err.Error(), "content-") && !strings.Contains(err.Error(), "config source") {
				t.Fatalf("unsafe source should fail before keyring lookup, got %v", err)
			}
		})
	}
}

func TestSnapshotOrderingUsesGenerationNotClock(t *testing.T) {
	setupScratchHome(t)
	if err := os.MkdirAll(snapshotsDir(), 0700); err != nil {
		t.Fatal(err)
	}
	gen1 := "20260724T020000Z-g00000000000000000001"
	gen2ClockBack := "20260723T010000Z-g00000000000000000002"
	for _, id := range []string{gen2ClockBack, gen1} {
		if err := os.WriteFile(SnapshotPath(id), []byte("fixture"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	ids, err := ListSnapshots()
	if err != nil {
		t.Fatal(err)
	}
	if len(ids) != 2 || ids[0] != gen1 || ids[1] != gen2ClockBack {
		t.Fatalf("generation ordering failed across clock rollback: %v", ids)
	}

	remoteRoot := t.TempDir()
	device := "testhost"
	for _, id := range []string{gen2ClockBack, gen1} {
		dir := filepath.Join(remoteRoot, device, "kits", id)
		if err := os.MkdirAll(dir, 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, recoveryManifestName), []byte("{}"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	remoteIDs, err := listRemoteKits(Config{CloudRemote: remoteRoot, CloudTimeoutSec: 30}, device)
	if err != nil {
		t.Fatal(err)
	}
	if len(remoteIDs) != 2 || remoteIDs[0] != gen1 || remoteIDs[1] != gen2ClockBack {
		t.Fatalf("remote generation ordering failed across clock rollback: %v", remoteIDs)
	}
}

func TestBackupUninitialisedIsFailure(t *testing.T) {
	setupScratchHome(t)
	err := cmdBackup()
	if err == nil || !strings.Contains(err.Error(), "not initialised") {
		t.Fatalf("uninitialised backup should fail honestly, got %v", err)
	}
}

func TestStateLockRejectsOverlap(t *testing.T) {
	setupScratchHome(t)
	lock, err := acquireStateLock()
	if err != nil {
		t.Fatal(err)
	}
	defer lock.Close()
	if _, err := acquireStateLock(); err == nil {
		t.Fatal("second overlapping backup lock unexpectedly succeeded")
	}
}

func TestUnlockCommandRejectsOverlap(t *testing.T) {
	setupScratchHome(t)
	lock, err := acquireStateLock()
	if err != nil {
		t.Fatal(err)
	}
	defer lock.Close()

	err = runUnlock(nil)
	if err == nil || !strings.Contains(err.Error(), "another XNote backup/recovery operation") {
		t.Fatalf("unlock should share the backup state lock, got %v", err)
	}
}

func TestE2EENoPlaintextInRemoteKit(t *testing.T) {
	cfgDir, _ := setupScratchHome(t)
	secrets := map[string]string{
		"content-TOPSECRET1": "CONFIDENTIAL_PROJECT_OMEGA\nAccount: 9988-7766-5544\nPasscode: correct-horse-staple",
		"content-TOPSECRET2": "PRIVATE_MEDICAL_NOTE_918273\nDiagnosis: classified",
		"info-TOPSECRET1":    "width 300\nheight 200\nback rgb(255,255,0)\ncontent content-TOPSECRET1\n",
		"info-TOPSECRET2":    "width 250\nheight 150\nback rgb(0,255,255)\ncontent content-TOPSECRET2\n",
		"default-style":      "fontname Monospace 14\n",
	}
	for name, body := range secrets {
		if err := os.WriteFile(filepath.Join(cfgDir, name), []byte(body), 0600); err != nil {
			t.Fatal(err)
		}
	}

	masterKey := mustRandBytes(t, 32)
	passphrase := []byte("super-secret-passphrase-never-sent-to-cloud")
	recoveryBytes := mustRandBytes(t, 32)
	if err := SaveKeyfile(fastTestKeyfile(t, masterKey, passphrase, recoveryBytes)); err != nil {
		t.Fatal(err)
	}
	if err := doBackupWithKey(masterKey); err != nil {
		t.Fatal(err)
	}
	ids, err := ListSnapshots()
	if err != nil || len(ids) != 1 {
		t.Fatalf("expected 1 snapshot, got %v (%v)", ids, err)
	}

	remoteRoot := t.TempDir()
	device := "testhost"
	kitDir, err := buildRecoveryKit(ids[0], device)
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(kitDir)

	cfg := Config{CloudRemote: remoteRoot, CloudTimeoutSec: 30}
	if err := pushRecoveryKit(cfg, kitDir, device); err != nil {
		t.Fatal(err)
	}

	forbiddenStrings := []string{
		"CONFIDENTIAL_PROJECT_OMEGA",
		"9988-7766-5544",
		"correct-horse-staple",
		"PRIVATE_MEDICAL_NOTE_918273",
		"content-TOPSECRET1",
		"content-TOPSECRET2",
		"info-TOPSECRET1",
		"info-TOPSECRET2",
		"default-style",
		"Monospace 14",
		string(passphrase),
	}

	err = filepath.WalkDir(remoteRoot, func(path string, d os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		for _, secret := range forbiddenStrings {
			if strings.Contains(path, secret) {
				t.Fatalf("remote path %q leaked plaintext identifier %q", path, secret)
			}
		}
		if d.IsDir() {
			return nil
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		for _, secret := range forbiddenStrings {
			if bytes.Contains(data, []byte(secret)) {
				t.Fatalf("remote file %s leaked plaintext %q", path, secret)
			}
		}
		if bytes.Contains(data, masterKey) {
			t.Fatalf("remote file %s leaked raw master_key", path)
		}
		if bytes.Contains(data, recoveryBytes) {
			t.Fatalf("remote file %s leaked raw recovery key bytes", path)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}

	// Verify that if a plaintext note file were ever placed inside kitDir,
	// pushRecoveryKit refuses to upload it.
	leakedNote := filepath.Join(kitDir, "content-accidental")
	if err := os.WriteFile(leakedNote, []byte("plaintext"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := pushRecoveryKit(cfg, kitDir, device); err == nil {
		t.Fatal("pushRecoveryKit accepted a kit containing a plaintext content-* file")
	}
}

func TestRcloneCryptDropboxAndGoogleDriveRemoteVerification(t *testing.T) {
	if _, err := exec.LookPath("rclone"); err != nil {
		t.Skip("rclone not installed")
	}
	fixture := prepareRecoveryFixture(t)

	dropboxSimRoot := t.TempDir()
	gdriveSimRoot := t.TempDir()
	cryptBackingRoot := t.TempDir()

	obscuredOut, err := exec.Command("rclone", "obscure", "test-crypt-remote-password").CombinedOutput()
	if err != nil {
		t.Fatalf("rclone obscure: %v (%s)", err, obscuredOut)
	}
	obscured := strings.TrimSpace(string(obscuredOut))

	rcloneConfig := filepath.Join(t.TempDir(), "rclone.conf")
	configText := strings.Join([]string{
		"[dropbox]",
		"type = alias",
		"remote = " + dropboxSimRoot,
		"",
		"[gdrive]",
		"type = alias",
		"remote = " + gdriveSimRoot,
		"",
		"[gdrive-vault]",
		"type = crypt",
		"remote = " + cryptBackingRoot,
		"password = " + obscured,
		"",
	}, "\n")
	if err := os.WriteFile(rcloneConfig, []byte(configText), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("RCLONE_CONFIG", rcloneConfig)

	for _, remoteSpec := range []string{
		"dropbox:xnote-backup",
		"gdrive:xnote-backup",
		"gdrive-vault:Backup/Apps/XNote",
	} {
		t.Run(remoteSpec, func(t *testing.T) {
			kitDir, err := buildRecoveryKit(fixture.snapshotID, fixture.device)
			if err != nil {
				t.Fatal(err)
			}
			defer os.RemoveAll(kitDir)

			cfg := Config{CloudRemote: remoteSpec, CloudTimeoutSec: 30}
			if err := pushRecoveryKit(cfg, kitDir, fixture.device); err != nil {
				t.Fatalf("pushRecoveryKit(%s): %v", remoteSpec, err)
			}
			ids, err := listRemoteKits(cfg, fixture.device)
			if err != nil {
				t.Fatalf("listRemoteKits(%s): %v", remoteSpec, err)
			}
			if len(ids) != 1 || ids[0] != fixture.snapshotID {
				t.Fatalf("listRemoteKits(%s)=%v, want [%s]", remoteSpec, ids, fixture.snapshotID)
			}
			fetchDir := t.TempDir()
			id, err := fetchRecoveryKit(cfg, "latest", fixture.device, fetchDir)
			if err != nil {
				t.Fatalf("fetchRecoveryKit(%s): %v", remoteSpec, err)
			}
			if id != fixture.snapshotID {
				t.Fatalf("fetched %s, want %s", id, fixture.snapshotID)
			}
		})
	}

	// Specifically verify that on a crypt remote (where rclone check --checksum
	// degrades to size-only), same-size remote tampering is caught by cryptcheck.
	var encFiles []string
	_ = filepath.WalkDir(cryptBackingRoot, func(path string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			encFiles = append(encFiles, path)
		}
		return nil
	})
	if len(encFiles) == 0 {
		t.Fatal("expected encrypted files in crypt backing root")
	}
	for _, ef := range encFiles {
		data, err := os.ReadFile(ef)
		if err != nil || len(data) == 0 {
			continue
		}
		data[len(data)-1] ^= 0xFF
		if err := os.WriteFile(ef, data, 0600); err != nil {
			t.Fatal(err)
		}
	}

	kitDir, err := buildRecoveryKit(fixture.snapshotID, fixture.device)
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(kitDir)
	cryptCfg := Config{CloudRemote: "gdrive-vault:Backup/Apps/XNote", CloudTimeoutSec: 30}
	if err := pushRecoveryKit(cryptCfg, kitDir, fixture.device); err == nil {
		t.Fatal("same-size tampered crypt remote unexpectedly passed verification")
	}
}

func TestBuildRecoveryKitSweepsOrphanedStagingDirs(t *testing.T) {
	fixture := prepareRecoveryFixture(t)
	stageRoot := filepath.Join(stateDir(), "remote-staging")
	orphan := filepath.Join(stageRoot, ".20260101T000000Z-g00000000000000000001-orphan")
	if err := os.MkdirAll(orphan, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(orphan, "head.json"), []byte("{}"), 0600); err != nil {
		t.Fatal(err)
	}

	kitDir, err := buildRecoveryKit(fixture.snapshotID, fixture.device)
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(kitDir)

	if _, err := os.Stat(orphan); !os.IsNotExist(err) {
		t.Fatalf("orphaned staging directory was not swept: err=%v", err)
	}
}

func TestSyncSkipsUnchangedNotesAndPushesWhenChanged(t *testing.T) {
	cfgDir, _ := setupScratchHome(t)
	writeTestNotes(t, cfgDir)

	masterKey := mustRandBytes(t, 32)
	if err := SaveKeyfile(fastTestKeyfile(t, masterKey, []byte("pw"), mustRandBytes(t, 32))); err != nil {
		t.Fatal(err)
	}
	binDir := t.TempDir()
	stubSecretTool := filepath.Join(binDir, "secret-tool")
	script := "#!/bin/sh\nif [ \"$1\" = \"lookup\" ]; then\n  printf '%s\\n' \"" + hexEncode(masterKey) + "\"\n  exit 0\nfi\nexit 0\n"
	if err := os.WriteFile(stubSecretTool, []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", binDir+string(os.PathListSeparator)+os.Getenv("PATH"))

	remoteRoot := t.TempDir()
	confContent := "CLOUD_REMOTE=" + remoteRoot + "\nCLOUD_DEVICE_NAME=testhost\n"
	if err := os.WriteFile(filepath.Join(cfgDir, "cloud-backup.conf"), []byte(confContent), 0600); err != nil {
		t.Fatal(err)
	}

	// First sync creates gen 1 and pushes to remote.
	if err := cmdSync(); err != nil {
		t.Fatalf("first sync: %v", err)
	}
	head1, err := LoadHead()
	if err != nil || head1.Generation != 1 {
		t.Fatalf("expected generation 1 after first sync, got %+v (%v)", head1, err)
	}

	// Second sync with identical notes must NOT mint generation 2.
	if err := cmdSync(); err != nil {
		t.Fatalf("second sync (unchanged): %v", err)
	}
	head2, err := LoadHead()
	if err != nil || head2.Generation != 1 {
		t.Fatalf("unchanged sync minted new generation: got %d, want 1", head2.Generation)
	}

	// Modifying a note must mint generation 2 on the next sync.
	if err := os.WriteFile(filepath.Join(cfgDir, "content-abc123"), []byte("Updated note text!"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := cmdSync(); err != nil {
		t.Fatalf("third sync (changed): %v", err)
	}
	head3, err := LoadHead()
	if err != nil || head3.Generation != 2 {
		t.Fatalf("changed sync did not advance generation: got %d, want 2", head3.Generation)
	}
}
