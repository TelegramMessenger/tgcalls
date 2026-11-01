package main

// Listener-audio parts of the emulated broadcast server: one second of every speaker's Opus packets, combined into a
// single multichannel Ogg Opus stream (RFC 7845, channel mapping family 255) the way tgcalls' AudioStreamingPart
// reads it: one channel per speaker, the channel -> ssrc map and the video endpoint map as OpusTags comments
// (TG_META, ACTIVE_MASK, ENDPOINTS). The packets are the speakers' own (no re-encode); a slot with no packet carries
// an empty Opus frame.

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"sort"
	"strconv"
	"strings"
)

const (
	broadcastSlotsPerPart  = 50
	broadcastSlotSamples   = 960 // 20 ms at 48 kHz
	broadcastMaxChannels   = 8   // AudioStreamingPartInternal ends a part with more
	broadcastOggSerial     = 0x54474253
	opusEmptyFrameTOC      = 0xf8 // config 31 (CELT FB 20 ms), mono, code 0
	broadcastOpusVendor    = "tgcalls-testbench"
	oggHeaderTypeBOS  byte = 0x02
	oggHeaderTypeEOS  byte = 0x04
)

type audioPartMeta struct {
	SSRCs          []uint32
	VideoEndpoints map[int]string // channel index -> endpoint
}

var oggCRCTable = func() [256]uint32 {
	var table [256]uint32
	for i := range table {
		crc := uint32(i) << 24
		for j := 0; j < 8; j++ {
			if crc&0x80000000 != 0 {
				crc = (crc << 1) ^ 0x04c11db7
			} else {
				crc <<= 1
			}
		}
		table[i] = crc
	}
	return table
}()

// oggCRC is RFC 3533's page checksum: CRC-32, polynomial 0x04c11db7, initial value 0, unreflected.
func oggCRC(data []byte) uint32 {
	var crc uint32
	for _, b := range data {
		crc = (crc << 8) ^ oggCRCTable[byte(crc>>24)^b]
	}
	return crc
}

// opusFrameSamples is RFC 6716 Table 2: the frame size of a TOC configuration, in 48 kHz samples.
func opusFrameSamples(config int) int {
	switch {
	case config < 12: // SILK NB/MB/WB: 10, 20, 40, 60 ms
		return []int{480, 960, 1920, 2880}[config%4]
	case config < 16: // Hybrid SWB/FB: 10, 20 ms
		return []int{480, 960}[config%2]
	default: // CELT NB/WB/SWB/FB: 2.5, 5, 10, 20 ms
		return []int{120, 240, 480, 960}[config%4]
	}
}

// opusPacketDurationSamples reads an Opus packet's duration from its TOC (RFC 6716 §3.1).
func opusPacketDurationSamples(pkt []byte) (int, error) {
	if len(pkt) == 0 {
		return 0, errors.New("empty Opus packet")
	}
	frameSamples := opusFrameSamples(int(pkt[0] >> 3))
	switch pkt[0] & 3 {
	case 0:
		return frameSamples, nil
	case 1, 2:
		return 2 * frameSamples, nil
	default:
		if len(pkt) < 2 {
			return 0, errors.New("code 3 packet without a frame count")
		}
		count := int(pkt[1] & 0x3f)
		if count == 0 {
			return 0, errors.New("code 3 packet with zero frames")
		}
		return count * frameSamples, nil
	}
}

func appendOpusLength(out []byte, n int) []byte {
	if n < 252 {
		return append(out, byte(n))
	}
	first := 252 + (n & 3)
	return append(out, byte(first), byte((n-first)/4))
}

// readOpusLength reads a frame length (RFC 6716 §3.2.1) at b, returning the length and the bytes it used.
func readOpusLength(b []byte) (int, int, error) {
	if len(b) == 0 {
		return 0, 0, errors.New("truncated frame length")
	}
	if b[0] < 252 {
		return int(b[0]), 1, nil
	}
	if len(b) < 2 {
		return 0, 0, errors.New("truncated two-byte frame length")
	}
	return int(b[0]) + 4*int(b[1]), 2, nil
}

