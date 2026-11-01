package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
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
	slice, err := os.ReadFile(filepath.Join("testdata", "broadcast", fmt.Sprintf("video_q1_%02d.mp4", broadcastFixtureSliceAt(3))))
	if err != nil {
		t.Fatalf("read fixture: %v", err)
	}
	if !bytes.Equal(part[offset+8:], slice) {
		t.Fatalf("the part's body is not part 3's slice of quality 1")
	}
	unified := fixtures.unifiedPart(2, 3000)
	unifiedSlice, _ := os.ReadFile(filepath.Join("testdata", "broadcast", fmt.Sprintf("unified_q2_%02d.mp4", broadcastFixtureSliceAt(3))))
	if !bytes.HasSuffix(unified, unifiedSlice) {
		t.Fatalf("the unified part's body is not part 3's unified slice of quality 2")
	}
}

// A frame's counter tells only its slice, which repeats every 10 s; a listener further behind live must still be
// measurable. Every step of the order changes slice, and three consecutive parts' slices occur once per period.
func TestSliceOrderLocatesThreeConsecutiveParts(t *testing.T) {
	seen := map[[3]int]int64{}
	for n := int64(0); n < broadcastSlicePeriod; n++ {
		a, b, c := broadcastFixtureSliceAt(n-2), broadcastFixtureSliceAt(n-1), broadcastFixtureSliceAt(n)
		if c < 0 || c >= broadcastFixtureSlices {
			t.Fatalf("part %d: slice %d", n, c)
		}
		if a == b || b == c {
			t.Fatalf("parts %d..%d repeat a slice: %d %d %d", n-2, n, a, b, c)
		}
		key := [3]int{a, b, c}
		if previous, ok := seen[key]; ok {
			t.Fatalf("parts %d and %d both end the slices %v", previous, n, key)
		}
		seen[key] = n
	}
	for _, n := range []int64{-1, 5, 1_700_000_003} {
		if broadcastFixtureSliceAt(n) != broadcastFixtureSliceAt(n+broadcastSlicePeriod) {
			t.Fatalf("part %d: the order does not repeat after %d parts", n, broadcastSlicePeriod)
		}
	}
}

func TestPartShowsTheSliceOfItsTimestamp(t *testing.T) {
	fixtures := loadTestFixtures(t)
	for _, tsMs := range []int64{0, 9000, 10000, 1_700_000_003_000} {
		slice := broadcastFixtureSliceAt(floorDiv64(tsMs, 1000))
		want, _ := os.ReadFile(filepath.Join("testdata", "broadcast", fmt.Sprintf("video_q0_%02d.mp4", slice)))
		if !bytes.HasSuffix(fixtures.videoPart("e", 0, tsMs), want) {
			t.Fatalf("tsMs %d: not slice %d", tsMs, slice)
		}
		unified, _ := os.ReadFile(filepath.Join("testdata", "broadcast", fmt.Sprintf("unified_q1_%02d.mp4", slice)))
		if !bytes.HasSuffix(fixtures.unifiedPart(1, tsMs), unified) {
			t.Fatalf("tsMs %d: unified not slice %d", tsMs, slice)
		}
	}
}

func TestMissingFixturesIsAnError(t *testing.T) {
	if _, err := loadBroadcastFixtures(t.TempDir()); err == nil {
		t.Fatalf("an empty directory loaded")
	}
}
