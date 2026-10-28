// config.go — read ~/.config/xnote/cloud-backup.conf and resolve XDG dirs.
//
// Recognised keys in cloud-backup.conf (shell-style KEY=value, # comments):
//   KEEP_SNAPSHOTS  int   how many local snapshots to retain (default 60)
//   CLOUD_REMOTE    str   off-box push target; empty = local-only (default).
//                         A filesystem path (starts with / ~ or .) is copied
//                         append-only with rsync. Mount remote storage first,
//                         then provide its local path (for example,
//                         /mnt/backup/xnote). Anything else is treated as an rclone
//                         remote (e.g. dropbox:xnote-backup) and needs `rclone`
//                         plus a one-time `rclone config` sign-in.
//   CLOUD_DEVICE    str   stable remote device directory (default: hostname).
//                         Set this when recovering on a differently named host.
//   CLOUD_TIMEOUT   int   hard seconds for one cloud push (default 120); guards a
//                         hung soft mount from wedging the backup.

package main

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const (
	CryptoClassic = "classic"
	CryptoHybrid  = "hybrid"
)

// Config holds settings from cloud-backup.conf (and defaults).
type Config struct {
	RclonePasswordCommand string
	KeepSnapshots         int
	// CloudRemote is the off-box push target: a mounted filesystem path
	// or an rclone remote (name:path). Empty means local-only (no push).
	CloudRemote string
	// CloudMirror is an optional second off-box target. When set, every
	// backup must reach BOTH remotes; a failure on either fails the backup.
	CloudMirror string
	// CloudDevice is the stable device directory below CloudRemote. Empty uses
	// the current hostname.
	CloudDevice string
	// CloudTimeoutSec caps a single cloud push so a hung soft mount cannot wedge
	// the backup.
	CloudTimeoutSec int
	// CryptoMode is classic (Argon2id + XChaCha20-Poly1305 only) or hybrid
	// (classic plus an ML-KEM-1024 envelope: new snapshots then need both
	// the master key and the PQ secret to open). Classic snapshots stay
	// readable in either mode.
	CryptoMode string
}

var defaultConfig = Config{
	KeepSnapshots:   60,
	CloudRemote:     "",
	CloudDevice:     "",
	CloudTimeoutSec: 120,
	CryptoMode:      CryptoClassic,
}

// LoadConfig reads cloud-backup.conf and returns a Config.
// Missing file is not an error — defaults apply.
func LoadConfig() (Config, error) {
	cfg := defaultConfig
	path := filepath.Join(configDir(), "cloud-backup.conf")
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return cfg, nil
		}
		return cfg, fmt.Errorf("open config %s: %w", path, err)
	}
	defer f.Close()

	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if k, v, ok := strings.Cut(line, "="); ok {
			k = strings.TrimSpace(k)
			v = strings.TrimSpace(v)
			if strings.HasPrefix(v, "\"") {
				if decoded, err := strconv.Unquote(v); err == nil {
					v = decoded
				} else {
					return cfg, fmt.Errorf("invalid quoted configuration value for %s", k)
				}
			} else {
				v = strings.Trim(v, "'")
			}
			switch k {
			case "KEEP_SNAPSHOTS":
				if n, err := strconv.Atoi(v); err == nil && n > 0 {
					cfg.KeepSnapshots = n
				}
			case "RCLONE_PASSWORD_COMMAND":
				cfg.RclonePasswordCommand = v
			case "CLOUD_REMOTE":
				cfg.CloudRemote = v
			case "CLOUD_MIRROR":
				cfg.CloudMirror = v
			case "CLOUD_DEVICE":
				cfg.CloudDevice = v
			case "CLOUD_TIMEOUT":
				if n, err := strconv.Atoi(v); err == nil && n > 0 {
					cfg.CloudTimeoutSec = n
				}
			case "CRYPTO_MODE":
				if v == CryptoClassic || v == CryptoHybrid {
					cfg.CryptoMode = v
				}
			}
		}
	}
	if err := sc.Err(); err != nil {
		return cfg, fmt.Errorf("read config %s: %w", path, err)
	}
	return cfg, nil
}

