package main

// Video and unified parts of the emulated broadcast server, from fixture slices (testdata/broadcast, made by
// make-broadcast-fixtures.sh): ten 1 s mp4 files per quality, one GOP each, looped over the live clock. A part is
// the video-part header tgcalls' VideoStreamingPart reads (signature, container, one event naming the endpoint and
// rotation) followed by the slice.

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
)

const (
	broadcastVideoPartSignature = 0xa12e810d
	broadcastFixtureSlices      = 10
	broadcastQualities          = 3
	// The endpoint a unified part names. tgcalls' StreamingMediaContext shows unified frames on the sink registered
	// as "unified" whatever the part names; the empty string is what the real server's 32-byte header (the only
	// shape the app's Swift prototypes accept) holds.
	broadcastUnifiedEndpoint = ""
)

type broadcastFixtures struct {
	video   [broadcastQualities][broadcastFixtureSlices][]byte
	unified [broadcastQualities][broadcastFixtureSlices][]byte
}

func loadBroadcastFixtures(dir string) (*broadcastFixtures, error) {
	fixtures := &broadcastFixtures{}
	for quality := 0; quality < broadcastQualities; quality++ {
		for slice := 0; slice < broadcastFixtureSlices; slice++ {
			video, err := os.ReadFile(filepath.Join(dir, fmt.Sprintf("video_q%d_%02d.mp4", quality, slice)))
			if err != nil {
				return nil, err
			}
			unified, err := os.ReadFile(filepath.Join(dir, fmt.Sprintf("unified_q%d_%02d.mp4", quality, slice)))
			if err != nil {
				return nil, err
			}
			fixtures.video[quality][slice] = video
			fixtures.unified[quality][slice] = unified
		}
	}
	return fixtures, nil
}

// encodeTLString writes a TL string: a 1-byte length (or 254 and a 3-byte little-endian length), the bytes, and
// padding to a multiple of 4.
func encodeTLString(s string) []byte {
	var out []byte
	if len(s) < 254 {
		out = append(out, byte(len(s)))
	} else {
		out = append(out, 254, byte(len(s)), byte(len(s)>>8), byte(len(s)>>16))
	}
	out = append(out, s...)
	for len(out)%4 != 0 {
		out = append(out, 0)
	}
	return out
}

func broadcastFixtureSlice(tsMs int64) int {
	return int(floorDiv64(tsMs, 1000) % broadcastFixtureSlices)
}

func wrapVideoPart(endpoint string, slice []byte) []byte {
	part := binary.LittleEndian.AppendUint32(nil, broadcastVideoPartSignature)
	part = append(part, encodeTLString("mp4")...)
	part = binary.LittleEndian.AppendUint32(part, 0) // activeMask
	part = binary.LittleEndian.AppendUint32(part, 1) // eventCount
	part = binary.LittleEndian.AppendUint32(part, 0) // offset of the event's media in what follows the header
	part = append(part, encodeTLString(endpoint)...)
	part = binary.LittleEndian.AppendUint32(part, 0) // rotation
	part = binary.LittleEndian.AppendUint32(part, 0) // extra
	return append(part, slice...)
}

func clampQuality(quality int) int {
	if quality < 0 {
		return 0
	}
	if quality >= broadcastQualities {
		return broadcastQualities - 1
	}
	return quality
}

// videoPart is a listener's video part: the fixture slice for tsMs at `quality`, for `endpoint`.
func (f *broadcastFixtures) videoPart(endpoint string, quality int, tsMs int64) []byte {
	return wrapVideoPart(endpoint, f.video[clampQuality(quality)][broadcastFixtureSlice(tsMs)])
}

// unifiedPart is an RTMP stream's part (video and AAC) for tsMs at `quality`.
func (f *broadcastFixtures) unifiedPart(quality int, tsMs int64) []byte {
	return wrapVideoPart(broadcastUnifiedEndpoint, f.unified[clampQuality(quality)][broadcastFixtureSlice(tsMs)])
}
