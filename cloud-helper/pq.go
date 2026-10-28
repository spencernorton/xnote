// pq.go — ML-KEM-1024 post-quantum envelope for hybrid snapshots.
//
// In hybrid mode a new snapshot's DEK is wrapped under a key derived from
// BOTH the classic archive wrap key (HKDF of the master key) and an ML-KEM
// shared secret, so opening it needs the master key and the PQ secret.
// Classic snapshots carry no envelope and stay readable in either mode,
// exactly like Timecrate classic kits under hybrid.

package main

import (
	"bytes"
	"crypto/mlkem"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/crypto/hkdf"
)

const (
	mlkem1024CiphertextLen = 1568 // FIPS 203 ML-KEM-1024 ciphertext size
	mlkem1024SeedLen       = 64
	mlkem1024PublicLen     = 1568
	hkdfInfoHybridWrap     = "xnote archive wrap hybrid v1"
)

func pqSecretPath() string { return filepath.Join(stateDir(), "pq-secret") }
func pqPublicPath() string { return filepath.Join(stateDir(), "pq-public") }

func pqKeysExist() bool {
	_, err1 := os.Stat(pqSecretPath())
	_, err2 := os.Stat(pqPublicPath())
	return err1 == nil && err2 == nil
}

// pqKeyPresent distinguishes absence from inaccessible or unsafe key files.
func pqKeyPresent(path string) (bool, error) {
	info, err := os.Lstat(path)
	if os.IsNotExist(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if !info.Mode().IsRegular() {
		return false, fmt.Errorf("PQ key %s is not a regular file", path)
	}
	return true, nil
}

// pqWriteNewKey never truncates existing key material, including on repair.
// A failed public write leaves the secret available for a later repair.
func pqWriteNewKey(path string, data []byte) error {
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	_, writeErr := f.Write(data)
	syncErr := f.Sync()
	closeErr := f.Close()
	if writeErr != nil {
		return writeErr
	}
	if syncErr != nil {
		return syncErr
	}
	return closeErr
}

// pqEnsureKeys repairs only a missing public key. A surviving secret is never
// replaced; losing it requires recovery from escrow, not a new identity.
// Callers hold the exclusive state lock.
func pqEnsureKeys() error {
	secret, err := pqKeyPresent(pqSecretPath())
	if err != nil {
		return err
	}
	public, err := pqKeyPresent(pqPublicPath())
	if err != nil {
		return err
	}
	if secret {
		dk, err := pqLoadSecret()
		if err != nil {
			return err
		}
		if public {
			_, err := pqLoadPublic() // Also proves that the existing pair matches.
			return err
		}
		if err := pqWriteNewKey(pqPublicPath(), dk.EncapsulationKey().Bytes()); err != nil {
			return fmt.Errorf("repair PQ public: %w", err)
		}
		return nil
	}
	if public {
		return fmt.Errorf("PQ secret missing; restore it from escrow (existing public key preserved)")
	}
	// Both keys are absent. Do not silently initialize over hybrid history.
	entries, err := os.ReadDir(snapshotsDir())
	if err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("inspect snapshots before PQ initialization: %w", err)
	}
	for _, entry := range entries {
		if !strings.HasSuffix(entry.Name(), ".xsnap") {
			continue
		}
		path := filepath.Join(snapshotsDir(), entry.Name())
		if err := requireRegularFile(path); err != nil {
			return err
		}
		f, err := os.Open(path)
		if err != nil {
			return err
		}
		hdr, headerErr := ReadHeader(f)
		closeErr := f.Close()
		if headerErr != nil {
			return fmt.Errorf("inspect snapshot %s before PQ initialization: %w", entry.Name(), headerErr)
		}
		if closeErr != nil {
			return closeErr
		}
		if hdr.PQ {
			return fmt.Errorf("PQ keys missing but hybrid snapshot %s exists; restore keys from escrow", entry.Name())
		}
	}
	if err := os.MkdirAll(stateDir(), 0700); err != nil {
		return fmt.Errorf("create state dir: %w", err)
	}
	dk, err := mlkem.GenerateKey1024()
	if err != nil {
		return fmt.Errorf("generate ML-KEM-1024 key: %w", err)
	}
	seed := dk.Bytes()
	defer zero(seed)
	if err := pqWriteNewKey(pqSecretPath(), seed); err != nil {
		return fmt.Errorf("write PQ secret: %w", err)
	}
	if err := pqWriteNewKey(pqPublicPath(), dk.EncapsulationKey().Bytes()); err != nil {
		return fmt.Errorf("write PQ public: %w", err)
	}
	log("PQ key generated (ML-KEM-1024, fingerprint %s)", pqFingerprint())
	return nil
}

