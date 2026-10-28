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

func pushRecoveryKit(cfg Config, kitDir, hostname string) error {
	return pushRecoveryKitTo(cfg, cfg.CloudRemote, kitDir, hostname)
}

// pushRecoveryKitTo uploads an already-validated kit to one remote without
// deleting or overwriting any remote object. manifest.json is copied only
// after the payload has been uploaded and verified.
func pushRecoveryKitTo(cfg Config, remote string, kitDir, hostname string) error {
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
	target, err := parseRemoteTarget(remote)
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

const (
	defaultGDriveRemoteName  = "xnote-gdrive"
	defaultGDriveRemoteSpec  = "xnote-gdrive:xnote-backup"
	defaultDropboxRemoteName = "xnote-dropbox"
	defaultDropboxRemoteSpec = "xnote-dropbox:xnote-backup"
	oauthConnectTimeout      = 300 * time.Second
)

func rcloneConfigPath() string {
	if p := strings.TrimSpace(os.Getenv("RCLONE_CONFIG")); p != "" {
		return p
	}
	if d := strings.TrimSpace(os.Getenv("XDG_CONFIG_HOME")); d != "" {
		return filepath.Join(d, "rclone", "rclone.conf")
	}
	return filepath.Join(os.Getenv("HOME"), ".config", "rclone", "rclone.conf")
}

// isRcloneRemoteConfigured checks whether the named section exists in rclone.conf
// with the expected backend type and a non-empty token. Secret values are never
// returned or logged.
func isRcloneRemoteConfigured(remoteName, expectedType string) bool {
	data, err := os.ReadFile(rcloneConfigPath())
	if err != nil {
		return false
	}
	var inSection, hasType, hasToken bool
	targetHeader := "[" + remoteName + "]"
	for _, rawLine := range strings.Split(string(data), "\n") {
		line := strings.TrimSpace(rawLine)
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			if inSection {
				break
			}
			inSection = (line == targetHeader)
			continue
		}
		if !inSection {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		k = strings.TrimSpace(k)
		v = strings.TrimSpace(v)
		switch k {
		case "type":
			hasType = (v == expectedType)
		case "token":
			hasToken = (v != "")
		}
	}
	return inSection && hasType && hasToken
}

func classifyTargetMode(cloudRemote string) string {
	r := strings.TrimSpace(cloudRemote)
	switch {
	case r == "":
		return "local"
	case r == defaultGDriveRemoteSpec || strings.HasPrefix(r, defaultGDriveRemoteName+":") || strings.HasPrefix(r, "gdrive:"):
		return "gdrive"
	case r == defaultDropboxRemoteSpec || strings.HasPrefix(r, defaultDropboxRemoteName+":") || strings.HasPrefix(r, "dropbox:"):
		return "dropbox"
	default:
		return "custom"
	}
}

func cmdSetTarget(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: set-target <local|gdrive|dropbox|custom> [remote-spec]")
	}
	mode := strings.TrimSpace(args[0])
	cfg, err := LoadConfig()
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}
	switch mode {
	case "local":
		cfg.CloudRemote = ""
	case "gdrive":
		cfg.CloudRemote = defaultGDriveRemoteSpec
	case "dropbox":
		cfg.CloudRemote = defaultDropboxRemoteSpec
	case "custom":
		if len(args) < 2 || strings.TrimSpace(args[1]) == "" {
			return fmt.Errorf("usage: set-target custom <path-or-rclone-remote>")
		}
		spec := strings.TrimSpace(args[1])
		if _, err := parseRemoteTarget(spec); err != nil {
			return err
		}
		cfg.CloudRemote = spec
	default:
		return fmt.Errorf("unknown target mode %q (expected local, gdrive, dropbox, or custom)", mode)
	}
	if err := SaveConfig(cfg); err != nil {
		return err
	}
	log("backup target updated -> %s", mode)
	return nil
}

func cmdConnect(args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: connect <gdrive|dropbox>")
	}
	provider := strings.TrimSpace(args[0])
	var remoteName, backendType, remoteSpec string
	var createArgs []string

	switch provider {
	case "gdrive":
		remoteName = defaultGDriveRemoteName
		backendType = "drive"
		remoteSpec = defaultGDriveRemoteSpec
		createArgs = []string{"config", "create", remoteName, backendType, "scope=drive.file", "config_is_local=true", "--no-output"}
	case "dropbox":
		remoteName = defaultDropboxRemoteName
		backendType = "dropbox"
		remoteSpec = defaultDropboxRemoteSpec
		createArgs = []string{"config", "create", remoteName, backendType, "config_is_local=true", "--no-output"}
	default:
		return fmt.Errorf("unknown cloud provider %q (expected gdrive or dropbox)", provider)
	}

	if _, err := exec.LookPath("rclone"); err != nil {
		return fmt.Errorf("rclone is not installed; install the 'rclone' package to sign in to cloud storage")
	}

	ctx, cancel := context.WithTimeout(context.Background(), oauthConnectTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "rclone", createArgs...)
	out, err := cmd.CombinedOutput()
	if ctx.Err() == context.DeadlineExceeded {
		return fmt.Errorf("browser sign-in timed out after %s", oauthConnectTimeout)
	}
	if err != nil {
		return fmt.Errorf("rclone sign-in failed: %w: %s", err, strings.TrimSpace(string(out)))
	}
	if !isRcloneRemoteConfigured(remoteName, backendType) {
		return fmt.Errorf("rclone sign-in completed without storing a valid %s token for %s", backendType, remoteName)
	}

	cfg, err := LoadConfig()
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}
	cfg.CloudRemote = remoteSpec
	if err := SaveConfig(cfg); err != nil {
		return err
	}
	log("connected %s (%s)", provider, remoteSpec)
	return nil
}

func cmdDisconnect(args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: disconnect <gdrive|dropbox>")
	}
	provider := strings.TrimSpace(args[0])
	var remoteName string
	switch provider {
	case "gdrive":
		remoteName = defaultGDriveRemoteName
	case "dropbox":
		remoteName = defaultDropboxRemoteName
	default:
		return fmt.Errorf("unknown cloud provider %q (expected gdrive or dropbox)", provider)
	}

	if _, err := exec.LookPath("rclone"); err == nil {
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		cmd := exec.CommandContext(ctx, "rclone", "config", "delete", remoteName)
		if out, err := cmd.CombinedOutput(); err != nil {
			return fmt.Errorf("rclone disconnect failed: %w: %s", err, strings.TrimSpace(string(out)))
		}
	}

	cfg, err := LoadConfig()
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}
	if classifyTargetMode(cfg.CloudRemote) == provider {
		cfg.CloudRemote = ""
		if err := SaveConfig(cfg); err != nil {
			return err
		}
	}
	log("disconnected %s", provider)
	return nil
}
