package main

// Video and unified parts of the emulated broadcast server, from fixture slices (testdata/broadcast, made by
// make-broadcast-fixtures.sh): ten 1 s mp4 files per quality, one GOP each, looped over the live clock. A part is
// the video-part header the old engine's video part decoder read (signature, container, one event naming the endpoint
// and rotation) followed by the slice.

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
	// The endpoint a unified part names. A stream's player shows unified frames as "unified" whatever the part names
	// (the old engine did too); the empty string is what the real server's 32-byte header (the only
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

// The order parts show the slices in. The fixture loops every 10 s, so a frame's counter alone tells its media time
// modulo 10 s, and a listener further behind live would read as nearer. In this order every step changes slice and
// any two consecutive steps occur once per 81 parts: s_n = s_(n-1) + 1 + d_(n mod 81) (mod 10), d the de Bruijn
// sequence B(9, 2). Three consecutive parts therefore locate a part modulo 162 (s_(n+81) = s_n + 5 tells the two
// halves apart); the listener's metrics do (broadcast::locateFixturePart in tools/cli/broadcast_signals, which builds
// the same sequence).
const (
	broadcastSliceOrderSteps = 81
	broadcastSlicePeriod     = 2 * broadcastSliceOrderSteps
)

var broadcastSliceOrder = func() [broadcastSlicePeriod]int {
	differences := deBruijn(broadcastFixtureSlices-1, 2)
	var order [broadcastSlicePeriod]int
	for n := 1; n < broadcastSlicePeriod; n++ {
		order[n] = (order[n-1] + 1 + differences[n%broadcastSliceOrderSteps]) % broadcastFixtureSlices
	}
	return order
}()

// deBruijn is the de Bruijn sequence B(k, n), by the standard Lyndon-word construction.
func deBruijn(k, n int) []int {
	a := make([]int, k*n+1)
	var sequence []int
	var step func(t, p int)
	step = func(t, p int) {
		if t > n {
			if n%p == 0 {
				sequence = append(sequence, a[1:p+1]...)
			}
			return
		}
		a[t] = a[t-p]
		step(t+1, p)
		for j := a[t-p] + 1; j < k; j++ {
			a[t] = j
			step(t+1, t)
		}
	}
	step(1, 1)
	return sequence
}

func broadcastFixtureSliceAt(partIndex int64) int {
	position := partIndex % broadcastSlicePeriod
	if position < 0 {
		position += broadcastSlicePeriod
	}
	return broadcastSliceOrder[position]
}

func broadcastFixtureSlice(tsMs int64) int {
	return broadcastFixtureSliceAt(floorDiv64(tsMs, 1000))
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
