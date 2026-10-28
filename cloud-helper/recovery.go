// recovery.go — versioned, self-checking off-box recovery kits.

package main

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

const (
	recoveryKitFormat    = "xnote-recovery-kit/v1"
	recoveryManifestName = "manifest.json"

	maxRecoveryManifestSize = int64(64 * 1024)
	maxRecoveryKeyfileSize  = int64(64 * 1024)
	maxRecoveryHeadSize     = int64(64 * 1024)
	maxRecoverySnapshotSize = int64(1024 * 1024 * 1024)
)

var snapshotIDPattern = regexp.MustCompile(`^[0-9]{8}T[0-9]{6}Z-g[0-9]{20}$`)

type RecoveryManifest struct {
	Format         string `json:"format"`
	CreatedUTC     string `json:"created_utc"`
	Device         string `json:"device"`
	SnapshotID     string `json:"snapshot_id"`
	Generation     uint64 `json:"generation"`
	SnapshotFile   string `json:"snapshot_file"`
	SnapshotSHA256 string `json:"snapshot_sha256"`
	KeyfileSHA256  string `json:"keyfile_sha256"`
	HeadSHA256     string `json:"head_sha256"`
}

func validateSnapshotID(id string) error {
	if !snapshotIDPattern.MatchString(id) {
		return fmt.Errorf("invalid snapshot ID %q", id)
	}
	_, err := snapshotGenerationFromID(id)
	return err
}

func recoverySnapshotRel(id string) string {
	return filepath.Join("snapshots", id+".xsnap")
}

func buildRecoveryKit(snapshotID, device string) (string, error) {
	if err := validateSnapshotID(snapshotID); err != nil {
		return "", err
	}
	if err := validateRemoteComponent("cloud device name", device); err != nil {
		return "", err
	}

	snapshotPath := SnapshotPath(snapshotID)
	if err := requireDirectoryNoSymlink(snapshotsDir()); err != nil {
		return "", fmt.Errorf("snapshot directory: %w", err)
	}
	if err := requireRegularFileMax(snapshotPath, maxRecoverySnapshotSize); err != nil {
		return "", fmt.Errorf("snapshot source: %w", err)
	}
	if err := requireRegularFileMax(keyfilePath(), maxRecoveryKeyfileSize); err != nil {
		return "", fmt.Errorf("keyfile source: %w", err)
	}
	if err := requireRegularFileMax(headPath(), maxRecoveryHeadSize); err != nil {
		return "", fmt.Errorf("head source: %w", err)
	}

	head, err := LoadHead()
	if err != nil {
		return "", err
	}
	sf, err := os.Open(snapshotPath)
	if err != nil {
		return "", err
	}
	hdr, headerErr := ReadHeader(sf)
	sf.Close()
	if headerErr != nil {
		return "", fmt.Errorf("read snapshot header: %w", headerErr)
	}
	snapshotHash, err := hashFile(snapshotPath)
	if err != nil {
		return "", err
	}
	if head.Generation != hdr.Generation || !bytes.Equal(head.HeadHash, snapshotHash[:]) {
		return "", fmt.Errorf("snapshot %s is not the validated local head", snapshotID)
	}

	stageRoot := filepath.Join(stateDir(), "remote-staging")
	if err := os.MkdirAll(stageRoot, 0700); err != nil {
		return "", fmt.Errorf("create recovery-kit staging root: %w", err)
	}
	sweepRemoteStaging(stageRoot)
	kitDir, err := os.MkdirTemp(stageRoot, "."+snapshotID+"-")
	if err != nil {
		return "", fmt.Errorf("create recovery-kit staging directory: %w", err)
	}
	ok := false
	defer func() {
		if !ok {
			os.RemoveAll(kitDir)
		}
	}()

	snapshotDest := filepath.Join(kitDir, recoverySnapshotRel(snapshotID))
	if err := copyRegularFile(snapshotPath, snapshotDest); err != nil {
		return "", err
	}
	if err := copyRegularFile(keyfilePath(), filepath.Join(kitDir, "keyfile.json")); err != nil {
		return "", err
	}
	if err := copyRegularFile(headPath(), filepath.Join(kitDir, "head.json")); err != nil {
		return "", err
	}

	keyHash, err := hashFile(filepath.Join(kitDir, "keyfile.json"))
	if err != nil {
		return "", err
	}
	headHash, err := hashFile(filepath.Join(kitDir, "head.json"))
	if err != nil {
		return "", err
	}
	manifest := RecoveryManifest{
		Format:         recoveryKitFormat,
		CreatedUTC:     time.Now().UTC().Format(time.RFC3339),
		Device:         device,
		SnapshotID:     snapshotID,
		Generation:     hdr.Generation,
		SnapshotFile:   filepath.ToSlash(recoverySnapshotRel(snapshotID)),
		SnapshotSHA256: hex.EncodeToString(snapshotHash[:]),
		KeyfileSHA256:  hex.EncodeToString(keyHash[:]),
		HeadSHA256:     hex.EncodeToString(headHash[:]),
	}
	data, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return "", err
	}
	data = append(data, '\n')
	if err := os.WriteFile(filepath.Join(kitDir, recoveryManifestName), data, 0600); err != nil {
		return "", fmt.Errorf("write recovery manifest: %w", err)
	}
	if _, err := validateRecoveryKit(kitDir); err != nil {
		return "", fmt.Errorf("validate staged recovery kit: %w", err)
	}
	if err := verifyStagingKitEncryptedOnly(kitDir, &manifest); err != nil {
		return "", fmt.Errorf("verify E2EE staging kit: %w", err)
	}
	ok = true
	return kitDir, nil
}

