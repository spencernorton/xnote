// keyring.go — libsecret keyring integration via shell-out to secret-tool.
//
// The unwrapped master_key is cached in the libsecret keyring so routine
// backups never need the passphrase. Argon2id only runs at init/unlock.
//
// Keyring integration choice: shell out to `secret-tool` (from the
// libsecret-tools package) rather than DBus directly. Rationale:
//   - secret-tool is the documented portable CLI for libsecret and is
//     available in all Ubuntu releases this app targets.
//   - The DBus Secret Service protocol is complex (object paths, sessions,
//     collection unlock flows); shelling out avoids reimplementing that.
//   - The downside (a subprocess per backup) is negligible — backup runs
//     asynchronously once per note-save event with a 45s debounce.
//
// If secret-tool is absent the binary falls back gracefully: backup still
// works (no-op if uninitialised), but unlock is required before backup.
//
// The keyring is reached through secret-tool over DBus. Talk to DBus directly
// if secret-tool proves unreliable in automated contexts (headless CI,
// systemd --user sessions without a running agent).

package main

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

const (
	secretToolAttrService = "xnote-backup"
	secretToolAttrAccount = "master_key"
)

// KeyringStore stores the raw master_key bytes in the system keyring.
// Encoded as hex for safe transport through secret-tool's string interface.
func KeyringStore(masterKey []byte) error {
	if _, err := exec.LookPath("secret-tool"); err != nil {
		// Degrade gracefully — not fatal
		return fmt.Errorf("secret-tool not found; master_key not cached (run 'unlock' on each session start)")
	}
	// Encode into a zeroed buffer rather than an immutable Go string,
	// so we can wipe it after the subprocess completes.
	// hex.NewEncoder writes into a bytes.Buffer we control; it is zeroed after use.
	var hexBuf bytes.Buffer
	enc := hex.NewEncoder(&hexBuf)
	enc.Write(masterKey) //nolint:errcheck — writes to bytes.Buffer, never errors
	cmd := exec.Command("secret-tool", "store",
		"--label", "XNote backup master key",
		"service", secretToolAttrService,
		"account", secretToolAttrAccount,
	)
	cmd.Stdin = &hexBuf
	defer zero(hexBuf.Bytes())
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("secret-tool store: %w\n%s", err, out)
	}
	return nil
}

// KeyringLoad retrieves the master_key from the system keyring.
// Returns nil, nil if not found (uninitialised or keyring locked).
func KeyringLoad() ([]byte, error) {
	if _, err := exec.LookPath("secret-tool"); err != nil {
		return nil, nil // no keyring available
	}
	cmd := exec.Command("secret-tool", "lookup",
		"service", secretToolAttrService,
		"account", secretToolAttrAccount,
	)
	out, err := cmd.Output()
	if err != nil {
		// Exit 1 means "not found" — not an error
		if exitErr, ok := err.(*exec.ExitError); ok && exitErr.ExitCode() == 1 {
			return nil, nil
		}
		return nil, fmt.Errorf("secret-tool lookup: %w", err)
	}
	hexKey := strings.TrimSpace(string(out))
	if hexKey == "" {
		return nil, nil
	}
	raw, err := hex.DecodeString(hexKey)
	if err != nil {
		return nil, fmt.Errorf("keyring: decode hex: %w", err)
	}
	return raw, nil
}

// KeyringDelete removes the cached master_key from the keyring.
func KeyringDelete() error {
	if _, err := exec.LookPath("secret-tool"); err != nil {
		return nil
	}
	cmd := exec.Command("secret-tool", "clear",
		"service", secretToolAttrService,
		"account", secretToolAttrAccount,
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok && exitErr.ExitCode() == 1 {
			return nil // no matching item; deletion is idempotent
		}
		return fmt.Errorf("secret-tool clear: %w\n%s", err, out)
	}
	return nil
}

// Clear the durable interlock only after the secret service returns this key.
func cacheAndVerifyMaster(master []byte) error {
	if err := syncRecoveryState(true); err != nil {
		return err
	}
	if err := KeyringStore(master); err != nil {
		return err
	}
	cached, err := KeyringLoad()
	defer zero(cached)
	if err != nil {
		return err
	}
	if !bytes.Equal(cached, master) {
		return fmt.Errorf("keyring readback does not match; run unlock")
	}
	if err := os.Remove(unlockRequiredPath()); err != nil && !os.IsNotExist(err) {
		return err
	}
	if err := syncDirectory(stateDir()); err != nil {
		_ = requireVerifiedUnlock("Recovery unlock persistence failed.")
		return err
	}
	return nil
}

func requireVerifiedUnlock(message string) error {
	path := unlockRequiredPath()
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		return err
	}
	_, writeErr := f.WriteString(message + "\n")
	syncErr := f.Sync()
	closeErr := f.Close()
	if writeErr != nil {
		return writeErr
	}
	if syncErr != nil {
		return syncErr
	}
	if closeErr != nil {
		return closeErr
	}
	d, err := os.Open(filepath.Dir(path))
	if err != nil {
		return err
	}
	defer d.Close()
	return d.Sync()
}