func pqLoadSecret() (*mlkem.DecapsulationKey1024, error) {
	if _, err := pqKeyPresent(pqSecretPath()); err != nil {
		return nil, err
	}
	seed, err := os.ReadFile(pqSecretPath())
	if err != nil {
		return nil, fmt.Errorf("PQ secret missing (hybrid snapshots need it; restore it from your escrow export): %w", err)
	}
	defer zero(seed)
	if len(seed) != mlkem1024SeedLen {
		return nil, fmt.Errorf("PQ secret has wrong length %d", len(seed))
	}
	dk, err := mlkem.NewDecapsulationKey1024(seed)
	if err != nil {
		return nil, fmt.Errorf("load PQ secret: %w", err)
	}
	return dk, nil
}

func pqLoadPublic() (*mlkem.EncapsulationKey1024, error) {
	if _, err := pqKeyPresent(pqPublicPath()); err != nil {
		return nil, err
	}
	raw, err := os.ReadFile(pqPublicPath())
	if err != nil {
		return nil, fmt.Errorf("PQ public key missing; run 'set-crypto hybrid' first: %w", err)
	}
	if len(raw) != mlkem1024PublicLen {
		return nil, fmt.Errorf("PQ public key has wrong length %d", len(raw))
	}
	ek, err := mlkem.NewEncapsulationKey1024(raw)
	if err != nil {
		return nil, fmt.Errorf("load PQ public: %w", err)
	}
	dk, err := pqLoadSecret()
	if err != nil {
		return nil, err
	}
	if !bytes.Equal(raw, dk.EncapsulationKey().Bytes()) {
		return nil, fmt.Errorf("PQ public key does not match the preserved secret; refusing hybrid encryption")
	}
	return ek, nil
}

// pqFingerprint returns the short SHA-256 fingerprint of the PQ public key.
func pqFingerprint() string {
	raw, err := os.ReadFile(pqPublicPath())
	if err != nil || len(raw) == 0 {
		return ""
	}
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(sum[:])[:16]
}

func hybridWrapKey(archiveWrapKey, sharedSecret []byte) ([]byte, error) {
	ikm := append(append([]byte{}, archiveWrapKey...), sharedSecret...)
	h := hkdf.New(sha256.New, ikm, nil, []byte(hkdfInfoHybridWrap))
	key := make([]byte, 32)
	if _, err := io.ReadFull(h, key); err != nil {
		return nil, fmt.Errorf("hkdf hybrid: %w", err)
	}
	return key, nil
}

// generateSnapshotDEK creates a DEK wrapped for the given crypto mode.
// In hybrid mode it also returns the ML-KEM ciphertext for the header.
func generateSnapshotDEK(masterKey []byte, mode string) (dek, wrappedDEK, kemCT []byte, err error) {
	dek, err = randBytes(32)
	if err != nil {
		return nil, nil, nil, err
	}
	archiveWrapKey, err := deriveArchiveWrapKey(masterKey)
	if err != nil {
		return nil, nil, nil, err
	}
	defer zero(archiveWrapKey)
	if mode != CryptoHybrid {
		wrappedDEK, err = wrapKey(archiveWrapKey, dek)
		return dek, wrappedDEK, nil, err
	}
	ek, err := pqLoadPublic()
	if err != nil {
		return nil, nil, nil, err
	}
	shared, ct := ek.Encapsulate()
	defer zero(shared)
	combined, err := hybridWrapKey(archiveWrapKey, shared)
	if err != nil {
		return nil, nil, nil, err
	}
	defer zero(combined)
	wrappedDEK, err = wrapKey(combined, dek)
	if err != nil {
		return nil, nil, nil, err
	}
	return dek, wrappedDEK, ct, nil
}

// unwrapSnapshotDEK unwraps a snapshot DEK, requiring the PQ secret for
// hybrid (v2) snapshots and only the master key for classic ones.
func unwrapSnapshotDEK(masterKey []byte, hdr *SnapshotHeader) ([]byte, error) {
	if hdr == nil {
		return nil, fmt.Errorf("nil snapshot header")
	}
	if !hdr.PQ {
		return unwrapDEK(masterKey, hdr.WrappedDEK)
	}
	if len(hdr.KemCT) != mlkem1024CiphertextLen {
		return nil, fmt.Errorf("hybrid snapshot KEM ciphertext has wrong length %d", len(hdr.KemCT))
	}
	dk, err := pqLoadSecret()
	if err != nil {
		return nil, err
	}
	shared, err := dk.Decapsulate(hdr.KemCT)
	if err != nil {
		return nil, fmt.Errorf("ML-KEM decapsulate (wrong PQ secret?): %w", err)
	}
	defer zero(shared)
	archiveWrapKey, err := deriveArchiveWrapKey(masterKey)
	if err != nil {
		return nil, err
	}
	defer zero(archiveWrapKey)
	combined, err := hybridWrapKey(archiveWrapKey, shared)
	if err != nil {
		return nil, err
	}
	defer zero(combined)
	return unwrapKey(combined, hdr.WrappedDEK)
}