// SaveConfig atomically writes cloud-backup.conf in configDir() with mode 0600.
func SaveConfig(cfg Config) error {
	if cfg.KeepSnapshots <= 0 {
		cfg.KeepSnapshots = defaultConfig.KeepSnapshots
	}
	if cfg.CloudTimeoutSec <= 0 {
		cfg.CloudTimeoutSec = defaultConfig.CloudTimeoutSec
	}
	cfg.CloudRemote = strings.TrimSpace(cfg.CloudRemote)
	if cfg.CloudRemote != "" {
		if _, err := parseRemoteTarget(cfg.CloudRemote); err != nil {
			return err
		}
	}
	cfg.CloudMirror = strings.TrimSpace(cfg.CloudMirror)
	if cfg.CloudMirror != "" {
		if _, err := parseRemoteTarget(cfg.CloudMirror); err != nil {
			return err
		}
		if cfg.CloudMirror == cfg.CloudRemote {
			return fmt.Errorf("CLOUD_MIRROR must differ from CLOUD_REMOTE")
		}
	}
	if cfg.CryptoMode != CryptoClassic && cfg.CryptoMode != CryptoHybrid {
		return fmt.Errorf("invalid CRYPTO_MODE %q (expected classic or hybrid)", cfg.CryptoMode)
	}
	cfg.CloudDevice = strings.TrimSpace(cfg.CloudDevice)
	if cfg.CloudDevice != "" {
		if err := validateRemoteComponent("cloud device name", cfg.CloudDevice); err != nil {
			return err
		}
	}

	dir := configDir()
	if err := os.MkdirAll(dir, 0700); err != nil {
		return fmt.Errorf("create config dir %s: %w", dir, err)
	}
	path := filepath.Join(dir, "cloud-backup.conf")
	tmp := path + ".tmp"

	var b strings.Builder
	b.WriteString("# XNote encrypted cloud backup configuration\n")
	if cfg.RclonePasswordCommand != "" {
		fmt.Fprintf(&b, "RCLONE_PASSWORD_COMMAND=%q\n", cfg.RclonePasswordCommand)
	}
	fmt.Fprintf(&b, "KEEP_SNAPSHOTS=%d\n", cfg.KeepSnapshots)
	fmt.Fprintf(&b, "CLOUD_REMOTE=%q\n", cfg.CloudRemote)
	if cfg.CloudMirror != "" {
		fmt.Fprintf(&b, "CLOUD_MIRROR=%q\n", cfg.CloudMirror)
	}
	if cfg.CloudDevice != "" {
		fmt.Fprintf(&b, "CLOUD_DEVICE=%q\n", cfg.CloudDevice)
	}
	fmt.Fprintf(&b, "CLOUD_TIMEOUT=%d\n", cfg.CloudTimeoutSec)
	fmt.Fprintf(&b, "CRYPTO_MODE=%q\n", cfg.CryptoMode)

	if err := os.WriteFile(tmp, []byte(b.String()), 0600); err != nil {
		return fmt.Errorf("write temp config %s: %w", tmp, err)
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return fmt.Errorf("replace config %s: %w", path, err)
	}
	return nil
}

// allRemotes returns the primary plus the optional mirror target.
func (c Config) allRemotes() []string {
	var out []string
	if c.CloudRemote != "" {
		out = append(out, c.CloudRemote)
	}
	if c.CloudMirror != "" {
		out = append(out, c.CloudMirror)
	}
	return out
}

// configDir returns ~/.config/xnote
func configDir() string {
	if d := os.Getenv("XDG_CONFIG_HOME"); d != "" {
		return filepath.Join(d, "xnote")
	}
	return filepath.Join(os.Getenv("HOME"), ".config", "xnote")
}

// stateDir returns ~/.local/share/xnote-backup
func stateDir() string {
	if d := os.Getenv("XDG_DATA_HOME"); d != "" {
		return filepath.Join(d, "xnote-backup")
	}
	return filepath.Join(os.Getenv("HOME"), ".local", "share", "xnote-backup")
}
