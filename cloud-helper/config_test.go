package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLoadConfigMissingUsesDefaults(t *testing.T) {
	setupScratchHome(t)

	cfg, err := LoadConfig()
	if err != nil {
		t.Fatalf("missing config should use defaults: %v", err)
	}
	if cfg != defaultConfig {
		t.Fatalf("missing config = %#v, want defaults %#v", cfg, defaultConfig)
	}
}

func TestLoadConfigReportsOpenFailure(t *testing.T) {
	cfgDir, _ := setupScratchHome(t)
	configPath := filepath.Join(cfgDir, "cloud-backup.conf")
	if err := os.Symlink("cloud-backup.conf", configPath); err != nil {
		t.Fatal(err)
	}

	if _, err := LoadConfig(); err == nil || !strings.Contains(err.Error(), "open config") {
		t.Fatalf("LoadConfig error = %v, want non-missing open failure", err)
	}
}

func TestLoadConfigReportsScannerError(t *testing.T) {
	cfgDir, _ := setupScratchHome(t)
	configPath := filepath.Join(cfgDir, "cloud-backup.conf")
	oversizedLine := "CLOUD_REMOTE=" + strings.Repeat("x", 70*1024) + "\n"
	if err := os.WriteFile(configPath, []byte(oversizedLine), 0600); err != nil {
		t.Fatal(err)
	}

	if _, err := LoadConfig(); err == nil || !strings.Contains(err.Error(), "read config") {
		t.Fatalf("LoadConfig error = %v, want scanner failure", err)
	}
}

func TestSaveConfigSetTargetAndConnectLifecycle(t *testing.T) {
	setupScratchHome(t)
	rcloneConf := filepath.Join(t.TempDir(), "rclone.conf")
	t.Setenv("RCLONE_CONFIG", rcloneConf)

	stubDir := t.TempDir()
	stubRclone := filepath.Join(stubDir, "rclone")
	stubScript := "#!/bin/sh\n" +
		"if [ \"$1\" = \"config\" ] && [ \"$2\" = \"create\" ]; then\n" +
		"  name=\"$3\"\n" +
		"  btype=\"$4\"\n" +
		"  printf '[%s]\\ntype = %s\\ntoken = {\"access_token\":\"ok\"}\\n\\n' \"$name\" \"$btype\" >> \"$RCLONE_CONFIG\"\n" +
		"  exit 0\n" +
		"fi\n" +
		"if [ \"$1\" = \"config\" ] && [ \"$2\" = \"delete\" ]; then\n" +
		"  name=\"$3\"\n" +
		"  python3 -c 'import os, re; p=os.environ[\"RCLONE_CONFIG\"]; s=open(p).read() if os.path.exists(p) else \"\"; s=re.sub(r\"\\[\" + os.sys.argv[1] + r\"\\][^\\[]*\", \"\", s); open(p, \"w\").write(s)' \"$name\"\n" +
		"  exit 0\n" +
		"fi\n" +
		"exit 1\n"
	if err := os.WriteFile(stubRclone, []byte(stubScript), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", stubDir+":"+os.Getenv("PATH"))

	// 1. Connect Google Drive
	if err := cmdConnect([]string{"gdrive"}); err != nil {
		t.Fatalf("cmdConnect(gdrive): %v", err)
	}
	rep, err := buildStatusReport()
	if err != nil {
		t.Fatalf("buildStatusReport: %v", err)
	}
	if !rep.GDriveConfigured || rep.TargetMode != "gdrive" || rep.CloudRemote != defaultGDriveRemoteSpec {
		t.Fatalf("after connect gdrive: %+v", rep)
	}

	// 2. Connect Dropbox
	if err := cmdConnect([]string{"dropbox"}); err != nil {
		t.Fatalf("cmdConnect(dropbox): %v", err)
	}
	rep, err = buildStatusReport()
	if err != nil {
		t.Fatalf("buildStatusReport: %v", err)
	}
	if !rep.DropboxConfigured || rep.TargetMode != "dropbox" || rep.CloudRemote != defaultDropboxRemoteSpec {
		t.Fatalf("after connect dropbox: %+v", rep)
	}

	// 3. Switch target to local, gdrive, and custom
	if err := cmdSetTarget([]string{"local"}); err != nil {
		t.Fatalf("cmdSetTarget(local): %v", err)
	}
	cfg, err := LoadConfig()
	if err != nil || cfg.CloudRemote != "" {
		t.Fatalf("after set-target local: cfg=%+v err=%v", cfg, err)
	}

	if err := cmdSetTarget([]string{"custom", "/tmp/xnote-mounted-backup"}); err != nil {
		t.Fatalf("cmdSetTarget(custom): %v", err)
	}
	rep, err = buildStatusReport()
	if err != nil || rep.TargetMode != "custom" || rep.CloudRemote != "/tmp/xnote-mounted-backup" {
		t.Fatalf("after set-target custom: %+v err=%v", rep, err)
	}

	// Bare words are rejected
	if err := cmdSetTarget([]string{"custom", "bareword"}); err == nil {
		t.Fatal("expected bare word custom target to be rejected")
	}

	// 4. Disconnect Dropbox while active -> resets CloudRemote to local
	if err := cmdSetTarget([]string{"dropbox"}); err != nil {
		t.Fatal(err)
	}
	if err := cmdDisconnect([]string{"dropbox"}); err != nil {
		t.Fatalf("cmdDisconnect(dropbox): %v", err)
	}
	rep, err = buildStatusReport()
	if err != nil {
		t.Fatal(err)
	}
	if rep.DropboxConfigured || rep.TargetMode != "local" || rep.CloudRemote != "" {
		t.Fatalf("after disconnect dropbox: %+v", rep)
	}
}
