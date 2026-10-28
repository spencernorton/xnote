// snapshot.go — snapshot header encoding, head.json state, and snapshot storage.
//
// Snapshot file format (binary, all multi-byte fields big-endian):
//   [12 bytes]  magic "XNOTE-SNAP-1"
//   [8 bytes]   generation (uint64)
//   [32 bytes]  parent_cipher_hash (sha256 of previous snapshot ciphertext, or zeros)
//   [2 bytes]   wrapped_dek_len (uint16)
//   [N bytes]   wrapped_dek (nonce||ciphertext from wrapKey)
//   [24 bytes]  base_nonce for stream decryption
//   --- end of header; all bytes above are authenticated as AAD of chunk 0 ---
//   [stream]    length-prefixed AEAD chunks (from EncryptStream)
//
// head.json tracks generation and head_hash for rollback protection.

package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// HeadState is the local anti-rollback record.
type HeadState struct {
	Generation uint64 `json:"generation"`
	HeadHash   []byte `json:"head_hash"` // sha256 of the most recent snapshot ciphertext
}

func headPath() string {
	return filepath.Join(snapshotsDir(), "..", "head.json")
}

func snapshotsDir() string {
	return filepath.Join(stateDir(), "snapshots")
}

// LoadHead reads head.json. Returns zero HeadState if missing (first run).
func LoadHead() (*HeadState, error) {
	data, err := os.ReadFile(headPath())
	if os.IsNotExist(err) {
		return &HeadState{}, nil
	}
	if err != nil {
		return nil, fmt.Errorf("read head.json: %w", err)
	}
	var h HeadState
	if err := json.Unmarshal(data, &h); err != nil {
		return nil, fmt.Errorf("parse head.json: %w", err)
	}
	return &h, nil
}

// SaveHead atomically writes head.json.
func SaveHead(h *HeadState) error {
	data, err := json.MarshalIndent(h, "", "  ")
	if err != nil {
		return err
	}
	return writeRecoveryFile(headPath(), data)
}

const snapMagicV2 = "XNOTE-SNAP-2"

// SnapshotHeader is the decoded in-memory representation of a snapshot header.
// V2 (hybrid) headers insert kem_len + KEM ciphertext after the wrapped DEK.
type SnapshotHeader struct {
	Generation       uint64
	ParentCipherHash [32]byte
	WrappedDEK       []byte
	BaseNonce        [24]byte
	PQ               bool
	KemCT            []byte
	// HeaderBytes is the serialised header for use as AAD
	HeaderBytes []byte
}

// WriteHeader serialises the header to w and returns the raw header bytes.
func WriteHeader(w io.Writer, hdr *SnapshotHeader) ([]byte, error) {
	magic := snapMagic
	if hdr.PQ {
		magic = snapMagicV2
	}
	buf := make([]byte, 0, 12+8+32+2+len(hdr.WrappedDEK)+2+len(hdr.KemCT)+24)

	buf = append(buf, []byte(magic)...)
	var gen8 [8]byte
	binary.BigEndian.PutUint64(gen8[:], hdr.Generation)
	buf = append(buf, gen8[:]...)
	buf = append(buf, hdr.ParentCipherHash[:]...)
	var wdekLen [2]byte
	binary.BigEndian.PutUint16(wdekLen[:], uint16(len(hdr.WrappedDEK)))
	buf = append(buf, wdekLen[:]...)
	buf = append(buf, hdr.WrappedDEK...)
	if hdr.PQ {
		var kemLen [2]byte
		binary.BigEndian.PutUint16(kemLen[:], uint16(len(hdr.KemCT)))
		buf = append(buf, kemLen[:]...)
		buf = append(buf, hdr.KemCT...)
	}
	buf = append(buf, hdr.BaseNonce[:]...)

	_, err := w.Write(buf)
	return buf, err
}

