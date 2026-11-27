//go:build tgcalls_vendored_opus

package main

// libopus for the broadcast server's transcoding, from //third-party/opus (the same library webrtc links), built
// only under Bazel (gotags tgcalls_vendored_opus). `go test` injects a fake codec instead.

/*
#include <stdlib.h>
#include <opus/opus.h>

static int tgcallsBroadcastSetBitrate(OpusEncoder *encoder, opus_int32 bitrate) {
	return opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
}
*/
import "C"

import (
	"fmt"
	"unsafe"
)

const (
	libopusMaxFrameSamples = 5760 // 120 ms at 48 kHz
	libopusMaxPacketBytes  = 4000
	broadcastBitrate       = 32000
)

type libopusTranscoder struct{}

type libopusDecoder struct {
	decoder *C.OpusDecoder
	pcm     []int16
}

type libopusEncoder struct {
	encoder *C.OpusEncoder
	packet  []byte
}

func defaultBroadcastTranscoder() broadcastTranscoder {
	return libopusTranscoder{}
}

func (libopusTranscoder) newDecoder() (audioDecoder, error) {
	var status C.int
	decoder := C.opus_decoder_create(48000, 1, &status)
	if status != C.OPUS_OK || decoder == nil {
		return nil, fmt.Errorf("opus_decoder_create: %d", int(status))
	}
	return &libopusDecoder{decoder: decoder, pcm: make([]int16, libopusMaxFrameSamples)}, nil
}

func (libopusTranscoder) newEncoder() (audioEncoder, error) {
	var status C.int
	encoder := C.opus_encoder_create(48000, 1, C.OPUS_APPLICATION_AUDIO, &status)
	if status != C.OPUS_OK || encoder == nil {
		return nil, fmt.Errorf("opus_encoder_create: %d", int(status))
	}
	C.tgcallsBroadcastSetBitrate(encoder, broadcastBitrate)
	return &libopusEncoder{encoder: encoder, packet: make([]byte, libopusMaxPacketBytes)}, nil
}

func (d *libopusDecoder) decode(pkt []byte) ([]int16, error) {
	if len(pkt) == 0 {
		return nil, fmt.Errorf("empty packet")
	}
	n := C.opus_decode(d.decoder, (*C.uchar)(unsafe.Pointer(&pkt[0])), C.opus_int32(len(pkt)),
		(*C.opus_int16)(unsafe.Pointer(&d.pcm[0])), C.int(libopusMaxFrameSamples), 0)
	if n < 0 {
		return nil, fmt.Errorf("opus_decode: %d", int(n))
	}
	return append([]int16(nil), d.pcm[:int(n)]...), nil
}

func (d *libopusDecoder) close() {
	if d.decoder != nil {
		C.opus_decoder_destroy(d.decoder)
		d.decoder = nil
	}
}

func (e *libopusEncoder) encode(pcm []int16) ([]byte, error) {
	if len(pcm) != broadcastSlotSamples {
		return nil, fmt.Errorf("encode needs %d samples, got %d", broadcastSlotSamples, len(pcm))
	}
	n := C.opus_encode(e.encoder, (*C.opus_int16)(unsafe.Pointer(&pcm[0])), C.int(broadcastSlotSamples),
		(*C.uchar)(unsafe.Pointer(&e.packet[0])), C.opus_int32(libopusMaxPacketBytes))
	if n < 0 {
		return nil, fmt.Errorf("opus_encode: %d", int(n))
	}
	return append([]byte(nil), e.packet[:int(n)]...), nil
}

func (e *libopusEncoder) close() {
	if e.encoder != nil {
		C.opus_encoder_destroy(e.encoder)
		e.encoder = nil
	}
}
