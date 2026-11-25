package main

import "testing"

func fwdPacket(seq uint16, ts uint32, payload []byte) []byte {
	pkt := make([]byte, 12+len(payload))
	pkt[0] = 0x80
	pkt[1] = 104
	pkt[2] = byte(seq >> 8)
	pkt[3] = byte(seq)
	pkt[4] = byte(ts >> 24)
	pkt[5] = byte(ts >> 16)
	pkt[6] = byte(ts >> 8)
	pkt[7] = byte(ts)
	copy(pkt[12:], payload)
	return pkt
}

func seqOf(pkt []byte) uint16 { return uint16(pkt[2])<<8 | uint16(pkt[3]) }
func tsOf(pkt []byte) uint32 {
	return uint32(pkt[4])<<24 | uint32(pkt[5])<<16 | uint32(pkt[6])<<8 | uint32(pkt[7])
}

// One receiver's view of one sender stays one continuous stream while the SFU moves it between simulcast layers: each
// layer has its own sequence space and timestamp base, and a receiver drops frames that look older than the last.
func TestLayerForwarderKeepsSequenceAndTimestampContinuous(t *testing.T) {
	var f layerForwarder
	var lastSeq uint16
	var lastTs uint32
	forward := func(layer int, seq uint16, ts uint32) {
		t.Helper()
		pkt := fwdPacket(seq, ts, []byte{1, 2, 3})
		f.translateMedia(pkt, layer)
		if lastTs != 0 {
			if seqOf(pkt) != lastSeq+1 {
				t.Fatalf("layer %d seq %d: forwarded seq %d after %d", layer, seq, seqOf(pkt), lastSeq)
			}
			if int32(tsOf(pkt)-lastTs) < 0 {
				t.Fatalf("layer %d ts %d: forwarded ts %d went back from %d", layer, ts, tsOf(pkt), lastTs)
			}
		}
		lastSeq, lastTs = seqOf(pkt), tsOf(pkt)
	}

	// Layer 0, then layer 1 (another sequence space, a timestamp base far behind), then layer 0 again, then a layer
	// whose sequence numbers wrap.
	for i := 0; i < 5; i++ {
		forward(0, uint16(100+i), uint32(5_000_000+i*3000))
	}
	for i := 0; i < 5; i++ {
		forward(1, uint16(40_000+i), uint32(1_000+i*3000))
	}
	for i := 0; i < 5; i++ {
		forward(0, uint16(110+i), uint32(5_030_000+i*3000))
	}
	for i := 0; i < 5; i++ {
		forward(1, uint16(65_533+i), uint32(4_000_000_000+uint32(i)*3000))
	}

	// Packets of one layer keep their spacing (a frame's packets share a timestamp, seq gaps stay gaps).
	var g layerForwarder
	a := fwdPacket(10, 9000, nil)
	b := fwdPacket(12, 9000, nil)
	g.translateMedia(a, 0)
	g.translateMedia(b, 0)
	if seqOf(b)-seqOf(a) != 2 || tsOf(b) != tsOf(a) {
		t.Fatalf("spacing within a layer changed: %d,%d -> %d,%d", seqOf(a), tsOf(a), seqOf(b), tsOf(b))
	}
}

// An RTX packet carries the original sequence number (OSN) in its first two payload bytes: it is rewritten with the
// media offset of its layer, and the RTX stream's own sequence numbers stay continuous across switches.
func TestLayerForwarderRewritesRtx(t *testing.T) {
	var f layerForwarder
	media := fwdPacket(100, 9000, []byte{9})
	f.translateMedia(media, 0)
	switched := fwdPacket(40_000, 1_000, []byte{9})
	f.translateMedia(switched, 1)
	mediaOffset := seqOf(switched) - 40_000

	rtx := fwdPacket(700, 1_000, []byte{byte(40_000 >> 8), byte(40_000 & 0xff), 0xaa})
	f.translateRtx(rtx, 1)
	osn := uint16(rtx[12])<<8 | uint16(rtx[13])
	if osn != 40_000+mediaOffset {
		t.Fatalf("OSN %d, want %d", osn, 40_000+mediaOffset)
	}
	if rtx[14] != 0xaa {
		t.Fatalf("payload after the OSN changed")
	}
	firstRtxSeq := seqOf(rtx)

	// A later RTX packet from another layer continues the RTX sequence.
	f.translateMedia(fwdPacket(120, 9500, []byte{9}), 0)
	rtx2 := fwdPacket(30, 9500, []byte{0, 120, 0xbb})
	f.translateRtx(rtx2, 0)
	if seqOf(rtx2) != firstRtxSeq+1 {
		t.Fatalf("RTX seq %d after %d", seqOf(rtx2), firstRtxSeq)
	}
}
