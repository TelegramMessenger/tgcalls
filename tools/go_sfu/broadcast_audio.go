package main

// Listener audio of the emulated broadcast server, built from the speakers' live RTP.
//
// tgcalls' group engine sends 60 ms Opus frames, which cannot be cut into 1000 ms parts, so the server does what the
// real one must: it decodes each speaker onto a 48 kHz timeline taken from the RTP timestamps, and re-encodes each
// part's 20 ms slots per channel. A slot nothing was decoded into is empty.

import (
	"errors"
	"fmt"
	"sort"
	"sync"
)

const (
	broadcastSlotMs          = 20
	broadcastDefaultRetainMs = 30000
)

type audioDecoder interface {
	decode(pkt []byte) ([]int16, error)
	close()
}

type audioEncoder interface {
	// encode turns 20 ms of 48 kHz mono PCM into one Opus packet.
	encode(pcm []int16) ([]byte, error)
	close()
}

type broadcastTranscoder interface {
	newDecoder() (audioDecoder, error)
	newEncoder() (audioEncoder, error)
}

type pcmBlock struct {
	samples [broadcastSlotSamples]int16
	filled  int
}

type broadcastSpeaker struct {
	ssrc          uint32
	participantID int
	joinSeq       int
	anchorRtp     uint32
	anchorSample  int64 // the first packet's arrival, in 48 kHz samples
	firstSlot     int64
	decoder       audioDecoder
	blocks        map[int64]*pcmBlock // absolute 20 ms slot -> decoded samples
	seen          map[uint32]int64    // RTP timestamp -> arrival, for duplicates
}

type broadcastAudio struct {
	mu           sync.Mutex
	transcoder   broadcastTranscoder
	retainMs     int64
	nextJoinSeq  int
	joinSeq      map[int]int   // participant -> join order
	leftAtMs     map[int]int64 // participant -> when it left
	speakers     map[uint32]*broadcastSpeaker
	latestMs     int64
	packetsSince int
}

func newBroadcastAudio(transcoder broadcastTranscoder, retainMs int64) *broadcastAudio {
	return &broadcastAudio{
		transcoder: transcoder,
		retainMs:   retainMs,
		joinSeq:    map[int]int{},
		leftAtMs:   map[int]int64{},
		speakers:   map[uint32]*broadcastSpeaker{},
	}
}

// parseRTP returns an RTP packet's timestamp and payload (RFC 3550: CSRCs, header extension, padding).
func parseRTP(pkt []byte) (uint32, []byte, bool) {
	if len(pkt) < 12 || pkt[0]>>6 != 2 {
		return 0, nil, false
	}
	offset := 12 + 4*int(pkt[0]&0x0f)
	if pkt[0]&0x10 != 0 {
		if len(pkt) < offset+4 {
			return 0, nil, false
		}
		offset += 4 + 4*(int(pkt[offset+2])<<8|int(pkt[offset+3]))
	}
	end := len(pkt)
	if pkt[0]&0x20 != 0 {
		if end == 0 {
			return 0, nil, false
		}
		end -= int(pkt[end-1])
	}
	if offset > end {
		return 0, nil, false
	}
	timestamp := uint32(pkt[4])<<24 | uint32(pkt[5])<<16 | uint32(pkt[6])<<8 | uint32(pkt[7])
	return timestamp, pkt[offset:end], true
}

func floorDiv64(value, divisor int64) int64 {
	quotient := value / divisor
	if value%divisor != 0 && (value < 0) != (divisor < 0) {
		quotient--
	}
	return quotient
}

func (b *broadcastAudio) participantJoined(participantID int) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if _, ok := b.joinSeq[participantID]; !ok {
		b.joinSeq[participantID] = b.nextJoinSeq
		b.nextJoinSeq++
	}
	delete(b.leftAtMs, participantID)
}

func (b *broadcastAudio) participantLeft(participantID int, atMs int64) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.leftAtMs[participantID] = atMs
}

