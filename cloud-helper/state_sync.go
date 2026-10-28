package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
)

func syncDirectory(path string) error {
	d, err := os.Open(path)
	if err != nil {
		return err
	}
	defer d.Close()
	return d.Sync()
}
func writeRecoveryFile(path string, data []byte) error {
	f, err := os.CreateTemp(filepath.Dir(path), ".recovery-write-")
	if err != nil {
		return err
	}
	tmp := f.Name()
	defer os.Remove(tmp)
	if _, err = f.Write(data); err != nil {
		f.Close()
		return err
	}
	if err = f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	if err = os.Rename(tmp, path); err != nil {
		return err
	}
	return syncDirectory(filepath.Dir(path))
}

// Verify the installed recovery set is complete and flush it before allowing
// a cached key to enable new backups. A persistence failure keeps the interlock.
func syncRecoveryState(checkPQ bool) error {
	if _, err := LoadKeyfile(); err != nil {
		return err
	}
	ids, err := ListSnapshots()
	if err != nil {
		return err
	}
	if len(ids) > 0 {
		head, err := LoadHead()
		if err != nil {
			return err
		}
		id := ids[len(ids)-1]
		gen, err := snapshotGenerationFromID(id)
		if err != nil {
			return err
		}
		hash, err := hashFile(SnapshotPath(id))
		if err != nil {
			return err
		}
		if gen != head.Generation || !bytes.Equal(hash[:], head.HeadHash) {
			return fmt.Errorf("recovery head is incomplete or inconsistent; backups remain locked")
		}
	}
	paths := []string{keyfilePath()}
	needPQ := false
	if _, err := os.Lstat(headPath()); err == nil {
		paths = append(paths, headPath())
	} else if !os.IsNotExist(err) {
		return err
	}
	for _, id := range ids {
		paths = append(paths, SnapshotPath(id))
		f, err := os.Open(SnapshotPath(id))
		if err != nil {
			return err
		}
		hdr, herr := ReadHeader(f)
		f.Close()
		if herr != nil {
			return herr
		}
		needPQ = needPQ || hdr.PQ
	}
	if needPQ && checkPQ {
		if _, err := pqLoadPublic(); err != nil {
			return err
		}
	}
	for _, p := range []string{pqSecretPath(), pqPublicPath()} {
		if _, err := os.Lstat(p); err == nil {
			paths = append(paths, p)
		} else if !os.IsNotExist(err) {
			return err
		}
	}
	for _, path := range paths {
		if err := requireRegularFile(path); err != nil {
			return err
		}
		f, err := os.Open(path)
		if err != nil {
			return err
		}
		err = f.Sync()
		closeErr := f.Close()
		if err != nil {
			return err
		}
		if closeErr != nil {
			return closeErr
		}
	}
	if _, err := os.Lstat(snapshotsDir()); err == nil {
		if err := syncDirectory(snapshotsDir()); err != nil {
			return err
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	return syncDirectory(stateDir())
}
