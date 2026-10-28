package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestMirrorFailureRetriesSameKitUntilBothDestinationsVerify(t *testing.T) {
	f := prepareRecoveryFixture(t)
	isolatedKeyring(t, f.masterKey)
	mirror := filepath.Join(t.TempDir(), "mirror")
	if err := os.WriteFile(mirror, []byte("blocked destination"), 0600); err != nil {
		t.Fatal(err)
	}
	cfg := defaultConfig
	cfg.CloudRemote = f.remoteRoot
	cfg.CloudMirror = mirror
	cfg.CloudDevice = f.device
	if err := SaveConfig(cfg); err != nil {
		t.Fatal(err)
	}
	if err := cmdSync(); err == nil {
		t.Fatal("mirror failure reported success")
	}
	ids, _ := ListSnapshots()
	id := ids[len(ids)-1]
	st, err := loadRemoteStatus()
	if err != nil {
		t.Fatal(err)
	}
	if st.LastError == "" || remoteReceiptMatches(cfg, st, id, f.device) {
		t.Fatal("partial delivery recorded complete receipt")
	}
	if _, err := validateRecoveryKit(filepath.Join(f.remoteRoot, f.device, "kits", id)); err != nil {
		t.Fatal("primary did not receive safe kit", err)
	}
	if err := os.Remove(mirror); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(mirror, 0700); err != nil {
		t.Fatal(err)
	}
	if err := cmdSync(); err != nil {
		t.Fatal("retry failed", err)
	}
	after, _ := ListSnapshots()
	if len(after) != len(ids) || after[len(after)-1] != id {
		t.Fatal("retry created duplicate snapshot")
	}
	if _, err := validateRecoveryKit(filepath.Join(mirror, f.device, "kits", id)); err != nil {
		t.Fatal("mirror retry kit invalid", err)
	}
	st, err = loadRemoteStatus()
	if err != nil {
		t.Fatal(err)
	}
	if st.LastError != "" || !remoteReceiptMatches(cfg, st, id, f.device) {
		t.Fatal("both destinations not recorded after retry")
	}
}
