package main

import (
	"encoding/binary"
	"testing"
)

// fakeTranscoder stands in for libopus: a "decoded" packet is its duration (from the TOC) of samples equal to the
// packet's second byte, and an "encoded" 20 ms frame records the first and last sample it was given.
type fakeTranscoder struct{}

type fakeDecoder struct{}
type fakeEncoder struct{}

func (fakeTranscoder) newDecoder() (audioDecoder, error) { return fakeDecoder{}, nil }
func (fakeTranscoder) newEncoder() (audioEncoder, error) { return fakeEncoder{}, nil }

func (fakeDecoder) decode(pkt []byte) ([]int16, error) {
	n, err := opusPacketDurationSamples(pkt)
	if err != nil {
		return nil, err
	}
	pcm := make([]int16, n)
	for i := range pcm {
		pcm[i] = int16(pkt[1])
	}
	return pcm, nil
}
func (fakeDecoder) close() {}

func (fakeEncoder) encode(pcm []int16) ([]byte, error) {
	return []byte{opusEmptyFrameTOC, byte(pcm[0]), byte(pcm[len(pcm)-1])}, nil
}
func (fakeEncoder) close() {}

const silk60msTOC = 3 << 3 // config 3: SILK NB 60 ms, code 0

func rtpPacket(ssrc uint32, timestamp uint32, payload []byte) []byte {
	pkt := make([]byte, 12, 12+len(payload))
	pkt[0] = 0x80
	pkt[1] = 111
	binary.BigEndian.PutUint32(pkt[4:8], timestamp)
	binary.BigEndian.PutUint32(pkt[8:12], ssrc)
	return append(pkt, payload...)
}

func newTestBroadcastAudio() *broadcastAudio {
	return newBroadcastAudio(fakeTranscoder{}, 30000)
}

// Sends `count` 60 ms packets for one speaker, starting at RTP time 0 and arrival `startMs`, tagged with i+1.
func sendSpeaker(b *broadcastAudio, ssrc uint32, participantID int, startMs int64, count int, skip map[int]bool, jitter func(int) int64) {
	for i := 0; i < count; i++ {
		if skip[i] {
			continue
		}
		arrival := startMs + int64(i)*60
		if jitter != nil {
			arrival += jitter(i)
		}
		b.addPacket(ssrc, participantID, arrival, rtpPacket(ssrc, uint32(i*2880), []byte{silk60msTOC, byte(i + 1), 0}))
	}
}

func TestParseRTPWithExtensionAndPadding(t *testing.T) {
	pkt := []byte{0x80 | 0x20 | 0x10 | 0x01, 111, 0, 1, 0x12, 0x34, 0x56, 0x78, 0, 0, 0, 9}
	pkt = append(pkt, 0, 0, 0, 7)                // one CSRC
	pkt = append(pkt, 0xbe, 0xde, 0, 1)          // extension header, one 32-bit word
	pkt = append(pkt, 0x10, 0xaa, 0, 0)          // the extension word
	pkt = append(pkt, 'a', 'b', 'c')             // payload
	pkt = append(pkt, 0, 0, 3)                   // padding, last byte = padding length
	ts, payload, ok := parseRTP(pkt)
	if !ok || ts != 0x12345678 || string(payload) != "abc" {
		t.Fatalf("parseRTP: ok %v ts %x payload %q", ok, ts, payload)
	}
	if _, _, ok := parseRTP(pkt[:11]); ok {
		t.Fatalf("short packet accepted")
	}
	if _, _, ok := parseRTP([]byte{0x40, 111, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}); ok {
		t.Fatalf("RTP version 1 accepted")
	}
}

func TestSlotsFromJitteredArrivals(t *testing.T) {
	b := newTestBroadcastAudio()
	// Anchored on a 20 ms boundary; arrivals jitter by up to ±8 ms, which must not move anything: the timeline
	// follows the RTP timestamps once anchored.
	jitter := func(i int) int64 { return []int64{0, 8, -8, 5, -3, 7, -6}[i%7] }
	sendSpeaker(b, 1001, 0, 100_000, 40, nil, jitter)
	meta, slots, err := b.slotsFor(101_000)
	if err != nil {
		t.Fatalf("slotsFor: %v", err)
	}
	if len(meta.SSRCs) != 1 || meta.SSRCs[0] != 1001 {
		t.Fatalf("SSRCs %v", meta.SSRCs)
	}
	for s := 0; s < 50; s++ {
		frame := slots[s][0]
		// Slot s starts 1000 + 20s ms after the anchor: inside packet (1000 + 20s) / 60, tagged +1.
		want := byte((1000+20*s)/60 + 1)
		if frame == nil || frame[1] != want {
			t.Fatalf("slot %d: %v, want tag %d", s, frame, want)
		}
	}
}