// ReadHeader deserialises a snapshot header from r.
func ReadHeader(r io.Reader) (*SnapshotHeader, error) {
	magicBuf := make([]byte, len(snapMagic))
	if _, err := io.ReadFull(r, magicBuf); err != nil {
		return nil, fmt.Errorf("read magic: %w", err)
	}
	isPQ := false
	if string(magicBuf) == snapMagicV2 {
		isPQ = true
	} else if string(magicBuf) != snapMagic {
		return nil, fmt.Errorf("not an xnote snapshot (bad magic)")
	}

	var gen8 [8]byte
	if _, err := io.ReadFull(r, gen8[:]); err != nil {
		return nil, fmt.Errorf("read generation: %w", err)
	}
	gen := binary.BigEndian.Uint64(gen8[:])

	var pch [32]byte
	if _, err := io.ReadFull(r, pch[:]); err != nil {
		return nil, fmt.Errorf("read parent_cipher_hash: %w", err)
	}

	var wdekLen [2]byte
	if _, err := io.ReadFull(r, wdekLen[:]); err != nil {
		return nil, fmt.Errorf("read wdek_len: %w", err)
	}
	wdekN := binary.BigEndian.Uint16(wdekLen[:])
	if wdekN == 0 || wdekN > 256 {
		return nil, fmt.Errorf("bad wrapped_dek length %d", wdekN)
	}
	wdek := make([]byte, wdekN)
	if _, err := io.ReadFull(r, wdek); err != nil {
		return nil, fmt.Errorf("read wrapped_dek: %w", err)
	}

	var kemCT []byte
	var kemLen [2]byte
	if isPQ {
		if _, err := io.ReadFull(r, kemLen[:]); err != nil {
			return nil, fmt.Errorf("read kem_len: %w", err)
		}
		kemN := binary.BigEndian.Uint16(kemLen[:])
		if int(kemN) != mlkem1024CiphertextLen {
			return nil, fmt.Errorf("bad KEM ciphertext length %d", kemN)
		}
		kemCT = make([]byte, kemN)
		if _, err := io.ReadFull(r, kemCT); err != nil {
			return nil, fmt.Errorf("read kem_ct: %w", err)
		}
	}

	var baseNonce [24]byte
	if _, err := io.ReadFull(r, baseNonce[:]); err != nil {
		return nil, fmt.Errorf("read base_nonce: %w", err)
	}

	hdr := &SnapshotHeader{
		Generation:       gen,
		ParentCipherHash: pch,
		WrappedDEK:       wdek,
		BaseNonce:        baseNonce,
		PQ:               isPQ,
		KemCT:            kemCT,
	}

	// Reconstruct header bytes for AAD (same encoding as WriteHeader)
	magic := snapMagic
	if isPQ {
		magic = snapMagicV2
	}
	buf := make([]byte, 0, 12+8+32+2+int(wdekN)+2+len(kemCT)+24)
	buf = append(buf, []byte(magic)...)
	buf = append(buf, gen8[:]...)
	buf = append(buf, pch[:]...)
	buf = append(buf, wdekLen[:]...)
	buf = append(buf, wdek...)
	if isPQ {
		buf = append(buf, kemLen[:]...)
		buf = append(buf, kemCT...)
	}
	buf = append(buf, baseNonce[:]...)
	hdr.HeaderBytes = buf

	return hdr, nil
}

// SnapshotID returns a lexically-sortable snapshot ID: timestamp + generation
// counter. The generation suffix ensures uniqueness even within a single second;
// it is zero-padded to 20 digits (uint64 max) so ListSnapshots' string sort
// orders same-second snapshots numerically (unpadded, "...-g10" sorted before
// "...-g9", so restore-latest could pick an older snapshot).
func SnapshotID(generation uint64) string {
	ts := time.Now().UTC().Format("20060102T150405Z")
	return fmt.Sprintf("%s-g%020d", ts, generation)
}

// snapshotGenerationFromID parses both the current zero-padded generation
// suffix and legacy (pre-2.2.8) unpadded suffixes. Generation, not wall-clock
// timestamp, is the authoritative ordering: the system clock can move backward.
func snapshotGenerationFromID(id string) (uint64, error) {
	if len(id) < len("20060102T150405Z-g0") ||
		id[8] != 'T' || id[15] != 'Z' ||
		!strings.HasPrefix(id[16:], "-g") {
		return 0, fmt.Errorf("invalid snapshot ID %q", id)
	}
	ts := id[:16]
	if _, err := time.Parse("20060102T150405Z", ts); err != nil {
		return 0, fmt.Errorf("invalid snapshot ID timestamp %q: %w", id, err)
	}
	genText := id[18:]
	if len(genText) == 0 || len(genText) > 20 {
		return 0, fmt.Errorf("invalid snapshot generation in %q", id)
	}
	generation, err := strconv.ParseUint(genText, 10, 64)
	if err != nil {
		return 0, fmt.Errorf("invalid snapshot generation in %q: %w", id, err)
	}
	if generation == 0 {
		return 0, fmt.Errorf("snapshot generation must be positive in %q", id)
	}
	return generation, nil
}

func sortSnapshotIDsByGeneration(ids []string) {
	sort.Slice(ids, func(i, j int) bool {
		genI, errI := snapshotGenerationFromID(ids[i])
		genJ, errJ := snapshotGenerationFromID(ids[j])
		if errI != nil || errJ != nil || genI == genJ {
			return ids[i] < ids[j]
		}
		return genI < genJ
	})
}

// SnapshotPath returns the full path for a snapshot file.
func SnapshotPath(id string) string {
	return filepath.Join(snapshotsDir(), id+".xsnap")
}

// ListSnapshots returns all snapshot IDs, sorted oldest-first.
func ListSnapshots() ([]string, error) {
	entries, err := os.ReadDir(snapshotsDir())
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var ids []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".xsnap") {
			id := strings.TrimSuffix(e.Name(), ".xsnap")
			if _, err := snapshotGenerationFromID(id); err == nil {
				ids = append(ids, id)
			}
		}
	}
	sortSnapshotIDsByGeneration(ids)
	return ids, nil
}

// RotateSnapshots deletes oldest snapshots beyond keep.
func RotateSnapshots(keep int) error {
	ids, err := ListSnapshots()
	if err != nil {
		return err
	}
	for len(ids) > keep {
		old := ids[0]
		ids = ids[1:]
		if err := os.Remove(SnapshotPath(old)); err != nil && !os.IsNotExist(err) {
			return fmt.Errorf("remove old snapshot %s: %w", old, err)
		}
	}
	return nil
}
