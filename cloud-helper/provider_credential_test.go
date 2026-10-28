package main

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

func TestProviderCredentialCommandSurvivesGUIEnvironmentLoss(t *testing.T) {
	setupScratchHome(t)
	t.Setenv("RCLONE_PASSWORD_COMMAND", "")
	cfg := defaultConfig
	cfg.RclonePasswordCommand = "/usr/bin/printf 'synthetic provider credential'"
	if err := SaveConfig(cfg); err != nil {
		t.Fatal(err)
	}
	loaded, err := LoadConfig()
	if err != nil {
		t.Fatal(err)
	}
	if loaded.RclonePasswordCommand != cfg.RclonePasswordCommand {
		t.Fatal("quoted credential command did not round trip")
	}
	dir := t.TempDir()
	script := `#!/bin/sh
[ "$RCLONE_PASSWORD_COMMAND" = "$XNOTE_EXPECTED_PROVIDER_COMMAND" ] || exit 99
printf 'type = drive\ntoken = synthetic-fixture\n'
`
	if err := os.WriteFile(filepath.Join(dir, "rclone"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir+":"+os.Getenv("PATH"))
	t.Setenv("XNOTE_EXPECTED_PROVIDER_COMMAND", cfg.RclonePasswordCommand)
	if _, err := newRcloneCommand(context.Background(), "config", "show", "fixture").Output(); err != nil {
		t.Fatal("provider account operation lost credential command", err)
	}
	if _, err := runRemoteCommand(loaded, "rclone", "copy", "synthetic", "fixture:path"); err != nil {
		t.Fatal("scheduled transport lost credential command", err)
	}
	t.Setenv("RCLONE_PASSWORD_COMMAND", "externally supplied command")
	t.Setenv("XNOTE_EXPECTED_PROVIDER_COMMAND", "externally supplied command")
	if _, err := newRcloneCommand(context.Background(), "config", "show", "fixture").Output(); err != nil {
		t.Fatal("explicit environment did not take precedence", err)
	}
}
