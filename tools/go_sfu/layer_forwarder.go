package main

import "sync"

// layerForwarder keeps what one receiver gets from one sender a single continuous RTP stream while the SFU moves it
// between the sender's simulcast layers. The SFU rewrites every layer to the sender's first SSRC (the receiver's sink
// sits on it), but each layer has its own sequence space and its own random timestamp base: forwarded as they are, a
// switch to a layer whose timestamps run behind makes the receiver drop every frame as older than the last one it
// decoded, keyframes included, for the rest of the call. So on the first packet of a new layer the forwarder sets
// offsets that continue the sequence numbers and timestamps, and applies them to every packet after.
//
// RTX packets get their own sequence continuity, and their original sequence number (OSN, the first two payload bytes)
// is rewritten with the media offset of the same layer, so a retransmission repairs the packet the receiver saw.
type layerForwarder struct {
	mu sync.Mutex

	started   bool
	layer     int
	lastSeq   uint16
	lastTs    uint32
	seqOffset uint16
	tsOffset  uint32
	// The media sequence offset of each layer, for the OSNs of its RTX packets.
	seqOffsetByLayer map[int]uint16

	rtxStarted   bool
	rtxLayer     int
	lastRtxSeq   uint16
	rtxSeqOffset uint16
}

// A new layer's first frame continues one frame interval (at 90 kHz, 30 fps) after the last forwarded timestamp.
const layerSwitchTimestampStep = 3000

func rtpSeq(pkt []byte) uint16 { return uint16(pkt[2])<<8 | uint16(pkt[3]) }

func rtpTs(pkt []byte) uint32 {
	return uint32(pkt[4])<<24 | uint32(pkt[5])<<16 | uint32(pkt[6])<<8 | uint32(pkt[7])
}

func setRtpSeq(pkt []byte, seq uint16) {
	pkt[2] = byte(seq >> 8)
	pkt[3] = byte(seq)
}

func setRtpTs(pkt []byte, ts uint32) {
	pkt[4] = byte(ts >> 24)
	pkt[5] = byte(ts >> 16)
	pkt[6] = byte(ts >> 8)
	pkt[7] = byte(ts)
}

// rtpPayloadOffset returns where the payload starts (after CSRCs and the header extension), or -1.
func rtpPayloadOffset(pkt []byte) int {
	if len(pkt) < 12 {
		return -1
	}
	offset := 12 + 4*int(pkt[0]&0x0f)
	if pkt[0]&0x10 != 0 {
		if len(pkt) < offset+4 {
			return -1
		}
		extensionWords := int(pkt[offset+2])<<8 | int(pkt[offset+3])
		offset += 4 + 4*extensionWords
	}
	if offset > len(pkt) {
		return -1
	}
	return offset
}

// translateMedia rewrites a media packet of `layer` in place (the caller forwards a copy).
func (f *layerForwarder) translateMedia(pkt []byte, layer int) {
	if len(pkt) < 12 {
		return
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	seq := rtpSeq(pkt)
	ts := rtpTs(pkt)
	if !f.started {
		f.started = true
		f.layer = layer
		f.seqOffset = 0
		f.tsOffset = 0
		f.lastSeq = seq
		f.lastTs = ts
	} else if layer != f.layer {
		f.layer = layer
		f.seqOffset = f.lastSeq + 1 - seq
		f.tsOffset = f.lastTs + layerSwitchTimestampStep - ts
	}
	if f.seqOffsetByLayer == nil {
		f.seqOffsetByLayer = make(map[int]uint16)
	}
	f.seqOffsetByLayer[layer] = f.seqOffset
	outSeq := seq + f.seqOffset
	outTs := ts + f.tsOffset
	setRtpSeq(pkt, outSeq)
	setRtpTs(pkt, outTs)
	if int16(outSeq-f.lastSeq) > 0 {
		f.lastSeq = outSeq
	}
	if int32(outTs-f.lastTs) > 0 {
		f.lastTs = outTs
	}
}

// translateRtx rewrites an RTX packet of `layer` in place: its sequence number continues the forwarded RTX stream, its
// timestamp gets the media offset, and its OSN the media sequence offset of `layer`.
func (f *layerForwarder) translateRtx(pkt []byte, layer int) {
	if len(pkt) < 12 {
		return
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	seq := rtpSeq(pkt)
	if !f.rtxStarted {
		f.rtxStarted = true
		f.rtxLayer = layer
		f.rtxSeqOffset = 0
		f.lastRtxSeq = seq
	} else if layer != f.rtxLayer {
		f.rtxLayer = layer
		f.rtxSeqOffset = f.lastRtxSeq + 1 - seq
	}
	outSeq := seq + f.rtxSeqOffset
	setRtpSeq(pkt, outSeq)
	if int16(outSeq-f.lastRtxSeq) > 0 {
		f.lastRtxSeq = outSeq
	}
	if f.started && layer == f.layer {
		setRtpTs(pkt, rtpTs(pkt)+f.tsOffset)
	}
	mediaOffset, ok := f.seqOffsetByLayer[layer]
	if !ok {
		return
	}
	payload := rtpPayloadOffset(pkt)
	if payload < 0 || len(pkt) < payload+2 {
		return // padding only
	}
	osn := uint16(pkt[payload])<<8 | uint16(pkt[payload+1])
	osn += mediaOffset
	pkt[payload] = byte(osn >> 8)
	pkt[payload+1] = byte(osn)
}
