package main

import (
	"bytes"
	"crypto/mlkem"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
)

func escrowReceiptPath() string { return filepath.Join(stateDir(), "pq-escrow-fingerprint") }
func pqEscrowRequired() bool {
	receipt, err := os.ReadFile(escrowReceiptPath())
	return err != nil || string(receipt) != pqFingerprint() || len(receipt) == 0
}

// Import the separately held escrow before unlocking a recovered kit. Never
// replace active key material with a different key from an old export.
func cmdImportKeys(args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: import-keys <escrow-directory>")
	}
	src := args[0]
	if err := requireDirectoryNoSymlink(src); err != nil {
		return err
	}
	kpath := filepath.Join(src, "keyfile.json")
	if err := requireRegularFileMax(kpath, maxRecoveryKeyfileSize); err != nil {
		return err
	}
	rawKF, err := os.ReadFile(kpath)
	if err != nil {
		return err
	}
	kf, err := parseEscrowKeyfile(rawKF)
	if err != nil {
		return err
	}
	var seed, public []byte
	seedPath := filepath.Join(src, "pq-secret")
	if _, err := os.Lstat(seedPath); err == nil {
		if err := requireRegularFileMax(seedPath, mlkem1024SeedLen); err != nil {
			return err
		}
		seed, err = os.ReadFile(seedPath)
		if err != nil {
			return err
		}
		defer zero(seed)
		dk, err := mlkem.NewDecapsulationKey1024(seed)
		if err != nil {
			return fmt.Errorf("invalid escrow PQ secret: %w", err)
		}
		pubPath := filepath.Join(src, "pq-public")
		if err := requireRegularFileMax(pubPath, mlkem1024PublicLen); err != nil {
			return err
		}
		public, err = os.ReadFile(pubPath)
		if err != nil {
			return err
		}
		if !bytes.Equal(public, dk.EncapsulationKey().Bytes()) {
			return fmt.Errorf("escrow PQ public key does not match its secret")
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	// Validate every conflict before writing any active file.
	for name, data := range map[string][]byte{"pq-secret": seed, "pq-public": public} {
		if data == nil {
			continue
		}
		path := filepath.Join(stateDir(), name)
		existing, err := os.ReadFile(path)
		if err == nil && !bytes.Equal(existing, data) {
			return fmt.Errorf("refusing to replace existing %s with a different key", name)
		}
		if err != nil && !os.IsNotExist(err) {
			return err
		}
	}
	if err := os.MkdirAll(stateDir(), 0700); err != nil {
		return err
	}
	if !isInitialised() {
		if err := requireVerifiedUnlock("Escrow import requires unlock before backup."); err != nil {
			return err
		}
		if err := SaveKeyfile(kf); err != nil {
			return err
		}
	}
	for name, data := range map[string][]byte{"pq-secret": seed, "pq-public": public} {
		if data == nil {
			continue
		}
		path := filepath.Join(stateDir(), name)
		if _, err := os.Lstat(path); os.IsNotExist(err) {
			f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
			if err != nil {
				return err
			}
			_, err = f.Write(data)
			closeErr := f.Close()
			if err != nil {
				return err
			}
			if closeErr != nil {
				return closeErr
			}
		}
	}
	if len(seed) > 0 {
		if err := os.WriteFile(escrowReceiptPath(), []byte(pqFingerprint()), 0600); err != nil {
			return err
		}
	}
	log("escrow keys imported; unlock the recovered kit, then test-recovery before restoring")
	return nil
}

func parseEscrowKeyfile(raw []byte) (*Keyfile, error) {
	var kf Keyfile
	if err := json.Unmarshal(raw, &kf); err != nil {
		return nil, err
	}
	if kf.Version != KeyfileVersion {
		return nil, fmt.Errorf("unsupported escrow keyfile version")
	}
	if err := ValidateKeyfile(&kf); err != nil {
		return nil, err
	}
	return &kf, nil
}
