// keyfile.go — keyfile.json serialisation for xnote-cloud-backup.
//
// keyfile.json lives at ~/.local/share/xnote-backup/keyfile.json.
// It contains everything needed to:
//   1. Re-derive KEK from passphrase (argon2 params + salt).
//   2. Unwrap master_key (wrapped under KEK).
//   3. Unwrap master_key from recovery code (independent slot, same params).
//
// Format is intentionally simple JSON for audibility; fields are stable
// and versioned so params can be upgraded independently.

package main

import (
	"encoding/base32"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
)

// KeyfileVersion identifies the on-disk format.
const KeyfileVersion = 1

const (
	// Imported keyfiles are untrusted until validated. Keep these bounds tight
	// enough that Argon2 cannot be coerced into a panic or an unbounded
	// allocation before AEAD authentication gets a chance to fail.
	minImportedArgonMemory  = 8 * 1024 // 8 MiB in KiB (also used by fast tests)
	maxImportedArgonMemory  = argon2Memory
	maxImportedArgonTime    = 10
	maxImportedArgonThreads = 16
	wrappedMasterKeyLen     = xchacha20NonceLen + 32 + 16 // nonce + key + Poly1305 tag
)

// ArgonParams stores the Argon2id parameters alongside each wrapped slot.
// Stored in keyfile.json so they can be upgraded without re-init.
type ArgonParams struct {
	Time    uint32 `json:"time"`
	Memory  uint32 `json:"memory_kib"`
	Threads uint8  `json:"threads"`
	KeyLen  uint32 `json:"key_len"`
	Salt    []byte `json:"salt"` // 16 random bytes, base64 in JSON via MarshalJSON
}

// Keyfile is the on-disk structure.
type Keyfile struct {
	Version int `json:"version"`

	// Passphrase slot
	PassphraseParams ArgonParams `json:"passphrase_params"`
	WrappedMasterKey []byte      `json:"wrapped_master_key"` // nonce||ciphertext, base64

	// Recovery slot (independent)
	RecoveryParams      ArgonParams `json:"recovery_params"`
	WrappedMasterKeyRec []byte      `json:"wrapped_master_key_recovery"` // nonce||ciphertext, base64
}

// keyfilePath returns the path to keyfile.json.
func keyfilePath() string {
	return filepath.Join(stateDir(), "keyfile.json")
}

// LoadKeyfile reads and parses keyfile.json.
func LoadKeyfile() (*Keyfile, error) {
	info, err := os.Lstat(keyfilePath())
	if os.IsNotExist(err) {
		return nil, fmt.Errorf("not initialised: run 'xnote-cloud-backup init' first")
	}
	if err != nil {
		return nil, fmt.Errorf("stat keyfile: %w", err)
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("keyfile is not a regular file")
	}
	if info.Size() > maxRecoveryKeyfileSize {
		return nil, fmt.Errorf("keyfile exceeds size limit")
	}
	data, err := os.ReadFile(keyfilePath())
	if err != nil {
		return nil, fmt.Errorf("read keyfile: %w", err)
	}
	var kf Keyfile
	if err := json.Unmarshal(data, &kf); err != nil {
		return nil, fmt.Errorf("parse keyfile: %w", err)
	}
	if kf.Version != KeyfileVersion {
		return nil, fmt.Errorf("unsupported keyfile version %d (expected %d)", kf.Version, KeyfileVersion)
	}
	if err := ValidateKeyfile(&kf); err != nil {
		return nil, fmt.Errorf("invalid keyfile: %w", err)
	}
	return &kf, nil
}

// SaveKeyfile atomically writes keyfile.json.
func SaveKeyfile(kf *Keyfile) error {
	if err := ValidateKeyfile(kf); err != nil {
		return fmt.Errorf("refusing invalid keyfile: %w", err)
	}
	dir := stateDir()
	if err := os.MkdirAll(dir, 0700); err != nil {
		return fmt.Errorf("mkdir state: %w", err)
	}
	data, err := json.MarshalIndent(kf, "", "  ")
	if err != nil {
		return fmt.Errorf("marshal keyfile: %w", err)
	}
	return writeRecoveryFile(keyfilePath(), data)
}