// sweepRemoteStaging removes orphaned staging directories left behind if a
// previous helper process was killed (e.g. SIGKILL on session logout) mid-push.
// Callers hold the exclusive backup.lock when invoking buildRecoveryKit.
func sweepRemoteStaging(stageRoot string) {
	entries, err := os.ReadDir(stageRoot)
	if err != nil {
		return
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".") {
			_ = os.RemoveAll(filepath.Join(stageRoot, entry.Name()))
		}
	}
}

// verifyStagingKitEncryptedOnly enforces that a staged recovery kit contains
// exclusively the allowlisted encrypted snapshot and metadata files—never raw
// note files (content-*, info-*, default-style) or unexpected artifacts.
func verifyStagingKitEncryptedOnly(kitDir string, manifest *RecoveryManifest) error {
	allowed := map[string]bool{
		".":                   true,
		"snapshots":           true,
		recoveryManifestName:  true,
		"keyfile.json":        true,
		"head.json":           true,
		manifest.SnapshotFile: true,
	}
	return filepath.WalkDir(kitDir, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(kitDir, path)
		if err != nil {
			return err
		}
		relSlash := filepath.ToSlash(rel)
		if !allowed[relSlash] {
			return fmt.Errorf("unexpected file in E2EE recovery kit: %s", relSlash)
		}
		if strings.HasPrefix(d.Name(), "content-") || strings.HasPrefix(d.Name(), "info-") || d.Name() == "default-style" {
			return fmt.Errorf("plaintext note file forbidden in E2EE recovery kit: %s", relSlash)
		}
		return nil
	})
}

func loadRecoveryManifest(kitDir string) (*RecoveryManifest, error) {
	manifestPath, err := requireSafeKitFile(kitDir, recoveryManifestName, maxRecoveryManifestSize)
	if err != nil {
		return nil, err
	}
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		return nil, err
	}
	var manifest RecoveryManifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return nil, fmt.Errorf("parse manifest: %w", err)
	}
	if manifest.Format != recoveryKitFormat {
		return nil, fmt.Errorf("unsupported recovery-kit format %q", manifest.Format)
	}
	if _, err := time.Parse(time.RFC3339, manifest.CreatedUTC); err != nil {
		return nil, fmt.Errorf("invalid manifest timestamp: %w", err)
	}
	if err := validateRemoteComponent("cloud device name", manifest.Device); err != nil {
		return nil, err
	}
	if err := validateSnapshotID(manifest.SnapshotID); err != nil {
		return nil, err
	}
	idGeneration, err := snapshotGenerationFromID(manifest.SnapshotID)
	if err != nil {
		return nil, err
	}
	if manifest.Generation == 0 || idGeneration != manifest.Generation {
		return nil, fmt.Errorf("manifest generation does not match snapshot ID")
	}
	if manifest.SnapshotFile != filepath.ToSlash(recoverySnapshotRel(manifest.SnapshotID)) {
		return nil, fmt.Errorf("manifest snapshot path does not match snapshot ID")
	}
	return &manifest, nil
}

func validateRecoveryKit(kitDir string) (*RecoveryManifest, error) {
	manifest, err := loadRecoveryManifest(kitDir)
	if err != nil {
		return nil, err
	}
	if err := validateRecoveryPayload(kitDir, manifest); err != nil {
		return nil, err
	}
	return manifest, nil
}

