package main

import "testing"

// A sender's simulcast layers from its join payload's ssrc-groups: SIM lists the layers, FID gives each its RTX.
func TestVideoLayersFromSimGroup(t *testing.T) {
	layers := videoLayersFromSSRCGroups([]ssrcGroupJSON{
		{Semantics: "SIM", Sources: []int32{10, 12}},
		{Semantics: "FID", Sources: []int32{10, 11}},
		{Semantics: "FID", Sources: []int32{12, 13}},
	})
	if len(layers) != 2 || layers[0] != (SimulcastLayer{SSRC: 10, FidSSRC: 11}) || layers[1] != (SimulcastLayer{SSRC: 12, FidSSRC: 13}) {
		t.Fatalf("layers = %+v", layers)
	}
}

// One layer has no SIM group, only its FID group (a one-layer AV1 screen share, a conference camera): one layer.
func TestVideoLayersFromSingleFidGroup(t *testing.T) {
	layers := videoLayersFromSSRCGroups([]ssrcGroupJSON{
		{Semantics: "FID", Sources: []int32{20, 21}},
	})
	if len(layers) != 1 || layers[0] != (SimulcastLayer{SSRC: 20, FidSSRC: 21}) {
		t.Fatalf("layers = %+v", layers)
	}
}

// No video, or FID groups the SIM group does not name: no layers to guess.
func TestVideoLayersWithoutVideo(t *testing.T) {
	if layers := videoLayersFromSSRCGroups(nil); len(layers) != 0 {
		t.Fatalf("no groups: layers = %+v", layers)
	}
	two := []ssrcGroupJSON{
		{Semantics: "FID", Sources: []int32{20, 21}},
		{Semantics: "FID", Sources: []int32{22, 23}},
	}
	if layers := videoLayersFromSSRCGroups(two); len(layers) != 0 {
		t.Fatalf("two FID groups without SIM: layers = %+v", layers)
	}
}

// The announcement of a one-layer sender carries its FID group and no SIM group, as the production server's does.
func TestVideoSSRCGroupsForAnnouncement(t *testing.T) {
	one := videoSSRCGroupsForAnnouncement([]SimulcastLayer{{SSRC: 20, FidSSRC: 21}})
	if len(one) != 1 || one[0].Semantics != "FID" {
		t.Fatalf("one layer: groups = %+v", one)
	}
	two := videoSSRCGroupsForAnnouncement([]SimulcastLayer{{SSRC: 10, FidSSRC: 11}, {SSRC: 12, FidSSRC: 13}})
	if len(two) != 3 || two[0].Semantics != "SIM" || len(two[0].Sources) != 2 {
		t.Fatalf("two layers: groups = %+v", two)
	}
}