// UnwrapMasterKeyFromPassphrase derives KEK from passphrase and unwraps master_key.
func UnwrapMasterKeyFromPassphrase(kf *Keyfile, passphrase []byte) ([]byte, error) {
	if len(passphrase) == 0 {
		return nil, fmt.Errorf("passphrase must not be empty")
	}
	if err := ValidateKeyfile(kf); err != nil {
		return nil, fmt.Errorf("invalid keyfile: %w", err)
	}
	// Use stored argon2 params so future param upgrades work correctly.
	p := kf.PassphraseParams
	ppKEK := deriveKEKWithParams(passphrase, p.Salt, p.Time, p.Memory, p.Threads, p.KeyLen)
	defer zero(ppKEK)
	mk, err := unwrapKey(ppKEK, kf.WrappedMasterKey)
	if err != nil {
		return nil, fmt.Errorf("wrong passphrase or corrupted keyfile: %w", err)
	}
	if len(mk) != 32 {
		return nil, fmt.Errorf("unwrapped master key has wrong length %d (expected 32)", len(mk))
	}
	return mk, nil
}

// UnwrapMasterKeyFromRecovery derives KEK from recovery bytes and unwraps master_key.
func UnwrapMasterKeyFromRecovery(kf *Keyfile, recoveryBytes []byte) ([]byte, error) {
	if len(recoveryBytes) == 0 {
		return nil, fmt.Errorf("recovery bytes must not be empty")
	}
	if err := ValidateKeyfile(kf); err != nil {
		return nil, fmt.Errorf("invalid keyfile: %w", err)
	}
	// Use stored argon2 params so future param upgrades work correctly.
	p := kf.RecoveryParams
	recKEK := deriveKEKWithParams(recoveryBytes, p.Salt, p.Time, p.Memory, p.Threads, p.KeyLen)
	defer zero(recKEK)
	mk, err := unwrapKey(recKEK, kf.WrappedMasterKeyRec)
	if err != nil {
		return nil, fmt.Errorf("wrong recovery code or corrupted keyfile: %w", err)
	}
	if len(mk) != 32 {
		return nil, fmt.Errorf("unwrapped master key has wrong length %d (expected 32)", len(mk))
	}
	return mk, nil
}

// BuildKeyfile constructs a new Keyfile from a master_key, passphrase, and recovery bytes.
func BuildKeyfile(masterKey, passphrase, recoveryBytes []byte) (*Keyfile, error) {
	if len(masterKey) != 32 {
		return nil, fmt.Errorf("master key must be exactly 32 bytes, got %d", len(masterKey))
	}
	if len(passphrase) == 0 {
		return nil, fmt.Errorf("passphrase must not be empty")
	}
	if len(recoveryBytes) != 32 {
		return nil, fmt.Errorf("recovery bytes must be exactly 32 bytes, got %d", len(recoveryBytes))
	}

	// Passphrase slot
	ppSalt, err := randBytes(argon2SaltLen)
	if err != nil {
		return nil, err
	}
	ppKEK := deriveKEK(passphrase, ppSalt)
	defer zero(ppKEK)
	wrappedMK, err := wrapKey(ppKEK, masterKey)
	if err != nil {
		return nil, err
	}

	// Recovery slot
	recSalt, err := randBytes(argon2SaltLen)
	if err != nil {
		return nil, err
	}
	recKEK := deriveKEK(recoveryBytes, recSalt)
	defer zero(recKEK)
	wrappedMKRec, err := wrapKey(recKEK, masterKey)
	if err != nil {
		return nil, err
	}

	kf := &Keyfile{
		Version: KeyfileVersion,
		PassphraseParams: ArgonParams{
			Time:    argon2Time,
			Memory:  argon2Memory,
			Threads: argon2Threads,
			KeyLen:  argon2KeyLen,
			Salt:    ppSalt,
		},
		WrappedMasterKey: wrappedMK,
		RecoveryParams: ArgonParams{
			Time:    argon2Time,
			Memory:  argon2Memory,
			Threads: argon2Threads,
			KeyLen:  argon2KeyLen,
			Salt:    recSalt,
		},
		WrappedMasterKeyRec: wrappedMKRec,
	}
	if err := ValidateKeyfile(kf); err != nil {
		return nil, fmt.Errorf("internal keyfile validation failed: %w", err)
	}
	return kf, nil
}

