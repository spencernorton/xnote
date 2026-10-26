// remote.go — recovery-safe off-box transport.
//
// CLOUD_REMOTE accepts either an explicit filesystem path (/..., ~/..., ./...)
// or an rclone remote (name:path). A bare word is rejected: silently treating a
// misspelled rclone name as a local directory would create a false "cloud OK".
//
// Remote data is append-only. Every backup uploads one complete recovery kit
// under <remote>/<device>/kits/<snapshot-id>/; neither rsync nor rclone is ever
// allowed to delete remote files. manifest.json is published last, so listing a
// remote only exposes kits whose payload upload completed.

package main

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

var (
	remoteNamePattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_-]*$`)
	componentPattern  = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$`)
)

type remoteTarget struct {
	isPath bool
	root   string
}

// remoteIsPath reports only explicit path syntax. Bare values are deliberately
// not paths; parseRemoteTarget rejects them as likely mistyped rclone remotes.
func remoteIsPath(remote string) bool {
	remote = strings.TrimSpace(remote)
	return strings.HasPrefix(remote, "/") ||
		remote == "~" || strings.HasPrefix(remote, "~/") ||
		remote == "." || remote == ".." ||
		strings.HasPrefix(remote, "./") || strings.HasPrefix(remote, "../")
}

func expandHome(p string) string {
	if p == "~" {
		return os.Getenv("HOME")
	}
	if strings.HasPrefix(p, "~/") {
		return filepath.Join(os.Getenv("HOME"), p[2:])
	}
	return p
}

func parseRemoteTarget(value string) (remoteTarget, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return remoteTarget{}, fmt.Errorf("CLOUD_REMOTE is not configured")
	}

	if remoteIsPath(value) {
		root, err := filepath.Abs(expandHome(value))
		if err != nil {
			return remoteTarget{}, fmt.Errorf("resolve CLOUD_REMOTE path: %w", err)
		}
		root = filepath.Clean(root)
		if root == string(filepath.Separator) {
			return remoteTarget{}, fmt.Errorf("refusing filesystem CLOUD_REMOTE at root directory")
		}
		return remoteTarget{isPath: true, root: root}, nil
	}

	name, remotePath, ok := strings.Cut(value, ":")
	if !ok {
		return remoteTarget{}, fmt.Errorf("invalid CLOUD_REMOTE %q: use an explicit path (/..., ~/..., ./...) or rclone remote name:path", value)
	}
	if !remoteNamePattern.MatchString(name) {
		return remoteTarget{}, fmt.Errorf("invalid rclone remote name %q", name)
	}
	for _, part := range strings.Split(strings.Trim(remotePath, "/"), "/") {
		if part == ".." {
			return remoteTarget{}, fmt.Errorf("CLOUD_REMOTE must not contain '..' path components")
		}
	}
	return remoteTarget{root: name + ":" + strings.Trim(remotePath, "/")}, nil
}

func validateRemoteComponent(kind, value string) error {
	if !componentPattern.MatchString(value) || value == "." || value == ".." {
		return fmt.Errorf("invalid %s %q", kind, value)
	}
	return nil
}

func remoteHost(cfg Config, fallback string) (string, error) {
	host := strings.TrimSpace(cfg.CloudDevice)
	if host == "" {
		host = fallback
	}
	if host == "" {
		host = "unknown-host"
	}
	if err := validateRemoteComponent("cloud device name", host); err != nil {
		return "", err
	}
	return host, nil
}

func (t remoteTarget) join(parts ...string) string {
	if t.isPath {
		all := append([]string{t.root}, parts...)
		return filepath.Join(all...)
	}
	base := strings.TrimRight(t.root, "/")
	if len(parts) == 0 {
		return base
	}
	return base + "/" + strings.Join(parts, "/")
}

func timeoutFor(cfg Config) time.Duration {
	timeout := time.Duration(cfg.CloudTimeoutSec) * time.Second
	if timeout <= 0 {
		timeout = 120 * time.Second
	}
	return timeout
}

func runRemoteCommand(cfg Config, name string, args ...string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), timeoutFor(cfg))
	defer cancel()
	cmd := exec.CommandContext(ctx, name, args...)
	out, err := cmd.CombinedOutput()
	if ctx.Err() == context.DeadlineExceeded {
		return "", fmt.Errorf("%s timed out after %s", name, timeoutFor(cfg))
	}
	if err != nil {
		return "", fmt.Errorf("%s failed: %w: %s", name, err, strings.TrimSpace(string(out)))
	}
	return string(out), nil
}

func ensureTransportAvailable(target remoteTarget) error {
	name := "rclone"
	if target.isPath {
		name = "rsync"
	}
	if _, err := exec.LookPath(name); err != nil {
		return fmt.Errorf("%s not found on PATH", name)
	}
	return nil
}