// selfDelimited converts an Opus packet to the self-delimiting framing of RFC 6716 Appendix B, which every stream
// of a multistream packet but the last uses: the size of the last frame (or, for CBR, of every frame) is added after
// the length fields the packet already has.
func selfDelimited(pkt []byte) ([]byte, error) {
	if len(pkt) == 0 {
		return nil, errors.New("empty Opus packet")
	}
	toc := pkt[0]
	switch toc & 3 {
	case 0:
		out := appendOpusLength([]byte{toc}, len(pkt)-1)
		return append(out, pkt[1:]...), nil
	case 1:
		body := len(pkt) - 1
		if body%2 != 0 {
			return nil, errors.New("code 1 packet with unequal frames")
		}
		out := appendOpusLength([]byte{toc}, body/2)
		return append(out, pkt[1:]...), nil
	case 2:
		n1, used, err := readOpusLength(pkt[1:])
		if err != nil {
			return nil, err
		}
		n2 := len(pkt) - 1 - used - n1
		if n2 < 0 {
			return nil, errors.New("code 2 packet shorter than its first frame")
		}
		out := append([]byte{toc}, pkt[1:1+used]...)
		out = appendOpusLength(out, n2)
		return append(out, pkt[1+used:]...), nil
	default:
		if len(pkt) < 2 {
			return nil, errors.New("code 3 packet without a frame count")
		}
		countByte := pkt[1]
		count := int(countByte & 0x3f)
		vbr := countByte&0x80 != 0
		hasPadding := countByte&0x40 != 0
		offset := 2
		padding := 0
		if hasPadding {
			for {
				if offset >= len(pkt) {
					return nil, errors.New("truncated padding length")
				}
				value := int(pkt[offset])
				offset++
				if value == 255 {
					padding += 254
				} else {
					padding += value
					break
				}
			}
		}
		sizes := 0
		if vbr {
			for i := 0; i < count-1; i++ {
				n, used, err := readOpusLength(pkt[offset:])
				if err != nil {
					return nil, err
				}
				sizes += n
				offset += used
			}
		}
		body := len(pkt) - offset - padding
		var added int
		if vbr {
			added = body - sizes
		} else {
			if count == 0 || body%count != 0 {
				return nil, errors.New("code 3 CBR packet with unequal frames")
			}
			added = body / count
		}
		if added < 0 {
			return nil, errors.New("code 3 packet shorter than its frames")
		}
		out := append([]byte(nil), pkt[:offset]...)
		out = appendOpusLength(out, added)
		return append(out, pkt[offset:]...), nil
	}
}

// A slot with no packet carries an encoded 20 ms CELT silence frame. Not a zero-length frame: libopus conceals one
// as a lost packet, extrapolating the speaker's last sound into the gap, and a speaker who sent nothing is silent.
func emptyOpusFrame() []byte {
	return []byte{opusEmptyFrameTOC, 0xff, 0xfe}
}

func emptyOpusFrameSelfDelimited() []byte {
	return []byte{opusEmptyFrameTOC, 0x02, 0xff, 0xfe}
}

func appendOggPage(out []byte, headerType byte, granule uint64, sequence uint32, packet []byte) []byte {
	var segments []byte
	remaining := len(packet)
	for remaining >= 255 {
		segments = append(segments, 255)
		remaining -= 255
	}
	segments = append(segments, byte(remaining))
	if len(segments) > 255 {
		panic("Ogg packet too large for one page")
	}
	page := make([]byte, 0, 27+len(segments)+len(packet))
	page = append(page, 'O', 'g', 'g', 'S', 0, headerType)
	page = binary.LittleEndian.AppendUint64(page, granule)
	page = binary.LittleEndian.AppendUint32(page, broadcastOggSerial)
	page = binary.LittleEndian.AppendUint32(page, sequence)
	page = append(page, 0, 0, 0, 0)
	page = append(page, byte(len(segments)))
	page = append(page, segments...)
	page = append(page, packet...)
	binary.LittleEndian.PutUint32(page[22:26], oggCRC(page))
	return append(out, page...)
}

// libavcodec/vorbis_data.c's ff_vorbis_channel_layout_offsets (FFmpeg 8.1.2).
var vorbisChannelLayoutOffsets = [8][8]int{
	{0},
	{0, 1},
	{0, 2, 1},
	{0, 1, 2, 3},
	{0, 2, 1, 3, 4},
	{0, 2, 1, 5, 3, 4},
	{0, 2, 1, 6, 5, 3, 4},
	{0, 2, 1, 7, 5, 6, 3, 4},
}

