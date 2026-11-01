package main

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// readTLString reads a TL string as tgcalls' VideoStreamingPart does, except that it takes the 3-byte length whole.
func readTLString(t *testing.T, data []byte, offset int) (string, int) {
	if data[offset] == 254 {
		length := int(data[offset+1]) | int(data[offset+2])<<8 | int(data[offset+3])<<16
		padding := (4 - length%4) % 4
		return string(data[offset+4 : offset+4+length]), offset + 4 + length + padding
	}
	length := int(data[offset])
	padding := (4 - (length+1)%4) % 4
	return string(data[offset+1 : offset+1+length]), offset + 1 + length + padding
}

func TestTLStringPadding(t *testing.T) {
	for _, length := range []int{0, 2, 3, 253, 254, 300} {
		s := string(bytes.Repeat([]byte{'e'}, length))
		encoded := encodeTLString(s)
		if len(encoded)%4 != 0 {
			t.Fatalf("length %d: encoded size %d not a multiple of 4", length, len(encoded))
		}
		decoded, end := readTLString(t, encoded, 0)
		if decoded != s || end != len(encoded) {
			t.Fatalf("length %d: decoded %d bytes, ended at %d of %d", length, len(decoded), end, len(encoded))
		}
	}
}

func loadTestFixtures(t *testing.T) *broadcastFixtures {
	fixtures, err := loadBroadcastFixtures(filepath.Join("testdata", "broadcast"))
	if err != nil {
		t.Fatalf("loadBroadcastFixtures: %v", err)
	}
	return fixtures
}

func TestVideoPartHeader(t *testing.T) {
	fixtures := loadTestFixtures(t)
	part := fixtures.videoPart("ep-b", 1, 3000)
	if binary.LittleEndian.Uint32(part[0:4]) != 0xa12e810d {
		t.Fatalf("signature %x", part[0:4])
	}
	container, offset := readTLString(t, part, 4)
	if container != "mp4" {
		t.Fatalf("container %q", container)
	}
	activeMask := binary.LittleEndian.Uint32(part[offset:])
	eventCount := binary.LittleEndian.Uint32(part[offset+4:])
	eventOffset := binary.LittleEndian.Uint32(part[offset+8:])
	if activeMask != 0 || eventCount != 1 || eventOffset != 0 {
		t.Fatalf("mask %d events %d offset %d", activeMask, eventCount, eventOffset)
	}
	endpoint, offset := readTLString(t, part, offset+12)
	if endpoint != "ep-b" {
		t.Fatalf("endpoint %q", endpoint)
	}
	rotation := binary.LittleEndian.Uint32(part[offset:])
	extra := binary.LittleEndian.Uint32(part[offset+4:])
	if rotation != 0 || extra != 0 {
		t.Fatalf("rotation %d extra %d", rotation, extra)
	}
	slice, err := os.ReadFile(filepath.Join("testdata", "broadcast", "video_q1_03.mp4"))
	if err != nil {
		t.Fatalf("read fixture: %v", err)
	}
	if !bytes.Equal(part[offset+8:], slice) {
		t.Fatalf("the part's body is not slice 3 of quality 1")
	}
	unified := fixtures.unifiedPart(2, 3000)
	unifiedSlice, _ := os.ReadFile(filepath.Join("testdata", "broadcast", "unified_q2_03.mp4"))
	if !bytes.HasSuffix(unified, unifiedSlice) {
		t.Fatalf("the unified part's body is not unified slice 3 of quality 2")
	}
}

func TestSliceIndexWraps(t *testing.T) {
	fixtures := loadTestFixtures(t)
	for _, c := range []struct {
		tsMs  int64
		slice string
	}{{9000, "video_q0_09.mp4"}, {10000, "video_q0_00.mp4"}, {1_700_000_003_000, "video_q0_03.mp4"}} {
		want, _ := os.ReadFile(filepath.Join("testdata", "broadcast", c.slice))
		if !bytes.HasSuffix(fixtures.videoPart("e", 0, c.tsMs), want) {
			t.Fatalf("tsMs %d: not %s", c.tsMs, c.slice)
		}
	}
}

func TestMissingFixturesIsAnError(t *testing.T) {
	if _, err := loadBroadcastFixtures(t.TempDir()); err == nil {
		t.Fatalf("an empty directory loaded")
	}
}
