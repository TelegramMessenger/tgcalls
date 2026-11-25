package main

// The AV1 RTP Dependency Descriptor. A production SFU forwards AV1 only with it; this SFU only counts it
// (GoSfu_QueryPayloadTypeStats's "dd"), at the ID its join responses list (GoSfu_SetDependencyDescriptorId), else at
// the client's default, 11.
const dependencyDescriptorURI = "https://aomediacodec.github.io/av1-rtp-spec/#dependency-descriptor-rtp-header-extension"
const dependencyDescriptorDefaultID = 11

// videoHdrextsJSON is the video "rtp-hdrexts" of a join response, the production server's list, and the Dependency
// Descriptor at ddID when ddID > 0.
func videoHdrextsJSON(ddID int) []rtpHdrextJSON {
	exts := []rtpHdrextJSON{
		{ID: 2, URI: "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"},
		{ID: 3, URI: "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"},
		{ID: 13, URI: "urn:3gpp:video-orientation"},
	}
	if ddID > 0 {
		exts = append(exts, rtpHdrextJSON{ID: ddID, URI: dependencyDescriptorURI})
	}
	return exts
}

// rtpHasExtension reports whether an RTP packet carries header extension `id` (RFC 8285, one-byte or two-byte form).
func rtpHasExtension(pkt []byte, id int) bool {
	if len(pkt) < 12 || pkt[0]&0x10 == 0 {
		return false
	}
	offset := 12 + 4*int(pkt[0]&0x0f)
	if len(pkt) < offset+4 {
		return false
	}
	profile := uint16(pkt[offset])<<8 | uint16(pkt[offset+1])
	length := 4 * (int(pkt[offset+2])<<8 | int(pkt[offset+3]))
	data := pkt[offset+4:]
	if len(data) < length {
		return false
	}
	data = data[:length]
	switch {
	case profile == 0xBEDE:
		for i := 0; i < len(data); {
			elementID := int(data[i] >> 4)
			if elementID == 0 {
				i++
				continue
			}
			if elementID == 15 {
				return false
			}
			if elementID == id {
				return true
			}
			i += 1 + int(data[i]&0x0f) + 1
		}
	case profile&0xFFF0 == 0x1000:
		for i := 0; i+1 < len(data); {
			elementID := int(data[i])
			if elementID == 0 {
				i++
				continue
			}
			if elementID == id {
				return true
			}
			i += 2 + int(data[i+1])
		}
	}
	return false
}