func validatePathDestination(dest string) error {
	destAbs, err := canonicalPath(dest)
	if err != nil {
		return err
	}
	stateAbs, err := canonicalPath(stateDir())
	if err != nil {
		return err
	}
	configAbs, err := canonicalPath(configDir())
	if err != nil {
		return err
	}
	if pathContains(stateAbs, destAbs) || pathContains(configAbs, destAbs) ||
		pathContains(destAbs, stateAbs) || pathContains(destAbs, configAbs) {
		return fmt.Errorf("refusing CLOUD_REMOTE destination %q because it overlaps XNote state or config", destAbs)
	}
	return nil
}

// canonicalPath resolves symlinks in the longest existing prefix, then puts
// any not-yet-created tail back. This closes the "remote path is a symlink into
// stateDir" false-off-box-success case even before the destination exists.
func canonicalPath(path string) (string, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	current := filepath.Clean(abs)
	var tail []string
	for {
		_, statErr := os.Lstat(current)
		if statErr == nil {
			break
		}
		if !os.IsNotExist(statErr) {
			return "", statErr
		}
		parent := filepath.Dir(current)
		if parent == current {
			return "", statErr
		}
		tail = append(tail, filepath.Base(current))
		current = parent
	}
	current, err = filepath.EvalSymlinks(current)
	if err != nil {
		return "", err
	}
	for i := len(tail) - 1; i >= 0; i-- {
		current = filepath.Join(current, tail[i])
	}
	return filepath.Clean(current), nil
}

func pathContains(parent, child string) bool {
	rel, err := filepath.Rel(parent, child)
	if err != nil {
		return false
	}
	return rel == "." || (rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)))
}

// pushRecoveryKit uploads an already-validated kit without deleting or
// overwriting any remote object. manifest.json is copied only after the payload
// has been uploaded and verified.
func pushRecoveryKit(cfg Config, kitDir, hostname string) error {
	manifest, err := validateRecoveryKit(kitDir)
	if err != nil {
		return fmt.Errorf("validate local recovery kit: %w", err)
	}
	if err := verifyStagingKitEncryptedOnly(kitDir, manifest); err != nil {
		return fmt.Errorf("verify local E2EE recovery kit: %w", err)
	}
	if err := validateSnapshotID(manifest.SnapshotID); err != nil {
		return err
	}
	target, err := parseRemoteTarget(cfg.CloudRemote)
	if err != nil {
		return err
	}
	if err := ensureTransportAvailable(target); err != nil {
		return err
	}
	if err := validateRemoteComponent("cloud device name", hostname); err != nil {
		return err
	}

	dest := target.join(hostname, "kits", manifest.SnapshotID)
	if target.isPath {
		if err := validatePathDestination(dest); err != nil {
			return err
		}
		if _, err := runRemoteCommand(cfg, "mkdir", "-p", "--", dest); err != nil {
			return fmt.Errorf("create remote kit directory: %w", err)
		}
		if _, err := runRemoteCommand(cfg, "rsync", "-a", "--ignore-existing",
			"--exclude=manifest.json", "--", kitDir+"/", dest+"/"); err != nil {
			return fmt.Errorf("copy recovery-kit payload: %w", err)
		}
		if err := validateRecoveryPayload(dest, manifest); err != nil {
			return fmt.Errorf("remote recovery-kit payload verification failed: %w", err)
		}
		if _, err := runRemoteCommand(cfg, "rsync", "-a", "--ignore-existing", "--",
			filepath.Join(kitDir, recoveryManifestName), filepath.Join(dest, recoveryManifestName)); err != nil {
			return fmt.Errorf("publish recovery-kit manifest: %w", err)
		}
		if _, err := validateRecoveryKit(dest); err != nil {
			return fmt.Errorf("remote recovery-kit verification failed: %w", err)
		}
		return nil
	}

	if _, err := runRemoteCommand(cfg, "rclone", "copy", kitDir, dest,
		"--immutable", "--checksum", "--exclude", recoveryManifestName); err != nil {
		return fmt.Errorf("upload recovery-kit payload: %w", err)
	}
	if err := verifyRclonePayload(cfg, kitDir, dest); err != nil {
		return fmt.Errorf("verify recovery-kit payload: %w", err)
	}
	if _, err := runRemoteCommand(cfg, "rclone", "copyto",
		filepath.Join(kitDir, recoveryManifestName),
		strings.TrimRight(dest, "/")+"/"+recoveryManifestName,
		"--immutable", "--checksum"); err != nil {
		return fmt.Errorf("publish recovery-kit manifest: %w", err)
	}
	readbackPath := filepath.Join(kitDir, ".manifest-readback")
	defer os.Remove(readbackPath)
	if err := rcloneCatToFile(
		cfg,
		strings.TrimRight(dest, "/")+"/"+recoveryManifestName,
		readbackPath,
		maxRecoveryManifestSize,
	); err != nil {
		return fmt.Errorf("read back recovery-kit manifest: %w", err)
	}
	remoteManifest, err := os.ReadFile(readbackPath)
	if err != nil {
		return err
	}
	localManifest, err := os.ReadFile(filepath.Join(kitDir, recoveryManifestName))
	if err != nil {
		return err
	}
	if !bytes.Equal(remoteManifest, localManifest) {
		return fmt.Errorf("remote recovery-kit manifest verification failed")
	}
	return nil
}

