package main

import (
	"bytes"
	"crypto/mlkem"
	"os"
	"path/filepath"
	"testing"
)

func pqRepairHome(t *testing.T) {
	t.Helper()
	root := t.TempDir()
	t.Setenv("HOME", root)
	t.Setenv("XDG_DATA_HOME", filepath.Join(root, "data"))
	t.Setenv("XDG_CONFIG_HOME", filepath.Join(root, "config"))
}

func pqRepairRead(t *testing.T, path string) []byte {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func pqRepairWrite(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
}

func pqRepairAbsent(t *testing.T, path string) {
	t.Helper()
	if _, err := os.Lstat(path); !os.IsNotExist(err) {
		t.Fatalf("expected absent %s, got %v", path, err)
	}
}

func pqRepairSnapshot(t *testing.T, hybrid bool) (string, []byte, []byte) {
	t.Helper()
	master := bytes.Repeat([]byte{37}, 32)
	mode := CryptoClassic
	if hybrid {
		mode = CryptoHybrid
	}
	dek, wrapped, kemCT, err := generateSnapshotDEK(master, mode)
	if err != nil {
		t.Fatal(err)
	}
	defer zero(dek)
	hdr := &SnapshotHeader{Generation: 1, PQ: hybrid, WrappedDEK: wrapped, KemCT: kemCT}
	nonce, err := randBytes(xchacha20NonceLen)
	if err != nil {
		t.Fatal(err)
	}
	copy(hdr.BaseNonce[:], nonce)
	var out bytes.Buffer
	aad, err := WriteHeader(&out, hdr)
	if err != nil {
		t.Fatal(err)
	}
	plain := []byte("synthetic old snapshot payload")
	if _, err := EncryptStreamWithNonce(&out, bytes.NewReader(plain), dek, aad, nonce); err != nil {
		t.Fatal(err)
	}
	path := SnapshotPath(SnapshotID(1))
	pqRepairWrite(t, path, out.Bytes())
	return path, master, plain
}

func TestPQRepairMissingPublicPreservesOldSnapshot(t *testing.T) {
	pqRepairHome(t)
	if err := pqEnsureKeys(); err != nil {
		t.Fatal(err)
	}
	secret := pqRepairRead(t, pqSecretPath())
	public := pqRepairRead(t, pqPublicPath())
	path, master, plain := pqRepairSnapshot(t, true)
	ciphertext := pqRepairRead(t, path)
	if err := os.Remove(pqPublicPath()); err != nil {
		t.Fatal(err)
	}
	if err := pqEnsureKeys(); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(secret, pqRepairRead(t, pqSecretPath())) {
		t.Fatal("repair replaced secret")
	}
	if !bytes.Equal(public, pqRepairRead(t, pqPublicPath())) {
		t.Fatal("repair changed public identity")
	}
	if !bytes.Equal(ciphertext, pqRepairRead(t, path)) {
		t.Fatal("repair changed old snapshot")
	}
	got, _, err := decryptSnapshotBytes(path, master)
	if err != nil || !bytes.Equal(got, plain) {
		t.Fatalf("old snapshot no longer decrypts: %v", err)
	}
}

func TestPQRepairMismatchedPairFailsClosed(t *testing.T) {
	pqRepairHome(t)
	if err := pqEnsureKeys(); err != nil {
		t.Fatal(err)
	}
	secret := pqRepairRead(t, pqSecretPath())
	other, err := mlkem.GenerateKey1024()
	if err != nil {
		t.Fatal(err)
	}
	public := other.EncapsulationKey().Bytes()
	pqRepairWrite(t, pqPublicPath(), public)
	if err := pqEnsureKeys(); err == nil {
		t.Fatal("mismatched pair accepted")
	}
	if _, _, _, err := generateSnapshotDEK(bytes.Repeat([]byte{9}, 32), CryptoHybrid); err == nil {
		t.Fatal("hybrid encryption accepted mismatched pair without ensure")
	}
	if !bytes.Equal(secret, pqRepairRead(t, pqSecretPath())) || !bytes.Equal(public, pqRepairRead(t, pqPublicPath())) {
		t.Fatal("mismatch rejection overwrote key material")
	}
}

func TestPQRepairMissingSecretDoesNotOverwrite(t *testing.T) {
	pqRepairHome(t)
	if err := pqEnsureKeys(); err != nil {
		t.Fatal(err)
	}
	public := pqRepairRead(t, pqPublicPath())
	if err := os.Remove(pqSecretPath()); err != nil {
		t.Fatal(err)
	}
	if err := pqEnsureKeys(); err == nil {
		t.Fatal("missing secret silently regenerated")
	}
	if _, _, _, err := generateSnapshotDEK(bytes.Repeat([]byte{9}, 32), CryptoHybrid); err == nil {
		t.Fatal("encrypted without secret")
	}
	pqRepairAbsent(t, pqSecretPath())
	if !bytes.Equal(public, pqRepairRead(t, pqPublicPath())) {
		t.Fatal("public overwritten")
	}
}

func TestPQRepairInitializationIdempotent(t *testing.T) {
	pqRepairHome(t)
	if err := pqEnsureKeys(); err != nil {
		t.Fatal(err)
	}
	secret := pqRepairRead(t, pqSecretPath())
	public := pqRepairRead(t, pqPublicPath())
	for i := 0; i < 2; i++ {
		if err := pqEnsureKeys(); err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(secret, pqRepairRead(t, pqSecretPath())) || !bytes.Equal(public, pqRepairRead(t, pqPublicPath())) {
			t.Fatal("idempotent initialization changed keys")
		}
	}
	for _, path := range []string{pqSecretPath(), pqPublicPath()} {
		info, err := os.Stat(path)
		if err != nil {
			t.Fatal(err)
		}
		if info.Mode().Perm() != 0600 {
			t.Fatalf("key permissions: %v", info.Mode())
		}
	}
}

func TestPQRepairLostBothKeysRejectsHybridHistory(t *testing.T) {
	pqRepairHome(t)
	if err := pqEnsureKeys(); err != nil {
		t.Fatal(err)
	}
	path, _, _ := pqRepairSnapshot(t, true)
	ciphertext := pqRepairRead(t, path)
	for _, key := range []string{pqSecretPath(), pqPublicPath()} {
		if err := os.Remove(key); err != nil {
			t.Fatal(err)
		}
	}
	if err := pqEnsureKeys(); err == nil {
		t.Fatal("initialized new identity over lost hybrid keys")
	}
	pqRepairAbsent(t, pqSecretPath())
	pqRepairAbsent(t, pqPublicPath())
	if !bytes.Equal(ciphertext, pqRepairRead(t, path)) {
		t.Fatal("changed hybrid history")
	}
}

func TestPQRepairInitializationAllowsClassicHistory(t *testing.T) {
	pqRepairHome(t)
	path, master, plain := pqRepairSnapshot(t, false)
	if err := pqEnsureKeys(); err != nil {
		t.Fatal(err)
	}
	got, _, err := decryptSnapshotBytes(path, master)
	if err != nil || !bytes.Equal(got, plain) {
		t.Fatalf("classic history affected: %v", err)
	}
}

func TestPQRepairUnreadableHistoryFailsClosed(t *testing.T) {
	pqRepairHome(t)
	pqRepairWrite(t, SnapshotPath(SnapshotID(1)), []byte("truncated"))
	if err := pqEnsureKeys(); err == nil {
		t.Fatal("initialized despite uninspectable history")
	}
	pqRepairAbsent(t, pqSecretPath())
	pqRepairAbsent(t, pqPublicPath())
}

func TestPQRepairInvalidSecretPreserved(t *testing.T) {
	pqRepairHome(t)
	bad := []byte("invalid seed")
	pqRepairWrite(t, pqSecretPath(), bad)
	if err := pqEnsureKeys(); err == nil {
		t.Fatal("invalid secret accepted")
	}
	if !bytes.Equal(bad, pqRepairRead(t, pqSecretPath())) {
		t.Fatal("invalid secret overwritten")
	}
	pqRepairAbsent(t, pqPublicPath())
}