func validateRecoveryPayload(kitDir string, manifest *RecoveryManifest) error {
	snapshotPath, err := requireSafeKitFile(kitDir, filepath.FromSlash(manifest.SnapshotFile), maxRecoverySnapshotSize)
	if err != nil {
		return err
	}
	keyPath, err := requireSafeKitFile(kitDir, "keyfile.json", maxRecoveryKeyfileSize)
	if err != nil {
		return err
	}
	headFile, err := requireSafeKitFile(kitDir, "head.json", maxRecoveryHeadSize)
	if err != nil {
		return err
	}

	snapshotHash, err := hashFile(snapshotPath)
	if err != nil {
		return err
	}
	keyHash, err := hashFile(keyPath)
	if err != nil {
		return err
	}
	headFileHash, err := hashFile(headFile)
	if err != nil {
		return err
	}
	if hex.EncodeToString(snapshotHash[:]) != manifest.SnapshotSHA256 ||
		hex.EncodeToString(keyHash[:]) != manifest.KeyfileSHA256 ||
		hex.EncodeToString(headFileHash[:]) != manifest.HeadSHA256 {
		return fmt.Errorf("recovery-kit SHA256 mismatch")
	}

	keyData, err := os.ReadFile(keyPath)
	if err != nil {
		return err
	}
	var keyfile Keyfile
	if err := json.Unmarshal(keyData, &keyfile); err != nil {
		return fmt.Errorf("parse recovery keyfile: %w", err)
	}
	if err := ValidateKeyfile(&keyfile); err != nil {
		return fmt.Errorf("invalid recovery keyfile: %w", err)
	}

	headData, err := os.ReadFile(headFile)
	if err != nil {
		return err
	}
	var head HeadState
	if err := json.Unmarshal(headData, &head); err != nil {
		return fmt.Errorf("parse recovery head: %w", err)
	}
	if len(head.HeadHash) != 32 {
		return fmt.Errorf("recovery head hash has wrong length")
	}

	sf, err := os.Open(snapshotPath)
	if err != nil {
		return err
	}
	hdr, headerErr := ReadHeader(sf)
	var bodySample [512]byte
	bodyN, _ := sf.Read(bodySample[:])
	sf.Close()
	if headerErr != nil {
		return fmt.Errorf("read recovery snapshot header: %w", headerErr)
	}
	if len(hdr.WrappedDEK) != wrappedMasterKeyLen {
		return fmt.Errorf("recovery snapshot wrapped DEK has invalid length %d", len(hdr.WrappedDEK))
	}
	if hdr.PQ && len(hdr.KemCT) != mlkem1024CiphertextLen {
		return fmt.Errorf("recovery snapshot KEM ciphertext has invalid length %d", len(hdr.KemCT))
	}
	if bodyN < 4+16 {
		return fmt.Errorf("recovery snapshot ciphertext body is truncated (%d bytes)", bodyN)
	}
	if bytes.Contains(bodySample[:bodyN], []byte("ustar")) {
		return fmt.Errorf("recovery snapshot body appears unencrypted (tar magic found)")
	}
	if hdr.Generation != manifest.Generation || head.Generation != manifest.Generation {
		return fmt.Errorf("recovery generation mismatch")
	}
	if !bytes.Equal(head.HeadHash, snapshotHash[:]) {
		return fmt.Errorf("recovery head does not authenticate snapshot ciphertext")
	}
	return nil
}

func requireRegularFile(path string) error {
	return requireRegularFileMax(path, -1)
}

func requireRegularFileMax(path string, maxBytes int64) error {
	info, err := os.Lstat(path)
	if err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("%s is not a regular file", path)
	}
	if maxBytes >= 0 && info.Size() > maxBytes {
		return fmt.Errorf("%s exceeds size limit (%d > %d bytes)", path, info.Size(), maxBytes)
	}
	return nil
}

func requireDirectoryNoSymlink(path string) error {
	info, err := os.Lstat(path)
	if err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("%s is not a real directory", path)
	}
	return nil
}

// requireSafeKitFile walks every path component below kitDir with Lstat. A
// regular final file behind a symlinked snapshots/ directory is not accepted.
func requireSafeKitFile(kitDir, relativePath string, maxBytes int64) (string, error) {
	if filepath.IsAbs(relativePath) {
		return "", fmt.Errorf("recovery-kit path must be relative")
	}
	clean := filepath.Clean(relativePath)
	if clean == "." || clean == ".." || strings.HasPrefix(clean, ".."+string(filepath.Separator)) {
		return "", fmt.Errorf("unsafe recovery-kit path %q", relativePath)
	}
	if err := requireDirectoryNoSymlink(kitDir); err != nil {
		return "", err
	}
	parts := strings.Split(clean, string(filepath.Separator))
	current := kitDir
	for _, part := range parts[:len(parts)-1] {
		current = filepath.Join(current, part)
		if err := requireDirectoryNoSymlink(current); err != nil {
			return "", err
		}
	}
	path := filepath.Join(current, parts[len(parts)-1])
	if err := requireRegularFileMax(path, maxBytes); err != nil {
		return "", err
	}
	return path, nil
}