// verifyRclonePayload verifies that the uploaded recovery-kit payload on an
// rclone remote matches local ciphertext hashes. Plain remotes (Dropbox,
// Google Drive, local/alias) support `rclone check --checksum` directly.
// Encrypted rclone `crypt` remotes (e.g. gdrive-vault:) do not expose
// plaintext hashes to `rclone check --checksum` and emit "No common hash found"
// or "hashes could not be checked"; in that case we run `rclone cryptcheck`
// (falling back to `rclone check --download` if the backend is neither
// hash-capable nor a crypt remote).
func verifyRclonePayload(cfg Config, kitDir, dest string) error {
	out, err := runRemoteCommand(cfg, "rclone", "check", kitDir, dest,
		"--one-way", "--checksum", "--exclude", recoveryManifestName)
	if err != nil {
		return err
	}
	if strings.Contains(out, "No common hash found") || strings.Contains(out, "hashes could not be checked") {
		if _, cryptErr := runRemoteCommand(cfg, "rclone", "cryptcheck", kitDir, dest,
			"--one-way", "--exclude", recoveryManifestName); cryptErr != nil {
			if strings.Contains(cryptErr.Error(), "is not a crypt remote") {
				_, dlErr := runRemoteCommand(cfg, "rclone", "check", kitDir, dest,
					"--one-way", "--download", "--exclude", recoveryManifestName)
				return dlErr
			}
			return cryptErr
		}
	}
	return nil
}

func listRemoteKits(cfg Config, hostname string) ([]string, error) {
	target, err := parseRemoteTarget(cfg.CloudRemote)
	if err != nil {
		return nil, err
	}
	if err := ensureTransportAvailable(target); err != nil {
		return nil, err
	}
	if err := validateRemoteComponent("cloud device name", hostname); err != nil {
		return nil, err
	}

	base := target.join(hostname, "kits")
	var ids []string
	if target.isPath {
		entries, err := os.ReadDir(base)
		if os.IsNotExist(err) {
			return nil, nil
		}
		if err != nil {
			return nil, fmt.Errorf("list remote kits: %w", err)
		}
		for _, entry := range entries {
			if !entry.IsDir() || validateSnapshotID(entry.Name()) != nil {
				continue
			}
			if _, err := requireSafeKitFile(
				filepath.Join(base, entry.Name()),
				recoveryManifestName,
				maxRecoveryManifestSize,
			); err == nil {
				ids = append(ids, entry.Name())
			}
		}
	} else {
		out, err := runRemoteCommand(cfg, "rclone", "lsf", base,
			"--recursive", "--files-only", "--include", "*/"+recoveryManifestName)
		if err != nil {
			return nil, fmt.Errorf("list remote kits: %w", err)
		}
		seen := make(map[string]bool)
		for _, line := range strings.Split(out, "\n") {
			line = strings.TrimSpace(line)
			parts := strings.Split(line, "/")
			if len(parts) == 2 && parts[1] == recoveryManifestName &&
				validateSnapshotID(parts[0]) == nil && !seen[parts[0]] {
				seen[parts[0]] = true
				ids = append(ids, parts[0])
			}
		}
	}
	sortSnapshotIDsByGeneration(ids)
	return ids, nil
}

func resolveRemoteKitID(cfg Config, requested, hostname string) (string, error) {
	if requested != "latest" {
		if err := validateSnapshotID(requested); err != nil {
			return "", err
		}
		return requested, nil
	}
	ids, err := listRemoteKits(cfg, hostname)
	if err != nil {
		return "", err
	}
	if len(ids) == 0 {
		return "", fmt.Errorf("no complete remote recovery kits found for %s", hostname)
	}
	return ids[len(ids)-1], nil
}

