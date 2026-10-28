// status.go — durable, non-secret backup health state.

package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

type RemoteStatus struct {
	LastAttemptUTC        string `json:"last_attempt_utc,omitempty"`
	LastAttemptSnapshotID string `json:"last_attempt_snapshot_id,omitempty"`
	LastSuccessUTC        string `json:"last_success_utc,omitempty"`
	LastSuccessSnapshotID string `json:"last_success_snapshot_id,omitempty"`
	LastError             string `json:"last_error,omitempty"`
	Device                string `json:"device,omitempty"`
}

type StatusReport struct {
	Initialised           bool   `json:"initialised"`
	KeyCached             bool   `json:"key_cached"`
	UnlockRequired        bool   `json:"unlock_required"`
	LocalSnapshots        int    `json:"local_snapshots"`
	HeadGeneration        uint64 `json:"head_generation"`
	KeepSnapshots         int    `json:"keep_snapshots"`
	CloudRemote           string `json:"cloud_remote"`
	CloudMirror           string `json:"cloud_mirror"`
	CryptoMode            string `json:"crypto_mode"`
	PQFingerprint         string `json:"pq_fingerprint"`
	CloudDevice           string `json:"cloud_device"`
	TargetMode            string `json:"target_mode"`
	GDriveConfigured      bool   `json:"gdrive_configured"`
	DropboxConfigured     bool   `json:"dropbox_configured"`
	LastAttemptUTC        string `json:"last_attempt_utc,omitempty"`
	LastAttemptSnapshotID string `json:"last_attempt_snapshot_id,omitempty"`
	LastSuccessUTC        string `json:"last_success_utc,omitempty"`
	LastSuccessSnapshotID string `json:"last_success_snapshot_id,omitempty"`
	LastError             string `json:"last_error,omitempty"`
	LastDrillUTC          string `json:"last_drill_utc,omitempty"`
	LastDrillResult       string `json:"last_drill_result,omitempty"`
	LastDrillSnapshotID   string `json:"last_drill_snapshot_id,omitempty"`
}

func remoteStatusPath() string {
	return filepath.Join(stateDir(), "remote-status.json")
}

func loadRemoteStatus() (*RemoteStatus, error) {
	data, err := os.ReadFile(remoteStatusPath())
	if os.IsNotExist(err) {
		return &RemoteStatus{}, nil
	}
	if err != nil {
		return nil, err
	}
	var status RemoteStatus
	if err := json.Unmarshal(data, &status); err != nil {
		return nil, err
	}
	return &status, nil
}

func recordRemoteAttempt(snapshotID, device string, pushErr error) error {
	status, err := loadRemoteStatus()
	if err != nil {
		status = &RemoteStatus{}
	}
	now := time.Now().UTC().Format(time.RFC3339)
	status.LastAttemptUTC = now
	status.LastAttemptSnapshotID = snapshotID
	status.Device = device
	if pushErr == nil {
		status.LastSuccessUTC = now
		status.LastSuccessSnapshotID = snapshotID
		status.LastError = ""
	} else {
		status.LastError = pushErr.Error()
	}
	data, err := json.MarshalIndent(status, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	if err := os.MkdirAll(stateDir(), 0700); err != nil {
		return err
	}
	tmp := remoteStatusPath() + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, remoteStatusPath())
}

func sanitizeSingleLine(s string) string {
	s = strings.ReplaceAll(s, "\r", " ")
	s = strings.ReplaceAll(s, "\n", " ")
	return strings.TrimSpace(s)
}

func buildStatusReport() (*StatusReport, error) {
	cfg, err := LoadConfig()
	if err != nil {
		return nil, fmt.Errorf("load config: %w", err)
	}
	initOK := isInitialised()
	_, unlockStatErr := os.Stat(unlockRequiredPath())
	unlockReq := (unlockStatErr == nil)

	keyCached := false
	if initOK {
		if mk, kerr := loadMasterKey(); kerr == nil && len(mk) == 32 {
			keyCached = true
			zero(mk)
		}
	}

	ids, err := ListSnapshots()
	if err != nil {
		return nil, err
	}
	var headGen uint64
	if head, herr := LoadHead(); herr == nil {
		headGen = head.Generation
	}

	host, err := remoteHost(cfg, hostname())
	if err != nil {
		host = hostname()
	}

	drill := loadDrillStatus()
	rep := &StatusReport{
		Initialised:       initOK,
		KeyCached:         keyCached,
		UnlockRequired:    unlockReq,
		LocalSnapshots:    len(ids),
		HeadGeneration:    headGen,
		KeepSnapshots:     cfg.KeepSnapshots,
		CloudRemote:       cfg.CloudRemote,
		CloudMirror:       cfg.CloudMirror,
		CryptoMode:        cfg.CryptoMode,
		PQFingerprint:     pqFingerprint(),
		CloudDevice:       host,
		TargetMode:        classifyTargetMode(cfg.CloudRemote),
		GDriveConfigured:  isRcloneRemoteConfigured(defaultGDriveRemoteName, "drive") || isRcloneRemoteConfigured("gdrive", "drive"),
		DropboxConfigured: isRcloneRemoteConfigured(defaultDropboxRemoteName, "dropbox") || isRcloneRemoteConfigured("dropbox", "dropbox"),
		LastDrillUTC:      drill.LastDrillUTC,
		LastDrillResult:   drill.Result,
		LastDrillSnapshotID: drill.SnapshotID,
	}

	if status, serr := loadRemoteStatus(); serr == nil && status != nil {
		rep.LastAttemptUTC = status.LastAttemptUTC
		rep.LastAttemptSnapshotID = status.LastAttemptSnapshotID
		rep.LastSuccessUTC = status.LastSuccessUTC
		rep.LastSuccessSnapshotID = status.LastSuccessSnapshotID
		rep.LastError = status.LastError
	}
	return rep, nil
}