func TestGapsBecomeEmpty(t *testing.T) {
	b := newTestBroadcastAudio()
	// Packets 20 and 21 (1200-1320 ms after the anchor) are missing: slots 10-15 of the part at +1000 ms.
	sendSpeaker(b, 1001, 0, 100_000, 40, map[int]bool{20: true, 21: true}, nil)
	_, slots, err := b.slotsFor(101_000)
	if err != nil {
		t.Fatalf("slotsFor: %v", err)
	}
	for s := 0; s < 50; s++ {
		empty := s >= 10 && s < 16
		if (slots[s][0] == nil) != empty {
			t.Fatalf("slot %d: %v (empty expected %v)", s, slots[s][0], empty)
		}
	}
}

func TestOnlyFirstEightSpeakers(t *testing.T) {
	b := newTestBroadcastAudio()
	for p := 0; p < 10; p++ {
		b.participantJoined(p)
	}
	// Audio arrives in reverse order of joining: channels still follow join order.
	for p := 9; p >= 0; p-- {
		sendSpeaker(b, uint32(2000+p), p, 100_000, 40, nil, nil)
	}
	meta, slots, err := b.slotsFor(101_000)
	if err != nil {
		t.Fatalf("slotsFor: %v", err)
	}
	if len(meta.SSRCs) != 8 || len(slots[0]) != 8 {
		t.Fatalf("%d channels, want 8", len(meta.SSRCs))
	}
	participants := b.channelParticipants(101_000)
	for c := 0; c < 8; c++ {
		if meta.SSRCs[c] != uint32(2000+c) || participants[c] != c {
			t.Fatalf("channel %d: ssrc %d participant %d", c, meta.SSRCs[c], participants[c])
		}
	}
	part, err := b.part(101_000, map[int]string{1: "ep-1", 9: "ep-9"})
	if err != nil || len(part) == 0 {
		t.Fatalf("part: %v", err)
	}
	meta, _, _ = b.slotsFor(101_000)
	if endpoints := b.videoEndpointsByChannel(meta, map[int]string{1: "ep-1", 9: "ep-9"}); len(endpoints) != 1 || endpoints[1] != "ep-1" {
		t.Fatalf("video endpoints by channel %v (participant 9 has no channel)", endpoints)
	}
}

func TestLeaverStaysEmptyUntilNextPart(t *testing.T) {
	b := newTestBroadcastAudio()
	b.participantJoined(0)
	b.participantJoined(1)
	sendSpeaker(b, 1000, 0, 100_000, 60, nil, nil)
	// Participant 1 sends until 1500 ms after the anchor (packet 25 ends there) and leaves then.
	sendSpeaker(b, 1001, 1, 100_000, 25, nil, nil)
	b.participantLeft(1, 101_500)
	meta, slots, err := b.slotsFor(101_000)
	if err != nil {
		t.Fatalf("slotsFor: %v", err)
	}
	if len(meta.SSRCs) != 2 || meta.SSRCs[1] != 1001 {
		t.Fatalf("part with the leaver: SSRCs %v", meta.SSRCs)
	}
	for s := 0; s < 50; s++ {
		if (slots[s][1] == nil) != (s >= 25) {
			t.Fatalf("leaver slot %d: %v", s, slots[s][1])
		}
	}
	meta, _, err = b.slotsFor(102_000)
	if err != nil || len(meta.SSRCs) != 1 || meta.SSRCs[0] != 1000 {
		t.Fatalf("next part: SSRCs %v err %v", meta.SSRCs, err)
	}
}

func TestNoSpeakersIsAnError(t *testing.T) {
	if _, _, err := newTestBroadcastAudio().slotsFor(101_000); err == nil {
		t.Fatalf("a part with no speakers built")
	}
}