func copyRegularFile(src, dst string) error {
	if err := requireRegularFile(src); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0700); err != nil {
		return err
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	_, copyErr := out.ReadFrom(in)
	syncErr := out.Sync()
	closeErr := out.Close()
	if copyErr != nil {
		return copyErr
	}
	if syncErr != nil {
		return syncErr
	}
	return closeErr
}

func stateHasRecoveryData() bool {
	for _, path := range []string{keyfilePath(), headPath(), unlockRequiredPath()} {
		if _, err := os.Lstat(path); err == nil {
			return true
		}
	}
	ids, err := ListSnapshots()
	return err != nil || len(ids) != 0
}

func unlockRequiredPath() string {
	return filepath.Join(stateDir(), "import-needs-unlock")
}

// installRecoveryKit atomically swaps the keyfile/head/snapshot set into local
// state. --force preserves the previous set in a pre-import-* directory.
func installRecoveryKit(kitDir string, force bool) (preservedAt string, err error) {
	manifest, err := validateRecoveryKit(kitDir)
	if err != nil {
		return "", err
	}
	if stateHasRecoveryData() && !force {
		return "", fmt.Errorf("local recovery state already exists; use --force to preserve and replace it")
	}
	if err := os.MkdirAll(stateDir(), 0700); err != nil {
		return "", err
	}

	stageDir, err := os.MkdirTemp(stateDir(), ".import-new-")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(stageDir)
	if err := copyRegularFile(filepath.Join(kitDir, "keyfile.json"), filepath.Join(stageDir, "keyfile.json")); err != nil {
		return "", err
	}
	if err := copyRegularFile(filepath.Join(kitDir, "head.json"), filepath.Join(stageDir, "head.json")); err != nil {
		return "", err
	}
	if err := copyRegularFile(
		filepath.Join(kitDir, filepath.FromSlash(manifest.SnapshotFile)),
		filepath.Join(stageDir, recoverySnapshotRel(manifest.SnapshotID))); err != nil {
		return "", err
	}

	hadState := stateHasRecoveryData()
	if err := requireVerifiedUnlock("Imported recovery state must be unlocked before backup."); err != nil {
		return "", err
	}
	var preserveDir string
	if hadState {
		preserveDir, err = os.MkdirTemp(stateDir(), "pre-import-")
		if err != nil {
			return "", err
		}
		for _, item := range []string{"keyfile.json", "head.json", "snapshots"} {
			oldPath := filepath.Join(stateDir(), item)
			if _, statErr := os.Lstat(oldPath); statErr == nil {
				if renameErr := os.Rename(oldPath, filepath.Join(preserveDir, item)); renameErr != nil {
					rollbackPreservedState(preserveDir)
					return "", fmt.Errorf("preserve existing %s: %w", item, renameErr)
				}
			}
		}
	}

	if preserveDir != "" {
		if err := syncDirectory(preserveDir); err != nil {
			return preserveDir, err
		}
		if err := syncDirectory(stateDir()); err != nil {
			return preserveDir, err
		}
	}
	rollback := func() {
		os.Remove(keyfilePath())
		os.Remove(headPath())
		os.RemoveAll(snapshotsDir())
		if preserveDir != "" {
			rollbackPreservedState(preserveDir)
		}
	}
	if err := os.Rename(filepath.Join(stageDir, "snapshots"), snapshotsDir()); err != nil {
		rollback()
		return "", fmt.Errorf("install imported snapshots: %w", err)
	}
	if err := os.Rename(filepath.Join(stageDir, "keyfile.json"), keyfilePath()); err != nil {
		rollback()
		return "", fmt.Errorf("install imported keyfile: %w", err)
	}
	// Head is the completion marker for the local import.
	if err := os.Rename(filepath.Join(stageDir, "head.json"), headPath()); err != nil {
		rollback()
		return "", fmt.Errorf("install imported head: %w", err)
	}
	if err := syncRecoveryState(false); err != nil {
		return preserveDir, fmt.Errorf("imported state remains locked: %w", err)
	}
	return preserveDir, nil
}

func rollbackPreservedState(preserveDir string) {
	for _, item := range []string{"snapshots", "keyfile.json", "head.json"} {
		src := filepath.Join(preserveDir, item)
		if _, err := os.Lstat(src); err == nil {
			_ = os.Rename(src, filepath.Join(stateDir(), item))
		}
	}
	_ = os.Remove(preserveDir)
}
