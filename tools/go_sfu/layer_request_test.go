package main

import "testing"

func twoLayers() []SimulcastLayer {
	return []SimulcastLayer{
		{SSRC: 2001, FidSSRC: 2002},
		{SSRC: 2003, FidSSRC: 2004},
	}
}

// A receiver that asks for a lower layer and then for a higher one again (a tile going to thumbnail and back to full)
// gets the higher layer back at once when no bandwidth estimate arrives (REMB-free clients, every live run): the
// request alone decides, as it does for a downswitch. Each switch asks the sender for a keyframe on the new layer.
func TestLayerSelectorRaisedRequestUpswitchesWhenStale(t *testing.T) {
	m := newMockCallbacks(twoLayers(), -1)
	ls := NewLayerSelector(1, 0, 1, 2, m.toCallbacks())
	defer ls.Stop()

	ls.SetMaxLayer(0)
	if got := ls.CurrentLayer(); got != 0 {
		t.Fatalf("downswitch to L0: current %d", got)
	}
	plis := m.pliCount.Load()

	// Full again: the request is layer 2, the sender has two layers, so its top one (L1).
	ls.SetMaxLayer(2)
	if got := ls.CurrentLayer(); got != 1 {
		t.Fatalf("raised request with stale BW should upswitch to the top layer the sender has (L1); got %d", got)
	}
	if got := m.currentSelected(); got != 1 {
		t.Fatalf("SetSelectedLayer should have been called with 1; got %d", got)
	}
	if got := m.pliCount.Load(); got != plis+1 {
		t.Fatalf("upswitch should request a keyframe on the new layer; PLIs %d -> %d", plis, got)
	}
}

// With a fresh estimate the state machine keeps deciding upswitches.
func TestLayerSelectorRaisedRequestWhenFresh(t *testing.T) {
	m := newMockCallbacks(twoLayers(), 50_000) // fresh, below L1's threshold
	ls := NewLayerSelector(1, 0, 0, 0, m.toCallbacks())
	defer ls.Stop()

	ls.SetMaxLayer(2)
	if got := ls.CurrentLayer(); got != 0 {
		t.Fatalf("fresh BW should leave the state machine in charge; current %d", got)
	}
}
