package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

func TestHybridRoundTripNeedsBothSecrets(t *testing.T) {
	_, _ = setupScratchHome(t)
	if err := pqEnsureKeys(); err != nil {
		t.Fatalf("pqEnsureKeys: %v", err)
	}
	master := mustRandBytes(t, 32)
	dek, wrapped, kemCT, err := generateSnapshotDEK(master, CryptoHybrid)
	if err != nil {
		t.Fatalf("generate hybrid: %v", err)
	}
	if len(kemCT) != mlkem1024CiphertextLen {
		t.Fatalf("kem ct len %d", len(kemCT))
	}
	hdr := &SnapshotHeader{PQ: true, WrappedDEK: wrapped, KemCT: kemCT}
	got, err := unwrapSnapshotDEK(master, hdr)
	if err != nil {
		t.Fatalf("unwrap hybrid: %v", err)
	}
	if !bytes.Equal(got, dek) {
		t.Fatal("hybrid DEK mismatch")
	}
	// Wrong master must fail even with the right PQ secret.
	if _, err := unwrapSnapshotDEK(mustRandBytes(t, 32), hdr); err == nil {
		t.Fatal("hybrid unwrap with wrong master must fail")
	}
	// Removing the PQ secret must fail even with the right master.
	secretBak, _ := os.ReadFile(pqSecretPath())
	if err := os.Remove(pqSecretPath()); err != nil {
		t.Fatal(err)
	}
	if _, err := unwrapSnapshotDEK(master, hdr); err == nil {
		t.Fatal("hybrid unwrap without PQ secret must fail")
	}
	if err := os.WriteFile(pqSecretPath(), secretBak, 0600); err != nil {
		t.Fatal(err)
	}
	// A tampered KEM ciphertext must fail (implicit rejection yields a
	// wrong shared secret, so AEAD authentication fails).
	bad := append([]byte{}, kemCT...)
	bad[0] ^= 0xff
	if _, err := unwrapSnapshotDEK(master, &SnapshotHeader{PQ: true, WrappedDEK: wrapped, KemCT: bad}); err == nil {
		t.Fatal("hybrid unwrap with tampered KEM must fail")
	}
}

