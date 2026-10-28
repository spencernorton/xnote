package main

import (
	"bytes"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func isolatedKeyring(t *testing.T, master []byte) {
	t.Helper()
	root := t.TempDir()
	k := filepath.Join(root, "cached-key")
	if err := os.WriteFile(k, []byte(hex.EncodeToString(master)), 0600); err != nil {
		t.Fatal(err)
	}
	script := `#!/bin/sh
case "$1" in
 lookup) cat "$XNOTE_FIXTURE_KEYRING" ;;
 store) if [ "$XNOTE_FIXTURE_STORE_FAIL" = 1 ]; then exit 1; fi
        if [ "$XNOTE_FIXTURE_STALE_READ" = 1 ]; then cat >/dev/null; else cat > "$XNOTE_FIXTURE_KEYRING"; fi ;;
 clear) : > "$XNOTE_FIXTURE_KEYRING" ;;
esac
`
	if err := os.WriteFile(filepath.Join(root, "secret-tool"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("XNOTE_FIXTURE_KEYRING", k)
	t.Setenv("PATH", root+":"+os.Getenv("PATH"))
}
func TestCacheVerificationKeepsRotationInterlock(t *testing.T) {
	setupScratchHome(t)
	old := mustRandBytes(t, 32)
	fresh := mustRandBytes(t, 32)
	isolatedKeyring(t, old)
	if err := SaveKeyfile(fastTestKeyfile(t, fresh, []byte("test"), mustRandBytes(t, 32))); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(stateDir(), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(unlockRequiredPath(), []byte("blocked"), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("XNOTE_FIXTURE_STALE_READ", "1")
	if err := cacheAndVerifyMaster(fresh); err == nil {
		t.Fatal("stale readback accepted")
	}
	if _, err := os.Stat(unlockRequiredPath()); err != nil {
		t.Fatal("interlock removed on stale cache", err)
	}
	t.Setenv("XNOTE_FIXTURE_STALE_READ", "")
	if err := cacheAndVerifyMaster(fresh); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(unlockRequiredPath()); !os.IsNotExist(err) {
		t.Fatal("verified unlock did not clear interlock")
	}
}
func TestRotationFailureBlocksBackupsAndPublishesFreshKit(t *testing.T) {
	f := prepareRecoveryFixture(t)
	isolatedKeyring(t, f.masterKey)
	cfg := defaultConfig
	cfg.CloudRemote = t.TempDir()
	cfg.CloudDevice = f.device
	if err := SaveConfig(cfg); err != nil {
		t.Fatal(err)
	}
	if err := cmdSync(); err != nil {
		t.Fatal(err)
	}
	oldIDs, _ := ListSnapshots()
	oldID := oldIDs[len(oldIDs)-1]
	t.Setenv("XNOTE_FIXTURE_STORE_FAIL", "1")
	withStdinFile(t, string(f.passphrase)+"\n")
	if err := cmdRotateKey(); err != nil {
		t.Fatal(err)
	}
	if err := cmdBackup(); err == nil || !strings.Contains(err.Error(), "unlocked") {
		t.Fatalf("rotation cache failure failed open: %v", err)
	}
	t.Setenv("XNOTE_FIXTURE_STORE_FAIL", "")
	withStdinFile(t, string(f.passphrase)+"\n")
	if err := cmdUnlock(nil); err != nil {
		t.Fatal(err)
	}
	if err := cmdSync(); err != nil {
		t.Fatal(err)
	}
	ids, _ := ListSnapshots()
	newID := ids[len(ids)-1]
	if newID == oldID {
		t.Fatal("rotation reused remote snapshot identity")
	}
	kit := filepath.Join(cfg.CloudRemote, f.device, "kits", newID)
	if _, err := validateRecoveryKit(kit); err != nil {
		t.Fatal("new remote kit invalid", err)
	}
	kf, err := LoadKeyfile()
	if err != nil {
		t.Fatal(err)
	}
	mk, err := UnwrapMasterKeyFromPassphrase(kf, f.passphrase)
	if err != nil {
		t.Fatal(err)
	}
	plain, _, err := decryptSnapshotBytes(filepath.Join(kit, recoverySnapshotRel(newID)), mk)
	if err != nil {
		t.Fatal("new kit cannot decrypt with new key", err)
	}
	if !bytes.Contains(plain, f.originalFiles["content-abc123"]) {
		t.Fatal("new kit lost notes")
	}
	receipt, err := loadRemoteStatus()
	if err != nil {
		t.Fatal(err)
	}
	if !remoteReceiptMatches(cfg, receipt, newID, f.device) {
		t.Fatal("new receipt invalid")
	}
	changed := cfg
	changed.CloudMirror = t.TempDir()
	if remoteReceiptMatches(changed, receipt, newID, f.device) {
		t.Fatal("receipt accepted for a new mirror")
	}
}
func TestHybridEscrowRestoresInEmptyState(t *testing.T) {
	f := prepareRecoveryFixture(t)
	isolatedKeyring(t, f.masterKey)
	if err := pqEnsureKeys(); err != nil {
		t.Fatal(err)
	}
	export := filepath.Join(t.TempDir(), "escrow")
	if err := cmdExportKeys([]string{export}); err != nil {
		t.Fatal(err)
	}
	cfg := defaultConfig
	cfg.CryptoMode = CryptoHybrid
	cfg.CloudRemote = f.remoteRoot
	cfg.CloudDevice = f.device
	if err := SaveConfig(cfg); err != nil {
		t.Fatal(err)
	}
	if err := cmdBackup(); err != nil {
		t.Fatal(err)
	}
	target := t.TempDir()
	t.Setenv("XDG_DATA_HOME", filepath.Join(target, "data"))
	t.Setenv("XDG_CONFIG_HOME", filepath.Join(target, "config"))
	if err := SaveConfig(cfg); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(os.Getenv("XNOTE_FIXTURE_KEYRING"), []byte(hexEncode(mustRandBytes(t, 32))), 0600); err != nil {
		t.Fatal(err)
	}
	if err := cmdImport([]string{"latest"}); err != nil {
		t.Fatal(err)
	}
	if mk, err := KeyringLoad(); err != nil || mk != nil {
		t.Fatalf("replacement keyring was not cleared: %v", err)
	}
	if err := cmdTestRecovery(nil); err == nil {
		t.Fatal("recovery unexpectedly succeeded before keys and unlock")
	}
	if err := cmdImportKeys([]string{export}); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(unlockRequiredPath()); err != nil {
		t.Fatal("import failed to block backup before unlock")
	}
	withStdinFile(t, RecoveryBytesToCode(f.recoveryBytes)+"\n")
	if err := cmdUnlock([]string{"--recovery"}); err != nil {
		t.Fatal(err)
	}
	if err := cmdTestRecovery(nil); err != nil {
		t.Fatal(err)
	}
	if err := cmdRestore([]string{"latest"}); err != nil {
		t.Fatal(err)
	}
	for name, want := range f.originalFiles {
		got, err := os.ReadFile(filepath.Join(configDir(), name))
		if err != nil || !bytes.Equal(got, want) {
			t.Fatalf("restored %s differs: %v", name, err)
		}
	}
}

func TestIncompleteStateCannotClearInterlock(t *testing.T) {
	f := prepareRecoveryFixture(t)
	isolatedKeyring(t, f.masterKey)
	if err := requireVerifiedUnlock("transaction incomplete"); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(headPath()); err != nil {
		t.Fatal(err)
	}
	if err := cacheAndVerifyMaster(f.masterKey); err == nil {
		t.Fatal("incomplete snapshot set unlocked")
	}
	if _, err := os.Stat(unlockRequiredPath()); err != nil {
		t.Fatal("interlock lost on persistence/consistency failure")
	}
}