// addPacket takes one audio RTP packet from a speaker, as the SFU received it.
func (b *broadcastAudio) addPacket(ssrc uint32, participantID int, arrivalMs int64, rtp []byte) {
	if b.transcoder == nil {
		return
	}
	timestamp, payload, ok := parseRTP(rtp)
	if !ok || len(payload) == 0 {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()

	if _, ok := b.joinSeq[participantID]; !ok {
		b.joinSeq[participantID] = b.nextJoinSeq
		b.nextJoinSeq++
	}
	speaker := b.speakers[ssrc]
	if speaker == nil {
		decoder, err := b.transcoder.newDecoder()
		if err != nil {
			return
		}
		speaker = &broadcastSpeaker{
			ssrc:          ssrc,
			participantID: participantID,
			joinSeq:       b.joinSeq[participantID],
			anchorRtp:     timestamp,
			anchorSample:  arrivalMs * 48,
			firstSlot:     floorDiv64(arrivalMs*48, broadcastSlotSamples),
			decoder:       decoder,
			blocks:        map[int64]*pcmBlock{},
			seen:          map[uint32]int64{},
		}
		b.speakers[ssrc] = speaker
	}
	if _, duplicate := speaker.seen[timestamp]; duplicate {
		return
	}
	speaker.seen[timestamp] = arrivalMs

	pcm, err := speaker.decoder.decode(payload)
	if err != nil {
		return
	}
	start := speaker.anchorSample + int64(int32(timestamp-speaker.anchorRtp))
	for i, sample := range pcm {
		absolute := start + int64(i)
		slot := floorDiv64(absolute, broadcastSlotSamples)
		block := speaker.blocks[slot]
		if block == nil {
			block = &pcmBlock{}
			speaker.blocks[slot] = block
		}
		block.samples[absolute-slot*broadcastSlotSamples] = sample
		block.filled++
	}

	if arrivalMs > b.latestMs {
		b.latestMs = arrivalMs
	}
	b.packetsSince++
	if b.packetsSince >= 256 {
		b.packetsSince = 0
		b.pruneLocked()
	}
}

func (b *broadcastAudio) pruneLocked() {
	oldestMs := b.latestMs - b.retainMs
	oldestSlot := floorDiv64(oldestMs*48, broadcastSlotSamples)
	for _, speaker := range b.speakers {
		for slot := range speaker.blocks {
			if slot < oldestSlot {
				delete(speaker.blocks, slot)
			}
		}
		for timestamp, arrival := range speaker.seen {
			if arrival < oldestMs {
				delete(speaker.seen, timestamp)
			}
		}
	}
}

// channelSpeakersLocked: the speakers of the part at tsMs, by channel. A speaker has a channel when it has sent
// audio by the part's end and had not left by its start; the first 8 by join order get one.
func (b *broadcastAudio) channelSpeakersLocked(tsMs int64) []*broadcastSpeaker {
	endSlot := floorDiv64((tsMs+1000)*48, broadcastSlotSamples)
	var speakers []*broadcastSpeaker
	for _, speaker := range b.speakers {
		if speaker.firstSlot >= endSlot {
			continue
		}
		if leftAt, left := b.leftAtMs[speaker.participantID]; left && leftAt <= tsMs {
			continue
		}
		speakers = append(speakers, speaker)
	}
	sort.Slice(speakers, func(i, j int) bool {
		if speakers[i].joinSeq != speakers[j].joinSeq {
			return speakers[i].joinSeq < speakers[j].joinSeq
		}
		return speakers[i].ssrc < speakers[j].ssrc
	})
	if len(speakers) > broadcastMaxChannels {
		speakers = speakers[:broadcastMaxChannels]
	}
	return speakers
}

// channelParticipants lists the participants of the part at tsMs by channel index.
func (b *broadcastAudio) channelParticipants(tsMs int64) []int {
	b.mu.Lock()
	defer b.mu.Unlock()
	var result []int
	for _, speaker := range b.channelSpeakersLocked(tsMs) {
		result = append(result, speaker.participantID)
	}
	return result
}

// slotsFor re-encodes the part at tsMs: its channels and, per 20 ms slot and channel, an Opus packet or nil.
func (b *broadcastAudio) slotsFor(tsMs int64) (audioPartMeta, [][][]byte, error) {
	if b.transcoder == nil {
		return audioPartMeta{}, nil, errors.New("no Opus transcoder in this build")
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	speakers := b.channelSpeakersLocked(tsMs)
	if len(speakers) == 0 {
		return audioPartMeta{}, nil, errors.New("no speakers")
	}
	meta := audioPartMeta{VideoEndpoints: map[int]string{}}
	for _, speaker := range speakers {
		meta.SSRCs = append(meta.SSRCs, speaker.ssrc)
	}
	baseSlot := floorDiv64(tsMs*48, broadcastSlotSamples)
	slots := make([][][]byte, broadcastSlotsPerPart)
	for s := range slots {
		slots[s] = make([][]byte, len(speakers))
	}
	for c, speaker := range speakers {
		encoder, err := b.transcoder.newEncoder()
		if err != nil {
			return audioPartMeta{}, nil, err
		}
		for s := 0; s < broadcastSlotsPerPart; s++ {
			block := speaker.blocks[baseSlot+int64(s)]
			if block == nil || block.filled == 0 {
				continue
			}
			frame, err := encoder.encode(block.samples[:])
			if err != nil {
				encoder.close()
				return audioPartMeta{}, nil, fmt.Errorf("channel %d slot %d: %w", c, s, err)
			}
			slots[s][c] = frame
		}
		encoder.close()
	}
	return meta, slots, nil
}

// videoEndpointsByChannel maps the part's channels to the endpoints of the participants that send video.
func (b *broadcastAudio) videoEndpointsByChannel(meta audioPartMeta, endpointsByParticipant map[int]string) map[int]string {
	b.mu.Lock()
	defer b.mu.Unlock()
	result := map[int]string{}
	for c, ssrc := range meta.SSRCs {
		speaker := b.speakers[ssrc]
		if speaker == nil {
			continue
		}
		if endpoint, ok := endpointsByParticipant[speaker.participantID]; ok {
			result[c] = endpoint
		}
	}
	return result
}

// part builds the listener-audio part at tsMs; videoEndpoints maps participants to the endpoints they send video on.
func (b *broadcastAudio) part(tsMs int64, videoEndpoints map[int]string) ([]byte, error) {
	meta, slots, err := b.slotsFor(tsMs)
	if err != nil {
		return nil, err
	}
	meta.VideoEndpoints = b.videoEndpointsByChannel(meta, videoEndpoints)
	return buildOpusPart(meta, slots)
}
