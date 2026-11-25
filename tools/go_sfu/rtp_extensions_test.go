package main

import "testing"

func rtpHeader(extensionProfile uint16, extension []byte) []byte {
	pkt := []byte{0x90, 106, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2} // V=2, X=1, PT 106
	words := (len(extension) + 3) / 4
	padded := make([]byte, words*4)
	copy(padded, extension)
	pkt = append(pkt, byte(extensionProfile>>8), byte(extensionProfile), byte(words>>8), byte(words))
	pkt = append(pkt, padded...)
	return append(pkt, 0xAA, 0xBB) // payload
}

// One-byte header extensions (RFC 8285): ID 4 bits, length-1 4 bits; ID 0 is padding.
func TestRTPHasExtensionOneByte(t *testing.T) {
	pkt := rtpHeader(0xBEDE, []byte{0x30, 0x01, 0x00, 0xB2, 0x01, 0x02, 0x03})
	if !rtpHasExtension(pkt, 11) || !rtpHasExtension(pkt, 3) {
		t.Fatalf("extensions 3 and 11 not found")
	}
	if rtpHasExtension(pkt, 2) {
		t.Fatalf("extension 2 found")
	}
}

// Two-byte header extensions: ID and length a byte each (an element longer than 16 bytes needs them).
func TestRTPHasExtensionTwoByte(t *testing.T) {
	ext := []byte{11, 20}
	ext = append(ext, make([]byte, 20)...)
	pkt := rtpHeader(0x1000, ext)
	if !rtpHasExtension(pkt, 11) {
		t.Fatalf("two-byte extension 11 not found")
	}
}

func TestRTPHasExtensionNone(t *testing.T) {
	pkt := []byte{0x80, 106, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0xAA}
	if rtpHasExtension(pkt, 11) {
		t.Fatalf("extension found in a packet without extensions")
	}
	if rtpHasExtension(pkt[:5], 11) {
		t.Fatalf("extension found in a short packet")
	}
}

// The join response lists the Dependency Descriptor only when told to, at that ID.
func TestJoinResponseListsDependencyDescriptor(t *testing.T) {
	for _, ext := range videoHdrextsJSON(0) {
		if ext.URI == dependencyDescriptorURI {
			t.Fatalf("dependency descriptor listed without an ID")
		}
	}
	found := false
	for _, ext := range videoHdrextsJSON(12) {
		if ext.URI == dependencyDescriptorURI && ext.ID == 12 {
			found = true
		}
	}
	if !found {
		t.Fatalf("dependency descriptor not listed at 12")
	}
}
