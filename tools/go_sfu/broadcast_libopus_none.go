//go:build !tgcalls_vendored_opus

package main

// Outside Bazel (`go test`) there is no vendored libopus: the broadcast server builds no listener audio, and tests
// inject their own codec.
func defaultBroadcastTranscoder() broadcastTranscoder {
	return nil
}