func cmdStatus(args ...string) error {
	mode := "human"
	for _, a := range args {
		switch a {
		case "--json":
			mode = "json"
		case "--porcelain":
			mode = "porcelain"
		default:
			return fmt.Errorf("unknown status option: %s", a)
		}
	}

	if mode == "json" || mode == "porcelain" {
		rep, err := buildStatusReport()
		if err != nil {
			return err
		}
		if mode == "json" {
			data, err := json.MarshalIndent(rep, "", "  ")
			if err != nil {
				return err
			}
			fmt.Println(string(data))
			return nil
		}
		fmt.Printf("initialised=%t\n", rep.Initialised)
		fmt.Printf("key_cached=%t\n", rep.KeyCached)
		fmt.Printf("unlock_required=%t\n", rep.UnlockRequired)
		fmt.Printf("local_snapshots=%d\n", rep.LocalSnapshots)
		fmt.Printf("head_generation=%d\n", rep.HeadGeneration)
		fmt.Printf("keep_snapshots=%d\n", rep.KeepSnapshots)
		fmt.Printf("cloud_remote=%s\n", sanitizeSingleLine(rep.CloudRemote))
		fmt.Printf("cloud_device=%s\n", sanitizeSingleLine(rep.CloudDevice))
		fmt.Printf("target_mode=%s\n", sanitizeSingleLine(rep.TargetMode))
		fmt.Printf("gdrive_configured=%t\n", rep.GDriveConfigured)
		fmt.Printf("dropbox_configured=%t\n", rep.DropboxConfigured)
		fmt.Printf("last_attempt_utc=%s\n", sanitizeSingleLine(rep.LastAttemptUTC))
		fmt.Printf("last_attempt_snapshot_id=%s\n", sanitizeSingleLine(rep.LastAttemptSnapshotID))
		fmt.Printf("last_success_utc=%s\n", sanitizeSingleLine(rep.LastSuccessUTC))
		fmt.Printf("last_success_snapshot_id=%s\n", sanitizeSingleLine(rep.LastSuccessSnapshotID))
		fmt.Printf("last_error=%s\n", sanitizeSingleLine(rep.LastError))
		fmt.Printf("cloud_mirror=%s\n", sanitizeSingleLine(rep.CloudMirror))
		fmt.Printf("crypto_mode=%s\n", sanitizeSingleLine(rep.CryptoMode))
		fmt.Printf("pq_fingerprint=%s\n", sanitizeSingleLine(rep.PQFingerprint))
		fmt.Printf("last_drill_utc=%s\n", sanitizeSingleLine(rep.LastDrillUTC))
		fmt.Printf("last_drill_result=%s\n", sanitizeSingleLine(rep.LastDrillResult))
		fmt.Printf("last_drill_snapshot_id=%s\n", sanitizeSingleLine(rep.LastDrillSnapshotID))
		return nil
	}

	cfg, err := LoadConfig()
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}
	fmt.Printf("Initialised: %t\n", isInitialised())
	if _, err := os.Stat(unlockRequiredPath()); err == nil {
		fmt.Println("Imported state unlock required: yes")
	}
	ids, err := ListSnapshots()
	if err != nil {
		return err
	}
	fmt.Printf("Local snapshots: %d\n", len(ids))
	if head, err := LoadHead(); err == nil {
		fmt.Printf("Head generation: %d\n", head.Generation)
	}
	if cfg.CloudRemote == "" {
		fmt.Println("Cloud remote: not configured")
		return nil
	}
	host, err := remoteHost(cfg, hostname())
	if err != nil {
		return err
	}
	fmt.Printf("Cloud remote: configured (device %s)\n", host)
	status, err := loadRemoteStatus()
	if err != nil {
		return fmt.Errorf("load remote status: %w", err)
	}
	if status.LastAttemptUTC == "" {
		fmt.Println("Remote attempt: never")
		return nil
	}
	fmt.Printf("Remote last attempt: %s (%s)\n", status.LastAttemptUTC, status.LastAttemptSnapshotID)
	if status.LastSuccessUTC != "" {
		fmt.Printf("Remote last success: %s (%s)\n", status.LastSuccessUTC, status.LastSuccessSnapshotID)
	}
	if status.LastError != "" {
		fmt.Printf("Remote last error: %s\n", status.LastError)
	}
	return nil
}

func hostname() string {
	host, _ := os.Hostname()
	return host
}
