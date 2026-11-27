package main

// C bindings of the emulated broadcast server (broadcast_server.go), keyed by SFU handle like sfu.go's.
// Kinds: 0 audio, 1 video, 2 unified. Statuses: 0 success, 1 not ready, 2 resync needed, 3 join missing.
// Qualities: 0 thumbnail, 1 medium, 2 full. Returned strings and parts are freed with GoSfu_Free.

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"
	"fmt"
)

func lookupSFU(handle C.int) *SFU {
	sfuRegistryMu.Lock()
	defer sfuRegistryMu.Unlock()
	return sfuRegistry[int(handle)]
}

// GoSfu_BroadcastConfigure loads the fixture slices and sets the availability delay and retained window.
// Returns 0, or -1 when the handle or the fixtures are missing.
//
//export GoSfu_BroadcastConfigure
func GoSfu_BroadcastConfigure(handle C.int, fixtureDir *C.char, availabilityDelayMs C.int, retainMs C.int) C.int {
	sfu := lookupSFU(handle)
	if sfu == nil {
		return -1
	}
	fixtures, err := loadBroadcastFixtures(C.GoString(fixtureDir))
	if err != nil {
		fmt.Printf("[GoSfu] BroadcastConfigure: %v\n", err)
		return -1
	}
	sfu.broadcast.configure(fixtures, int64(availabilityDelayMs), int64(retainMs))
	return 0
}

// GoSfu_BroadcastSetVideoEndpoints names the participants that send video: [{"participant":0,"endpoint":"ep-0"}].
// Returns 0, or -1 on a bad handle or JSON.
//
//export GoSfu_BroadcastSetVideoEndpoints
func GoSfu_BroadcastSetVideoEndpoints(handle C.int, endpointsJSON *C.char) C.int {
	sfu := lookupSFU(handle)
	if sfu == nil {
		return -1
	}
	var entries []struct {
		Participant int    `json:"participant"`
		Endpoint    string `json:"endpoint"`
	}
	if err := json.Unmarshal([]byte(C.GoString(endpointsJSON)), &entries); err != nil {
		fmt.Printf("[GoSfu] BroadcastSetVideoEndpoints: %v\n", err)
		return -1
	}
	endpoints := map[int]string{}
	for _, entry := range entries {
		endpoints[entry.Participant] = entry.Endpoint
	}
	sfu.broadcast.setVideoEndpoints(endpoints)
	return 0
}

// GoSfu_BroadcastAddClient registers a listener with a link profile (wifi, lte, poor-lte, 3g, step-down or
// "<bps>,<rttMs>") and scripted events ("notready-burst@8,resync@14,join-missing@18", seconds from now).
// Returns 0, or -1 on an unknown profile or event.
//
//export GoSfu_BroadcastAddClient
func GoSfu_BroadcastAddClient(handle C.int, clientID C.int, profile *C.char, events *C.char) C.int {
	sfu := lookupSFU(handle)
	if sfu == nil {
		return -1
	}
	if err := sfu.broadcast.addClient(int(clientID), C.GoString(profile), C.GoString(events)); err != nil {
		fmt.Printf("[GoSfu] BroadcastAddClient: %v\n", err)
		return -1
	}
	return 0
}

//export GoSfu_BroadcastServerTimeMs
func GoSfu_BroadcastServerTimeMs(handle C.int) C.longlong {
	sfu := lookupSFU(handle)
	if sfu == nil {
		return 0
	}
	return C.longlong(sfu.broadcast.serverTimeMs())
}

//export GoSfu_BroadcastLastPartMs
func GoSfu_BroadcastLastPartMs(handle C.int) C.longlong {
	sfu := lookupSFU(handle)
	if sfu == nil {
		return 0
	}
	return C.longlong(sfu.broadcast.lastPartMs())
}

// GoSfu_BroadcastFetch requests one part and blocks for the client's link delivery time. The part (nil unless the
// status is success) is freed with GoSfu_Free; *outResponseMs is the server's clock at the answer.
//
//export GoSfu_BroadcastFetch
func GoSfu_BroadcastFetch(handle C.int, clientID C.int, kind C.int, tsMs C.longlong, channelID C.int, quality C.int, outStatus *C.int, outResponseMs *C.longlong, outLen *C.int) *C.char {
	*outLen = 0
	*outStatus = C.int(statusResync)
	sfu := lookupSFU(handle)
	if sfu == nil {
		return nil
	}
	status, data, responseMs := sfu.broadcast.fetch(int(clientID), partKind(kind), int64(tsMs), int32(channelID), int(quality))
	*outStatus = C.int(status)
	*outResponseMs = C.longlong(responseMs)
	if len(data) == 0 {
		return nil
	}
	*outLen = C.int(len(data))
	return (*C.char)(C.CBytes(data))
}

// GoSfu_BroadcastStats: {"requests": {key: {"success","notReady","resync","joinMissing"}}, "bytes": {key: n}}.
//
//export GoSfu_BroadcastStats
func GoSfu_BroadcastStats(handle C.int, clientID C.int) *C.char {
	sfu := lookupSFU(handle)
	if sfu == nil {
		return C.CString("{}")
	}
	return C.CString(sfu.broadcast.statsJSON(int(clientID)))
}