// opusHead is RFC 7845's identification header for one uncoupled stream per channel (family 255). Its mapping table
// undoes the Vorbis-to-FFmpeg reorder that FFmpeg's libopus wrapper (which tgcalls decodes parts with) applies to
// 3-8 channels whatever the family, so that decoder output channel c, the part's channel c, is stream c.
func opusHead(channels int) []byte {
	head := []byte("OpusHead")
	head = append(head, 1, byte(channels))
	head = binary.LittleEndian.AppendUint16(head, 0) // pre-skip
	head = binary.LittleEndian.AppendUint32(head, 48000)
	head = binary.LittleEndian.AppendUint16(head, 0) // output gain
	head = append(head, 255, byte(channels), 0)      // family 255, one uncoupled stream per channel
	mapping := make([]byte, channels)
	for c := 0; c < channels; c++ {
		if channels > 2 {
			mapping[vorbisChannelLayoutOffsets[channels-1][c]] = byte(c)
		} else {
			mapping[c] = byte(c)
		}
	}
	return append(head, mapping...)
}

func opusTags(meta audioPartMeta) []byte {
	var tgMeta bytes.Buffer
	n := uint32(len(meta.SSRCs))
	binary.Write(&tgMeta, binary.LittleEndian, n)
	binary.Write(&tgMeta, binary.LittleEndian, n)
	for c, ssrc := range meta.SSRCs {
		binary.Write(&tgMeta, binary.LittleEndian, uint32(0))
		binary.Write(&tgMeta, binary.LittleEndian, uint32(c))
		binary.Write(&tgMeta, binary.LittleEndian, ssrc)
	}

	var channels []int
	for c := range meta.VideoEndpoints {
		if c >= 0 && c < 32 {
			channels = append(channels, c)
		}
	}
	sort.Ints(channels)
	var mask uint32
	var endpoints []string
	for _, c := range channels {
		mask |= 1 << uint(c)
		endpoints = append(endpoints, meta.VideoEndpoints[c])
	}

	comments := []string{
		"TG_META=" + base64.StdEncoding.EncodeToString(tgMeta.Bytes()),
		"ACTIVE_MASK=" + strconv.FormatUint(uint64(mask), 10),
		"ENDPOINTS=" + strings.Join(endpoints, " "),
	}
	tags := []byte("OpusTags")
	tags = binary.LittleEndian.AppendUint32(tags, uint32(len(broadcastOpusVendor)))
	tags = append(tags, broadcastOpusVendor...)
	tags = binary.LittleEndian.AppendUint32(tags, uint32(len(comments)))
	for _, comment := range comments {
		tags = binary.LittleEndian.AppendUint32(tags, uint32(len(comment)))
		tags = append(tags, comment...)
	}
	return tags
}

// buildOpusPart writes one listener-audio part: slots[s][c] is channel c's packet for 20 ms slot s, nil for none.
func buildOpusPart(meta audioPartMeta, slots [][][]byte) ([]byte, error) {
	channels := len(meta.SSRCs)
	if channels == 0 || channels > broadcastMaxChannels {
		return nil, fmt.Errorf("listener audio needs 1-%d channels, got %d", broadcastMaxChannels, channels)
	}
	if len(slots) != broadcastSlotsPerPart {
		return nil, fmt.Errorf("listener audio needs %d slots, got %d", broadcastSlotsPerPart, len(slots))
	}

	out := appendOggPage(nil, oggHeaderTypeBOS, 0, 0, opusHead(channels))
	out = appendOggPage(out, 0, 0, 1, opusTags(meta))
	for s, slot := range slots {
		if len(slot) != channels {
			return nil, fmt.Errorf("slot %d has %d channels, want %d", s, len(slot), channels)
		}
		var packet []byte
		for c, channelPacket := range slot {
			last := c == channels-1
			if channelPacket == nil {
				if last {
					packet = append(packet, emptyOpusFrame()...)
				} else {
					packet = append(packet, emptyOpusFrameSelfDelimited()...)
				}
				continue
			}
			duration, err := opusPacketDurationSamples(channelPacket)
			if err != nil {
				return nil, fmt.Errorf("slot %d channel %d: %w", s, c, err)
			}
			if duration != broadcastSlotSamples {
				return nil, fmt.Errorf("slot %d channel %d: %d samples, want %d", s, c, duration, broadcastSlotSamples)
			}
			if last {
				packet = append(packet, channelPacket...)
			} else {
				framed, err := selfDelimited(channelPacket)
				if err != nil {
					return nil, fmt.Errorf("slot %d channel %d: %w", s, c, err)
				}
				packet = append(packet, framed...)
			}
		}
		headerType := byte(0)
		if s == len(slots)-1 {
			headerType = oggHeaderTypeEOS
		}
		out = appendOggPage(out, headerType, uint64((s+1)*broadcastSlotSamples), uint32(s+2), packet)
	}
	return out, nil
}