func TestClassicReadableAndHeaderVersions(t *testing.T) {
	master := mustRandBytes(t, 32)
	dek, wrapped, kemCT, err := generateSnapshotDEK(master, CryptoClassic)
	if err != nil {
		t.Fatalf("generate classic: %v", err)
	}
	if kemCT != nil {
		t.Fatal("classic must not carry a KEM ciphertext")
	}
	hdr := &SnapshotHeader{Generation: 7, WrappedDEK: wrapped}
	var buf bytes.Buffer
	aad, err := WriteHeader(&buf, hdr)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasPrefix(buf.Bytes(), []byte(snapMagic)) || bytes.HasPrefix(buf.Bytes(), []byte(snapMagicV2)) {
		t.Fatal("classic header must use v1 magic")
	}
	back, err := ReadHeader(bytes.NewReader(buf.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	if back.PQ || back.Generation != 7 || !bytes.Equal(back.HeaderBytes, aad) {
		t.Fatal("classic header round-trip mismatch")
	}
	got, err := unwrapSnapshotDEK(master, back)
	if err != nil || !bytes.Equal(got, dek) {
		t.Fatalf("classic unwrap: %v", err)
	}

	// Hybrid header uses v2 magic and round-trips its KEM ciphertext.
	hdr2 := &SnapshotHeader{Generation: 8, WrappedDEK: wrapped, PQ: true, KemCT: make([]byte, mlkem1024CiphertextLen)}
	var buf2 bytes.Buffer
	if _, err := WriteHeader(&buf2, hdr2); err != nil {
		t.Fatal(err)
	}
	if !bytes.HasPrefix(buf2.Bytes(), []byte(snapMagicV2)) {
		t.Fatal("hybrid header must use v2 magic")
	}
	back2, err := ReadHeader(bytes.NewReader(buf2.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	if !back2.PQ || len(back2.KemCT) != mlkem1024CiphertextLen {
		t.Fatal("hybrid header round-trip mismatch")
	}
}

func TestHybridSnapshotFileDecryptsAndTamperFails(t *testing.T) {
	cfgDir, _ := setupScratchHome(t)
	writeTestNotes(t, cfgDir)
	if err := pqEnsureKeys(); err != nil {
		t.Fatalf("pq: %v", err)
	}
	master := mustRandBytes(t, 32)

	var tarBuf bytes.Buffer
	if _, _, err := WriteTarWithNoteCount(&tarBuf, cfgDir); err != nil {
		t.Fatalf("tar: %v", err)
	}
	dek, wrapped, kemCT, err := generateSnapshotDEK(master, CryptoHybrid)
	if err != nil {
		t.Fatalf("wrap: %v", err)
	}
	hdr := &SnapshotHeader{Generation: 42, WrappedDEK: wrapped, PQ: true, KemCT: kemCT}
	nonce := mustRandBytes(t, xchacha20NonceLen)
	copy(hdr.BaseNonce[:], nonce)
	var headerBuf bytes.Buffer
	aad, err := WriteHeader(&headerBuf, hdr)
	if err != nil {
		t.Fatal(err)
	}
	var body bytes.Buffer
	if _, err := EncryptStreamWithNonce(&body, bytes.NewReader(tarBuf.Bytes()), dek, aad, nonce); err != nil {
		t.Fatalf("encrypt: %v", err)
	}
	if err := os.MkdirAll(snapshotsDir(), 0700); err != nil {
		t.Fatal(err)
	}
	id := SnapshotID(42)
	// SnapshotID embeds the current timestamp; pin generation by parsing back.
	gen, err := snapshotGenerationFromID(id)
	if err != nil || gen != 42 {
		// SnapshotID always uses the passed generation, so this must hold.
		t.Fatalf("id gen: %v %d", err, gen)
	}
	snapPath := SnapshotPath(id)
	full := append(headerBuf.Bytes(), body.Bytes()...)
	if err := os.WriteFile(snapPath, full, 0600); err != nil {
		t.Fatal(err)
	}
	plain, backHdr, err := decryptSnapshotBytes(snapPath, master)
	if err != nil {
		t.Fatalf("decrypt hybrid snapshot: %v", err)
	}
	if !backHdr.PQ {
		t.Fatal("expected hybrid header")
	}
	_, notes, err := countTarNotes(plain)
	if err != nil || notes == 0 {
		t.Fatalf("notes: %v %d", err, notes)
	}
	if !bytes.Equal(plain, tarBuf.Bytes()) {
		t.Fatal("hybrid snapshot plaintext mismatch")
	}
	// Flip one ciphertext byte: decryption must fail with no output.
	tampered := append([]byte{}, full...)
	tampered[len(tampered)-1] ^= 0xff
	if err := os.WriteFile(snapPath, tampered, 0600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := decryptSnapshotBytes(snapPath, master); err == nil {
		t.Fatal("tampered hybrid snapshot must fail")
	}
}

func TestConfigCryptoAndMirror(t *testing.T) {
	_, _ = setupScratchHome(t)
	cfg := Config{KeepSnapshots: 60, CloudRemote: "/tmp/primary-xnote", CloudMirror: "/tmp/mirror-xnote", CloudTimeoutSec: 120, CryptoMode: CryptoHybrid}
	if err := SaveConfig(cfg); err != nil {
		t.Fatalf("SaveConfig: %v", err)
	}
	got, err := LoadConfig()
	if err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}
	if got.CryptoMode != CryptoHybrid || got.CloudMirror != "/tmp/mirror-xnote" {
		t.Fatalf("config round-trip: %+v", got)
	}
	if len(got.allRemotes()) != 2 {
		t.Fatal("allRemotes must return primary plus mirror")
	}
	bad := got
	bad.CloudMirror = bad.CloudRemote
	if err := SaveConfig(bad); err == nil {
		t.Fatal("identical mirror must be rejected")
	}
	// Existing configs without the new keys default to classic, local mirror.
	p := filepath.Join(configDir(), "cloud-backup.conf")
	if err := os.WriteFile(p, []byte("CLOUD_REMOTE=\"/tmp/only\"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	def, err := LoadConfig()
	if err != nil {
		t.Fatal(err)
	}
	if def.CryptoMode != CryptoClassic || def.CloudMirror != "" {
		t.Fatalf("defaults: %+v", def)
	}
}