// ValidateKeyfile is intentionally called both at the trust boundary
// (LoadKeyfile / recovery-kit validation) and immediately before every Argon2
// derivation. The second check is defense in depth for in-memory callers.
func ValidateKeyfile(kf *Keyfile) error {
	if kf == nil {
		return fmt.Errorf("nil keyfile")
	}
	if kf.Version != KeyfileVersion {
		return fmt.Errorf("unsupported keyfile version %d", kf.Version)
	}
	if err := validateArgonParams("passphrase", kf.PassphraseParams); err != nil {
		return err
	}
	if err := validateArgonParams("recovery", kf.RecoveryParams); err != nil {
		return err
	}
	if len(kf.WrappedMasterKey) != wrappedMasterKeyLen {
		return fmt.Errorf("passphrase wrapped master key has length %d, expected %d", len(kf.WrappedMasterKey), wrappedMasterKeyLen)
	}
	if len(kf.WrappedMasterKeyRec) != wrappedMasterKeyLen {
		return fmt.Errorf("recovery wrapped master key has length %d, expected %d", len(kf.WrappedMasterKeyRec), wrappedMasterKeyLen)
	}
	return nil
}

func validateArgonParams(slot string, p ArgonParams) error {
	if len(p.Salt) != argon2SaltLen {
		return fmt.Errorf("%s Argon2 salt has length %d, expected %d", slot, len(p.Salt), argon2SaltLen)
	}
	if p.Time == 0 || p.Time > maxImportedArgonTime {
		return fmt.Errorf("%s Argon2 time %d is outside 1..%d", slot, p.Time, maxImportedArgonTime)
	}
	if p.Memory < minImportedArgonMemory || p.Memory > maxImportedArgonMemory {
		return fmt.Errorf("%s Argon2 memory %d KiB is outside %d..%d", slot, p.Memory, minImportedArgonMemory, maxImportedArgonMemory)
	}
	if p.Threads == 0 || p.Threads > maxImportedArgonThreads {
		return fmt.Errorf("%s Argon2 threads %d is outside 1..%d", slot, p.Threads, maxImportedArgonThreads)
	}
	if p.KeyLen != argon2KeyLen {
		return fmt.Errorf("%s Argon2 key length %d, expected %d", slot, p.KeyLen, argon2KeyLen)
	}
	return nil
}

// RewrapPassphrase replaces the passphrase slot without touching the recovery slot.
func RewrapPassphrase(kf *Keyfile, masterKey, newPassphrase []byte) error {
	if len(newPassphrase) == 0 {
		return fmt.Errorf("passphrase must not be empty")
	}
	ppSalt, err := randBytes(argon2SaltLen)
	if err != nil {
		return err
	}
	ppKEK := deriveKEK(newPassphrase, ppSalt)
	defer zero(ppKEK)
	wrappedMK, err := wrapKey(ppKEK, masterKey)
	if err != nil {
		return err
	}
	kf.PassphraseParams = ArgonParams{
		Time:    argon2Time,
		Memory:  argon2Memory,
		Threads: argon2Threads,
		KeyLen:  argon2KeyLen,
		Salt:    ppSalt,
	}
	kf.WrappedMasterKey = wrappedMK
	return nil
}

// Custom JSON marshalling: base64-encode []byte fields in ArgonParams
// (the default encoding from json.Marshal handles []byte as base64 automatically in Go).
// This is already the default Go behaviour — explicit confirmation only.

// base32NoPad is standard base32 without padding.
// Its alphabet (A-Z 2-7) has no overlap with '-' (the group separator),
// so stripping '-' before decode is unambiguous — unlike base64url whose
// alphabet includes '-' and '_' as data characters.
var base32NoPad = base32.StdEncoding.WithPadding(base32.NoPadding)

// RecoveryCodeToBytes decodes a recovery code (base32 groups separated by '-') to raw bytes.
func RecoveryCodeToBytes(code string) ([]byte, error) {
	// Strip hyphens and spaces — safe because base32 alphabet never contains '-'
	clean := ""
	for _, c := range code {
		if c != '-' && c != ' ' {
			clean += string(c)
		}
	}
	raw, err := base32NoPad.DecodeString(clean)
	if err != nil {
		return nil, fmt.Errorf("invalid recovery code: %w", err)
	}
	if len(raw) != 32 {
		return nil, fmt.Errorf("invalid recovery code: decoded to %d bytes, want 32", len(raw))
	}
	return raw, nil
}

// RecoveryBytesToCode encodes raw bytes to the display recovery code (base32, no padding,
// grouped in 8-character blocks separated by '-' for readability).
func RecoveryBytesToCode(b []byte) string {
	s := base32NoPad.EncodeToString(b)
	// Break into 8-char groups for readability
	var out []byte
	for i := 0; i < len(s); i++ {
		if i > 0 && i%8 == 0 {
			out = append(out, '-')
		}
		out = append(out, s[i])
	}
	return string(out)
}