// fetchRecoveryKit downloads one complete remote kit into an empty local
// directory and verifies every hash and the snapshot/head relationship.
func fetchRecoveryKit(cfg Config, requested, hostname, destDir string) (string, error) {
	if err := validateRemoteComponent("cloud device name", hostname); err != nil {
		return "", err
	}
	id, err := resolveRemoteKitID(cfg, requested, hostname)
	if err != nil {
		return "", err
	}
	target, err := parseRemoteTarget(cfg.CloudRemote)
	if err != nil {
		return "", err
	}
	if err := ensureTransportAvailable(target); err != nil {
		return "", err
	}
	if entries, err := os.ReadDir(destDir); err != nil {
		return "", fmt.Errorf("read fetch destination: %w", err)
	} else if len(entries) != 0 {
		return "", fmt.Errorf("fetch destination must be empty")
	}

	source := target.join(hostname, "kits", id)
	if err := fetchRecoveryFile(cfg, target, source, recoveryManifestName, destDir, maxRecoveryManifestSize); err != nil {
		return "", fmt.Errorf("download recovery manifest: %w", err)
	}
	manifest, err := loadRecoveryManifest(destDir)
	if err != nil {
		return "", fmt.Errorf("downloaded recovery manifest is invalid: %w", err)
	}
	if manifest.SnapshotID != id || manifest.Device != hostname {
		return "", fmt.Errorf("downloaded recovery-kit identity mismatch")
	}
	for _, file := range []struct {
		path string
		max  int64
	}{
		{"keyfile.json", maxRecoveryKeyfileSize},
		{"head.json", maxRecoveryHeadSize},
		{manifest.SnapshotFile, maxRecoverySnapshotSize},
	} {
		if err := fetchRecoveryFile(cfg, target, source, file.path, destDir, file.max); err != nil {
			return "", fmt.Errorf("download recovery file %s: %w", file.path, err)
		}
	}
	manifest, err = validateRecoveryKit(destDir)
	if err != nil {
		return "", fmt.Errorf("downloaded recovery kit is invalid: %w", err)
	}
	if manifest.SnapshotID != id || manifest.Device != hostname {
		return "", fmt.Errorf("downloaded recovery kit identity mismatch")
	}
	return id, nil
}

func fetchRecoveryFile(cfg Config, target remoteTarget, sourceRoot, relativePath, destDir string, maxBytes int64) error {
	clean := filepath.ToSlash(filepath.Clean(filepath.FromSlash(relativePath)))
	if clean == "." || clean == ".." || strings.HasPrefix(clean, "../") {
		return fmt.Errorf("unsafe recovery path %q", relativePath)
	}
	dest := filepath.Join(destDir, filepath.FromSlash(clean))
	if err := os.MkdirAll(filepath.Dir(dest), 0700); err != nil {
		return err
	}

	if target.isPath {
		source, err := requireSafeKitFile(sourceRoot, filepath.FromSlash(clean), maxBytes)
		if err != nil {
			return err
		}
		if _, err := runRemoteCommand(cfg, "rsync", "-a",
			"--max-size="+strconv.FormatInt(maxBytes, 10), "--", source, dest); err != nil {
			return err
		}
	} else {
		source := strings.TrimRight(sourceRoot, "/") + "/" + clean
		if err := rcloneCatToFile(cfg, source, dest, maxBytes); err != nil {
			return err
		}
	}
	_, err := requireSafeKitFile(destDir, filepath.FromSlash(clean), maxBytes)
	return err
}

func rcloneCatToFile(cfg Config, source, dest string, maxBytes int64) error {
	ctx, cancel := context.WithTimeout(context.Background(), timeoutFor(cfg))
	defer cancel()
	cmd := exec.CommandContext(ctx, "rclone", "cat", source)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	if err := cmd.Start(); err != nil {
		return err
	}

	out, err := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return err
	}
	n, copyErr := io.Copy(out, io.LimitReader(stdout, maxBytes+1))
	syncErr := out.Sync()
	closeErr := out.Close()
	if n > maxBytes || copyErr != nil {
		_ = cmd.Process.Kill()
	}
	waitErr := cmd.Wait()
	if n > maxBytes {
		_ = os.Remove(dest)
		return fmt.Errorf("remote file exceeds size limit of %d bytes", maxBytes)
	}
	if copyErr != nil {
		_ = os.Remove(dest)
		return copyErr
	}
	if ctx.Err() == context.DeadlineExceeded {
		_ = os.Remove(dest)
		return fmt.Errorf("rclone timed out after %s", timeoutFor(cfg))
	}
	if waitErr != nil {
		_ = os.Remove(dest)
		return fmt.Errorf("rclone cat failed: %w: %s", waitErr, strings.TrimSpace(stderr.String()))
	}
	if syncErr != nil {
		_ = os.Remove(dest)
		return syncErr
	}
	if closeErr != nil {
		_ = os.Remove(dest)
		return closeErr
	}
	return nil
}
