package main

import (
	"encoding/json"
	"testing"
)

// The join response lists AV1 (106, RTX 107) only when the SFU is told to.
func TestJoinResponseListsAv1(t *testing.T) {
	without := videoPayloadTypesJSON(false)
	for _, pt := range without {
		if pt.Name == "AV1" {
			t.Fatalf("AV1 listed without SetListAv1")
		}
	}

	with := videoPayloadTypesJSON(true)
	var av1, rtx *videoPayloadTypeJSON
	for i := range with {
		switch {
		case with[i].Name == "AV1":
			av1 = &with[i]
		case with[i].Name == "rtx" && with[i].Parameters["apt"] == "106":
			rtx = &with[i]
		}
	}
	if av1 == nil || av1.ID != 106 || av1.Clockrate != 90000 {
		t.Fatalf("AV1 entry: %+v", av1)
	}
	if av1.Parameters["profile"] != "0" || av1.Parameters["level-idx"] != "5" || av1.Parameters["tier"] != "0" {
		t.Fatalf("AV1 parameters: %v", av1.Parameters)
	}
	if rtx == nil || rtx.ID != 107 {
		t.Fatalf("AV1 RTX entry: %+v", rtx)
	}
	// H264 is still listed first.
	if with[0].Name != "H264" {
		t.Fatalf("first entry %q", with[0].Name)
	}
}

func videoRtpPacket(pt byte, seq uint16, ts uint32) []byte {
	pkt := make([]byte, 20)
	pkt[0] = 0x80
	pkt[1] = pt
	pkt[2] = byte(seq >> 8)
	pkt[3] = byte(seq)
	pkt[4] = byte(ts >> 24)
	pkt[5] = byte(ts >> 16)
	pkt[6] = byte(ts >> 8)
	pkt[7] = byte(ts)
	return pkt
}

// Packets and frames (distinct RTP timestamps) per sender, layer and payload type.
func TestPayloadTypeStatsPerLayer(t *testing.T) {
	s := NewSFU()
	defer s.Destroy()

	// Layer 0: H264, 3 frames of 2 packets. Layer 1: AV1, 2 frames of 3 packets, and one RTX packet.
	seq := uint16(1)
	for frame := uint32(0); frame < 3; frame++ {
		for i := 0; i < 2; i++ {
			s.recordPayloadType(7, 0, videoRtpPacket(104, seq, 1000+frame*3000))
			seq++
		}
	}
	for frame := uint32(0); frame < 2; frame++ {
		for i := 0; i < 3; i++ {
			s.recordPayloadType(7, 1, videoRtpPacket(106, seq, 5000+frame*3000))
			seq++
		}
	}
	s.recordPayloadType(7, 1, videoRtpPacket(107, seq, 5000))
	// Another sender is kept apart.
	s.recordPayloadType(8, 0, videoRtpPacket(104, 1, 1000))

	var stats map[string]map[string]payloadTypeStatsJSON
	if err := json.Unmarshal([]byte(s.QueryPayloadTypeStats(7)), &stats); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	check := func(layer, pt string, packets, frames int64) {
		t.Helper()
		got, ok := stats[layer][pt]
		if !ok {
			t.Fatalf("layer %s pt %s missing: %v", layer, pt, stats)
		}
		if got.Packets != packets || got.Frames != frames {
			t.Fatalf("layer %s pt %s: %+v, want %d packets, %d frames", layer, pt, got, packets, frames)
		}
	}
	check("0", "104", 6, 3)
	check("1", "106", 6, 2)
	check("1", "107", 1, 1)
	if len(stats["0"]) != 1 || len(stats["1"]) != 2 {
		t.Fatalf("unexpected payload types: %v", stats)
	}
}
