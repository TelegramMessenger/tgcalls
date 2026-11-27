package main

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"strconv"
	"strings"
	"testing"
)

// Bitwise CRC-32 as RFC 3533 specifies it (polynomial 0x04c11db7, initial value 0, no reflection, no final xor),
// written independently of the table-driven one under test.
func referenceOggCRC(data []byte) uint32 {
	var crc uint32
	for _, b := range data {
		crc ^= uint32(b) << 24
		for i := 0; i < 8; i++ {
			if crc&0x80000000 != 0 {
				crc = (crc << 1) ^ 0x04c11db7
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

func TestOggCRCKnownVector(t *testing.T) {
	header := []byte{'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 1, 5}
	page := append(header, []byte("hello")...)
	if got, want := oggCRC(page), referenceOggCRC(page); got != want {
		t.Fatalf("oggCRC = %08x, reference %08x", got, want)
	}
	if oggCRC([]byte("OggS")) == 0 {
		t.Fatalf("oggCRC of non-empty data is zero")
	}
}

func TestTOCDurations(t *testing.T) {
	// RFC 6716 Table 2, frame sizes in 48 kHz samples per configuration.
	frameSamples := []int{
		480, 960, 1920, 2880, // SILK NB 10/20/40/60
		480, 960, 1920, 2880, // SILK MB
		480, 960, 1920, 2880, // SILK WB
		480, 960, // Hybrid SWB 10/20
		480, 960, // Hybrid FB
		120, 240, 480, 960, // CELT NB 2.5/5/10/20
		120, 240, 480, 960, // CELT WB
		120, 240, 480, 960, // CELT SWB
		120, 240, 480, 960, // CELT FB
	}
	for config := 0; config < 32; config++ {
		toc := byte(config << 3)
		cases := []struct {
			packet []byte
			frames int
		}{
			{[]byte{toc | 0, 1, 2}, 1},
			{[]byte{toc | 1, 1, 2}, 2},
			{[]byte{toc | 2, 1, 7, 8}, 2},
			{[]byte{toc | 3, 3, 1, 1, 1}, 3},
		}
		for _, c := range cases {
			got, err := opusPacketDurationSamples(c.packet)
			if err != nil {
				t.Fatalf("config %d code %d: %v", config, c.packet[0]&3, err)
			}
			if want := frameSamples[config] * c.frames; got != want {
				t.Fatalf("config %d code %d: %d samples, want %d", config, c.packet[0]&3, got, want)
			}
		}
	}
	if _, err := opusPacketDurationSamples(nil); err == nil {
		t.Fatalf("empty packet accepted")
	}
}

func decodeOpusLength(b []byte) (int, int) {
	if b[0] < 252 {
		return int(b[0]), 1
	}
	return int(b[0]) + 4*int(b[1]), 2
}

func TestSelfDelimitedLengths(t *testing.T) {
	frame := func(n int, fill byte) []byte { return bytes.Repeat([]byte{fill}, n) }
	toc := byte(31 << 3)

	// Code 0: TOC, N, frame.
	out, err := selfDelimited(append([]byte{toc}, frame(100, 0xaa)...))
	if err != nil || out[0] != toc || out[1] != 100 || !bytes.Equal(out[2:], frame(100, 0xaa)) {
		t.Fatalf("code 0: %v %x", err, out[:4])
	}
	// Code 0, a long frame: the two-byte length form.
	out, _ = selfDelimited(append([]byte{toc}, frame(300, 0xbb)...))
	if n, used := decodeOpusLength(out[1:]); n != 300 || used != 2 || len(out) != 1+2+300 {
		t.Fatalf("code 0 long: length %d in %d bytes, total %d", n, used, len(out))
	}
	// Code 1: two equal frames, one length.
	out, _ = selfDelimited(append([]byte{toc | 1}, frame(40, 0xcc)...))
	if out[1] != 20 || len(out) != 1+1+40 {
		t.Fatalf("code 1: %x len %d", out[:3], len(out))
	}
	// Code 2: N1 present, N2 added after it.
	code2 := append([]byte{toc | 2, 10}, frame(10, 1)...)
	code2 = append(code2, frame(25, 2)...)
	out, _ = selfDelimited(code2)
	if out[1] != 10 || out[2] != 25 || len(out) != 3+35 {
		t.Fatalf("code 2: %x len %d", out[:4], len(out))
	}
	// Code 3 CBR, three frames of 7: the frame size added after the count byte.
	out, _ = selfDelimited(append([]byte{toc | 3, 3}, frame(21, 3)...))
	if out[2] != 7 || len(out) != 3+21 {
		t.Fatalf("code 3 CBR: %x len %d", out[:4], len(out))
	}
	// Code 3 VBR (bit 7 of the count byte), frames 5, 6, 9: the last size added after the present ones.
	code3 := append([]byte{toc | 3, 0x80 | 3, 5, 6}, frame(5, 4)...)
	code3 = append(code3, frame(6, 5)...)
	code3 = append(code3, frame(9, 6)...)
	out, _ = selfDelimited(code3)
	if out[2] != 5 || out[3] != 6 || out[4] != 9 || len(out) != 5+20 {
		t.Fatalf("code 3 VBR: %x len %d", out[:6], len(out))
	}
	// The empty frame.
	if empty := emptyOpusFrameSelfDelimited(); !bytes.Equal(empty, []byte{0xf8, 0x02, 0xff, 0xfe}) {
		t.Fatalf("empty self-delimited frame %x", empty)
	}
}

// FFmpeg's libopus wrapper (libavcodec/libopusdec.c), which tgcalls' part decoder uses, reorders 3-8 channels from
// Vorbis to FFmpeg order whatever the mapping family: decoder output channel ch reads stream
// head.mapping[ff_vorbis_channel_layout_offsets[N-1][ch]]. The part's channel c (TG_META) is that output channel, so
// the head's mapping must make output channel c read stream c, the speaker the server put there.
func TestOpusHeadMappingUndoesLibopusReorder(t *testing.T) {
	vorbisOffsets := [8][8]int{ // libavcodec/vorbis_data.c, FFmpeg 8.1.2
		{0},
		{0, 1},
		{0, 2, 1},
		{0, 1, 2, 3},
		{0, 2, 1, 3, 4},
		{0, 2, 1, 5, 3, 4},
		{0, 2, 1, 6, 5, 3, 4},
		{0, 2, 1, 7, 5, 6, 3, 4},
	}
	for channels := 1; channels <= 8; channels++ {
		head := opusHead(channels)
		mapping := head[21 : 21+channels]
		for ch := 0; ch < channels; ch++ {
			stream := int(mapping[ch])
			if channels > 2 {
				stream = int(mapping[vorbisOffsets[channels-1][ch]])
			}
			if stream != ch {
				t.Fatalf("%d channels: decoder output %d reads stream %d (mapping %v)", channels, ch, stream, mapping)
			}
		}
	}
}

func TestPartRejectsNineChannels(t *testing.T) {
	meta := audioPartMeta{SSRCs: []uint32{1, 2, 3, 4, 5, 6, 7, 8, 9}}
	slots := make([][][]byte, 50)
	for s := range slots {
		slots[s] = make([][]byte, 9)
	}
	if _, err := buildOpusPart(meta, slots); err == nil {
		t.Fatalf("nine channels accepted")
	}
	if _, err := buildOpusPart(audioPartMeta{}, nil); err == nil {
		t.Fatalf("zero channels accepted")
	}
}

type oggPage struct {
	headerType byte
	granule    uint64
	sequence   uint32
	packet     []byte
}

func parseOggPages(t *testing.T, data []byte) []oggPage {
	var pages []oggPage
	for offset := 0; offset < len(data); {
		if string(data[offset:offset+4]) != "OggS" || data[offset+4] != 0 {
			t.Fatalf("bad page at %d", offset)
		}
		segments := int(data[offset+26])
		table := data[offset+27 : offset+27+segments]
		bodyLength := 0
		for _, l := range table {
			bodyLength += int(l)
		}
		pageLength := 27 + segments + bodyLength
		page := append([]byte(nil), data[offset:offset+pageLength]...)
		storedCRC := binary.LittleEndian.Uint32(page[22:26])
		copy(page[22:26], []byte{0, 0, 0, 0})
		if referenceOggCRC(page) != storedCRC {
			t.Fatalf("page at %d: bad CRC", offset)
		}
		pages = append(pages, oggPage{
			headerType: data[offset+5],
			granule:    binary.LittleEndian.Uint64(data[offset+6 : offset+14]),
			sequence:   binary.LittleEndian.Uint32(data[offset+18 : offset+22]),
			packet:     data[offset+27+segments : offset+pageLength],
		})
		offset += pageLength
	}
	return pages
}

func TestPartPagesParse(t *testing.T) {
	ssrcs := []uint32{1001, 1002, 1003}
	meta := audioPartMeta{SSRCs: ssrcs, VideoEndpoints: map[int]string{0: "ep-a", 2: "ep-c"}}
	packet := append([]byte{0xf8}, bytes.Repeat([]byte{0x11}, 60)...)
	slots := make([][][]byte, 50)
	for s := range slots {
		slots[s] = [][]byte{packet, packet, packet}
		if s >= 10 && s < 15 {
			slots[s][2] = nil
		}
	}
	data, err := buildOpusPart(meta, slots)
	if err != nil {
		t.Fatalf("buildOpusPart: %v", err)
	}
	pages := parseOggPages(t, data)
	if len(pages) != 52 {
		t.Fatalf("%d pages, want 52 (head, tags, 50 audio)", len(pages))
	}
	if pages[0].headerType&0x02 == 0 || pages[len(pages)-1].headerType&0x04 == 0 {
		t.Fatalf("BOS/EOS flags wrong: %x %x", pages[0].headerType, pages[len(pages)-1].headerType)
	}
	head := pages[0].packet
	if string(head[:8]) != "OpusHead" || head[8] != 1 || head[9] != 3 || head[18] != 255 || head[19] != 3 || head[20] != 0 ||
		!bytes.Equal(head[21:24], []byte{0, 2, 1}) || binary.LittleEndian.Uint32(head[12:16]) != 48000 {
		t.Fatalf("OpusHead %x", head)
	}
	tags := pages[1].packet
	if string(tags[:8]) != "OpusTags" {
		t.Fatalf("OpusTags missing")
	}
	comments := map[string]string{}
	offset := 8
	vendorLength := int(binary.LittleEndian.Uint32(tags[offset:]))
	offset += 4 + vendorLength
	count := int(binary.LittleEndian.Uint32(tags[offset:]))
	offset += 4
	for i := 0; i < count; i++ {
		length := int(binary.LittleEndian.Uint32(tags[offset:]))
		offset += 4
		parts := strings.SplitN(string(tags[offset:offset+length]), "=", 2)
		comments[parts[0]] = parts[1]
		offset += length
	}
	raw, err := base64.StdEncoding.DecodeString(comments["TG_META"])
	if err != nil || len(raw) != 8+3*12 {
		t.Fatalf("TG_META %q: %v", comments["TG_META"], err)
	}
	if binary.LittleEndian.Uint32(raw[0:]) != 3 || binary.LittleEndian.Uint32(raw[4:]) != 3 {
		t.Fatalf("TG_META counts %x", raw[:8])
	}
	for c := 0; c < 3; c++ {
		entry := raw[8+12*c:]
		if binary.LittleEndian.Uint32(entry[0:]) != 0 || binary.LittleEndian.Uint32(entry[4:]) != uint32(c) ||
			binary.LittleEndian.Uint32(entry[8:]) != ssrcs[c] {
			t.Fatalf("TG_META entry %d %x", c, entry[:12])
		}
	}
	if mask, _ := strconv.Atoi(comments["ACTIVE_MASK"]); mask != 0b101 {
		t.Fatalf("ACTIVE_MASK %q", comments["ACTIVE_MASK"])
	}
	if comments["ENDPOINTS"] != "ep-a ep-c" {
		t.Fatalf("ENDPOINTS %q", comments["ENDPOINTS"])
	}
	var previous uint64
	for i, page := range pages[2:] {
		if page.granule != uint64(960*(i+1)) || page.granule <= previous {
			t.Fatalf("audio page %d granule %d", i, page.granule)
		}
		previous = page.granule
		if page.sequence != uint32(i+2) {
			t.Fatalf("audio page %d sequence %d", i, page.sequence)
		}
	}
	if pages[len(pages)-1].granule != 48000 {
		t.Fatalf("last granule %d", pages[len(pages)-1].granule)
	}
	// Slot 12: streams 0 and 1 self-delimited (TOC, length 60, frame), stream 2 the silence frame (normal framing).
	slot12 := pages[2+12].packet
	if len(slot12) != 2*(2+60)+3 || slot12[1] != 60 || !bytes.Equal(slot12[len(slot12)-3:], []byte{0xf8, 0xff, 0xfe}) {
		t.Fatalf("slot 12 packet %d bytes", len(slot12))
	}
}
