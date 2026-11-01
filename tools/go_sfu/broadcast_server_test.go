package main

import (
	"encoding/json"
	"math"
	"path/filepath"
	"testing"
	"time"
)

// fakeClock: sleeping advances it.
type fakeClock struct{ t time.Time }

func (c *fakeClock) now() time.Time          { return c.t }
func (c *fakeClock) sleep(d time.Duration)   { c.t = c.t.Add(d) }
func (c *fakeClock) set(ms int64)            { c.t = time.UnixMilli(ms) }
func (c *fakeClock) advance(d time.Duration) { c.t = c.t.Add(d) }

const testNowMs = 1_700_000_100_000

func newTestServer(t *testing.T) (*broadcastServer, *fakeClock) {
	clock := &fakeClock{}
	clock.set(testNowMs)
	server := newBroadcastServer(newTestBroadcastAudio())
	server.now = clock.now
	server.sleep = clock.sleep
	fixtures, err := loadBroadcastFixtures(filepath.Join("testdata", "broadcast"))
	if err != nil {
		t.Fatalf("fixtures: %v", err)
	}
	server.configure(fixtures, 300, 30000)
	return server, clock
}

func mustAddClient(t *testing.T, server *broadcastServer, id int, profile, events string) {
	if err := server.addClient(id, profile, events); err != nil {
		t.Fatalf("addClient(%q, %q): %v", profile, events, err)
	}
}

func TestAvailability(t *testing.T) {
	server, clock := newTestServer(t)
	mustAddClient(t, server, 1, "1000000000,0", "")
	part := int64(testNowMs)
	clock.set(part + 1000 + 300 - 1)
	if status, _, _ := server.fetch(1, partUnified, part, 1, 2); status != statusNotReady {
		t.Fatalf("one ms early: status %d", status)
	}
	clock.set(part + 1000 + 300)
	status, data, responseMs := server.fetch(1, partUnified, part, 1, 2)
	if status != statusSuccess || len(data) == 0 {
		t.Fatalf("on time: status %d, %d bytes", status, len(data))
	}
	if responseMs < part+1300 {
		t.Fatalf("responseMs %d before the request", responseMs)
	}
	if last := server.lastPartMs(); last != part {
		t.Fatalf("lastPartMs %d, want %d", last, part)
	}
}

func TestOddTimestamps(t *testing.T) {
	server, _ := newTestServer(t)
	mustAddClient(t, server, 1, "1000000000,0", "")
	cases := []struct {
		tsMs int64
		want partStatus
	}{
		{0, statusResync},
		{-1000, statusResync},
		{1500, statusResync},
		{testNowMs + 60000, statusNotReady},
		{testNowMs - 31000, statusResync},
	}
	for _, c := range cases {
		if status, _, _ := server.fetch(1, partUnified, c.tsMs, 1, 2); status != c.want {
			t.Fatalf("tsMs %d: status %d, want %d", c.tsMs, status, c.want)
		}
	}
	if status, _, _ := server.fetch(99, partUnified, testNowMs-5000, 1, 2); status != statusResync {
		t.Fatalf("unknown client: status %d", status)
	}
	if status, _, _ := server.fetch(1, partVideo, testNowMs-5000, 7, 2); status != statusResync {
		t.Fatalf("unknown video channel: status %d", status)
	}
}

func TestLinkModelMatchesShaper(t *testing.T) {
	profile, ok := profileNamed("lte")
	if !ok || profile.roundTrip != 60*time.Millisecond {
		t.Fatalf("lte profile %+v", profile)
	}
	start := time.UnixMilli(testNowMs)
	link := newPartLink(profile, start)
	for i, want := range []float64{0.16, 0.26, 0.36} {
		hold := link.hold(125_000, start, start).Seconds()
		if math.Abs(hold-want) > 1e-6 {
			t.Fatalf("response %d: hold %.3f s, want %.3f", i, hold, want)
		}
	}
	// A response sent later, after the link is free again, waits only for its round trip and its bytes.
	later := start.Add(10 * time.Second)
	if hold := link.hold(125_000, later, later).Seconds(); math.Abs(hold-0.16) > 1e-6 {
		t.Fatalf("idle link: hold %.3f s", hold)
	}
}

func TestStepDownProfile(t *testing.T) {
	profile, ok := profileNamed("step-down")
	if !ok || profile.roundTrip != 80*time.Millisecond {
		t.Fatalf("step-down profile %+v", profile)
	}
	start := time.UnixMilli(testNowMs)
	link := newPartLink(profile, start)
	for _, c := range []struct {
		seconds float64
		bps     float64
	}{{0, 10e6}, {29.9, 10e6}, {30, 1e6}, {59.9, 1e6}, {60, 10e6}, {200, 10e6}} {
		at := start.Add(time.Duration(c.seconds * float64(time.Second)))
		if got := link.bitsPerSecond(at); got != c.bps {
			t.Fatalf("at %.1f s: %.0f bps, want %.0f", c.seconds, got, c.bps)
		}
	}
	for _, name := range []string{"wifi", "poor-lte", "3g", "2000000,100"} {
		if _, ok := profileNamed(name); !ok {
			t.Fatalf("profile %q unknown", name)
		}
	}
	if _, ok := profileNamed("dial-up"); ok {
		t.Fatalf("unknown profile accepted")
	}
}

func TestEvents(t *testing.T) {
	server, clock := newTestServer(t)
	mustAddClient(t, server, 1, "1000000000,0", "notready-burst@2,resync@5,join-missing@7")
	addedMs := clock.t.UnixMilli()
	part := int64(testNowMs - 5000)
	expect := func(atSeconds float64, want partStatus) {
		clock.set(addedMs + int64(atSeconds*1000))
		if status, _, _ := server.fetch(1, partUnified, part, 1, 2); status != want {
			t.Fatalf("at %.1f s: status %d, want %d", atSeconds, status, want)
		}
	}
	expect(1.0, statusSuccess)
	expect(2.5, statusNotReady)
	expect(3.9, statusNotReady)
	expect(4.1, statusSuccess)
	expect(5.2, statusResync)
	expect(5.3, statusSuccess)
	expect(7.5, statusJoinMissing)
	expect(7.6, statusSuccess)
	if err := server.addClient(2, "wifi", "earthquake@3"); err == nil {
		t.Fatalf("unknown event accepted")
	}
}

func TestStatsCount(t *testing.T) {
	server, clock := newTestServer(t)
	mustAddClient(t, server, 1, "1000000000,0", "")
	part := int64(testNowMs - 5000)
	_, first, _ := server.fetch(1, partUnified, part, 1, 2)
	_, second, _ := server.fetch(1, partUnified, part, 1, 2)
	server.fetch(1, partUnified, testNowMs+60000, 1, 0)
	server.fetch(1, partUnified, 1500, 1, 0)
	clock.advance(time.Second)
	var stats struct {
		Requests map[string]map[string]int `json:"requests"`
		Bytes    map[string]int            `json:"bytes"`
	}
	if err := json.Unmarshal([]byte(server.statsJSON(1)), &stats); err != nil {
		t.Fatalf("stats JSON: %v", err)
	}
	if stats.Requests["unified_q2"]["success"] != 2 || stats.Requests["unified_q0"]["notReady"] != 1 || stats.Requests["unified_q0"]["resync"] != 1 {
		t.Fatalf("requests %v", stats.Requests)
	}
	if stats.Bytes["unified_q2"] != len(first)+len(second) {
		t.Fatalf("bytes %v, want %d", stats.Bytes, len(first)+len(second))
	}
}
