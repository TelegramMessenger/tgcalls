package main

// The emulated broadcast server: what Telegram's server answers a broadcast listener's part, time and stream-channel
// requests with, under a per-listener link. Parts are 1000 ms; part timestamps are Unix milliseconds on the grid.
// A part [T, T+1000) is available from T + 1000 + availabilityDelayMs; earlier is NotReady, older than the retained
// window or off the grid is ResyncNeeded. The link delays each response as the app's DebugNetworkShaper does.

import (
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"sync"
	"time"
)

type partKind int

const (
	partAudio   partKind = 0
	partVideo   partKind = 1
	partUnified partKind = 2
)

type partStatus int

const (
	statusSuccess     partStatus = 0
	statusNotReady    partStatus = 1
	statusResync      partStatus = 2
	statusJoinMissing partStatus = 3
)

const (
	broadcastDefaultAvailabilityDelayMs = 300
	broadcastErrorResponseBytes         = 100
	broadcastNotReadyBurst              = 2 * time.Second
)

// --- link profiles (DebugNetworkShaper's, submodules/TelegramCore/Sources/Network/DebugNetworkShaper.swift) ---

type linkStep struct {
	start         time.Duration // since the link began
	bitsPerSecond float64
}

type linkProfile struct {
	name      string
	steps     []linkStep
	roundTrip time.Duration
}

func profileNamed(name string) (linkProfile, bool) {
	single := func(bps float64, rtt time.Duration) linkProfile {
		return linkProfile{name: name, steps: []linkStep{{0, bps}}, roundTrip: rtt}
	}
	switch name {
	case "wifi":
		return single(50e6, 20*time.Millisecond), true
	case "lte":
		return single(10e6, 60*time.Millisecond), true
	case "poor-lte":
		return single(3e6, 150*time.Millisecond), true
	case "3g":
		return single(1.5e6, 300*time.Millisecond), true
	case "step-down":
		return linkProfile{name: name, steps: []linkStep{
			{0, 10e6}, {30 * time.Second, 1e6}, {60 * time.Second, 10e6},
		}, roundTrip: 80 * time.Millisecond}, true
	}
	parts := strings.Split(name, ",")
	if len(parts) != 2 {
		return linkProfile{}, false
	}
	bps, err1 := strconv.ParseFloat(parts[0], 64)
	rttMs, err2 := strconv.ParseFloat(parts[1], 64)
	if err1 != nil || err2 != nil || bps <= 0 || rttMs < 0 {
		return linkProfile{}, false
	}
	return single(bps, time.Duration(rttMs*float64(time.Millisecond))), true
}

// partLink is one listener's link: a response's bytes start arriving one round trip after it was sent, or when the
// link is done with the responses before it, and take the profile's bandwidth.
type partLink struct {
	mu      sync.Mutex
	profile linkProfile
	start   time.Time
	freeAt  time.Time
}

func newPartLink(profile linkProfile, start time.Time) *partLink {
	return &partLink{profile: profile, start: start}
}

func (l *partLink) bitsPerSecond(at time.Time) float64 {
	elapsed := at.Sub(l.start)
	result := l.profile.steps[0].bitsPerSecond
	for _, step := range l.profile.steps {
		if step.start <= elapsed {
			result = step.bitsPerSecond
		}
	}
	if result < 1 {
		return 1
	}
	return result
}

// hold is how long to hold a response of `bytes` sent at `sentAt`, at `now`; it moves the link on.
func (l *partLink) hold(bytes int, sentAt, now time.Time) time.Duration {
	l.mu.Lock()
	defer l.mu.Unlock()
	begin := sentAt.Add(l.profile.roundTrip)
	if l.freeAt.After(begin) {
		begin = l.freeAt
	}
	transfer := time.Duration(8 * float64(bytes) / l.bitsPerSecond(begin) * float64(time.Second))
	finish := begin.Add(transfer)
	l.freeAt = finish
	if hold := finish.Sub(now); hold > 0 {
		return hold
	}
	return 0
}

// --- clients and scripted events ---

type broadcastEvent struct {
	kind  string // "notready-burst", "resync", "join-missing"
	at    time.Duration
	fired bool
}

func parseBroadcastEvents(spec string) ([]*broadcastEvent, error) {
	var events []*broadcastEvent
	for _, item := range strings.Split(spec, ",") {
		item = strings.TrimSpace(item)
		if item == "" {
			continue
		}
		parts := strings.Split(item, "@")
		if len(parts) != 2 {
			return nil, fmt.Errorf("event %q: want kind@seconds", item)
		}
		switch parts[0] {
		case "notready-burst", "resync", "join-missing":
		default:
			return nil, fmt.Errorf("unknown event %q", parts[0])
		}
		seconds, err := strconv.ParseFloat(parts[1], 64)
		if err != nil || seconds < 0 {
			return nil, fmt.Errorf("event %q: bad time", item)
		}
		events = append(events, &broadcastEvent{kind: parts[0], at: time.Duration(seconds * float64(time.Second))})
	}
	return events, nil
}

type broadcastCounts struct {
	Success     int `json:"success"`
	NotReady    int `json:"notReady"`
	Resync      int `json:"resync"`
	JoinMissing int `json:"joinMissing"`
}

type broadcastClient struct {
	id       int
	link     *partLink
	addedAt  time.Time
	events   []*broadcastEvent
	requests map[string]*broadcastCounts
	bytes    map[string]int
}

// --- the server ---

type broadcastServer struct {
	mu                  sync.Mutex
	now                 func() time.Time
	sleep               func(time.Duration)
	audio               *broadcastAudio
	fixtures            *broadcastFixtures
	availabilityDelayMs int64
	retainMs            int64
	videoEndpoints      map[int]string // participant -> endpoint
	clients             map[int]*broadcastClient
	audioParts          map[int64][]byte // available listener-audio parts, built once
}

func newBroadcastServer(audio *broadcastAudio) *broadcastServer {
	return &broadcastServer{
		now:                 time.Now,
		sleep:               time.Sleep,
		audio:               audio,
		availabilityDelayMs: broadcastDefaultAvailabilityDelayMs,
		retainMs:            broadcastDefaultRetainMs,
		videoEndpoints:      map[int]string{},
		clients:             map[int]*broadcastClient{},
		audioParts:          map[int64][]byte{},
	}
}

func (s *broadcastServer) configure(fixtures *broadcastFixtures, availabilityDelayMs, retainMs int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.fixtures = fixtures
	s.availabilityDelayMs = availabilityDelayMs
	s.retainMs = retainMs
	s.audioParts = map[int64][]byte{}
	if s.audio != nil {
		s.audio.mu.Lock()
		s.audio.retainMs = retainMs
		s.audio.mu.Unlock()
	}
}

func (s *broadcastServer) setVideoEndpoints(endpoints map[int]string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.videoEndpoints = endpoints
	s.audioParts = map[int64][]byte{}
}

func (s *broadcastServer) addClient(id int, profileName, events string) error {
	profile, ok := profileNamed(profileName)
	if !ok {
		return fmt.Errorf("unknown link profile %q", profileName)
	}
	parsed, err := parseBroadcastEvents(events)
	if err != nil {
		return err
	}
	now := s.now()
	s.mu.Lock()
	defer s.mu.Unlock()
	s.clients[id] = &broadcastClient{
		id:       id,
		link:     newPartLink(profile, now),
		addedAt:  now,
		events:   parsed,
		requests: map[string]*broadcastCounts{},
		bytes:    map[string]int{},
	}
	return nil
}

func (s *broadcastServer) serverTimeMs() int64 {
	return s.now().UnixMilli()
}

// lastPartMs is the newest available part: an RTMP stream's last_timestamp_ms.
func (s *broadcastServer) lastPartMs() int64 {
	s.mu.Lock()
	delay := s.availabilityDelayMs
	s.mu.Unlock()
	return floorDiv64(s.now().UnixMilli()-1000-delay, 1000) * 1000
}

func statsKey(kind partKind, quality int) string {
	switch kind {
	case partAudio:
		return "audio"
	case partVideo:
		return fmt.Sprintf("video_q%d", clampQuality(quality))
	default:
		return fmt.Sprintf("unified_q%d", clampQuality(quality))
	}
}

// scriptedStatusLocked applies the client's scripted events at `elapsed`; ok is false when none applies.
func scriptedStatusLocked(client *broadcastClient, elapsed time.Duration) (partStatus, bool) {
	for _, event := range client.events {
		switch event.kind {
		case "notready-burst":
			if elapsed >= event.at && elapsed < event.at+broadcastNotReadyBurst {
				return statusNotReady, true
			}
		case "resync", "join-missing":
			if !event.fired && elapsed >= event.at {
				event.fired = true
				if event.kind == "resync" {
					return statusResync, true
				}
				return statusJoinMissing, true
			}
		}
	}
	return statusSuccess, false
}

// answer decides a request at `now` (the server's clock when the request arrives).
func (s *broadcastServer) answer(client *broadcastClient, kind partKind, tsMs int64, channelID int32, quality int, now time.Time) (partStatus, []byte) {
	nowMs := now.UnixMilli()
	s.mu.Lock()
	if status, scripted := scriptedStatusLocked(client, now.Sub(client.addedAt)); scripted {
		s.mu.Unlock()
		return status, nil
	}
	delay, retain, fixtures := s.availabilityDelayMs, s.retainMs, s.fixtures
	endpoints := s.videoEndpoints
	cached := s.audioParts[tsMs]
	s.mu.Unlock()

	if tsMs <= 0 || tsMs%1000 != 0 {
		return statusResync, nil
	}
	if nowMs < tsMs+1000+delay {
		return statusNotReady, nil
	}
	if tsMs < nowMs-retain {
		return statusResync, nil
	}
	switch kind {
	case partAudio:
		if cached != nil {
			return statusSuccess, cached
		}
		part, err := s.audio.part(tsMs, endpoints)
		if err != nil {
			fmt.Printf("[GoSfu] broadcast audio part %d: %v\n", tsMs, err)
			return statusNotReady, nil
		}
		s.mu.Lock()
		s.audioParts[tsMs] = part
		for old := range s.audioParts {
			if old < nowMs-retain {
				delete(s.audioParts, old)
			}
		}
		s.mu.Unlock()
		return statusSuccess, part
	case partVideo:
		if fixtures == nil {
			return statusResync, nil
		}
		participants := s.audio.channelParticipants(tsMs)
		channel := int(channelID) - 1
		if channel < 0 || channel >= len(participants) {
			return statusResync, nil
		}
		endpoint, ok := endpoints[participants[channel]]
		if !ok {
			return statusResync, nil
		}
		return statusSuccess, fixtures.videoPart(endpoint, quality, tsMs)
	case partUnified:
		if fixtures == nil {
			return statusResync, nil
		}
		return statusSuccess, fixtures.unifiedPart(quality, tsMs)
	}
	return statusResync, nil
}

// fetch answers one part request from a listener, held for its link's delivery time.
func (s *broadcastServer) fetch(clientID int, kind partKind, tsMs int64, channelID int32, quality int) (partStatus, []byte, int64) {
	sentAt := s.now()
	s.mu.Lock()
	client := s.clients[clientID]
	s.mu.Unlock()
	if client == nil {
		return statusResync, nil, sentAt.UnixMilli()
	}

	status, data := s.answer(client, kind, tsMs, channelID, quality, sentAt)
	size := len(data)
	if status != statusSuccess {
		data = nil
		size = broadcastErrorResponseBytes
	}
	s.sleep(client.link.hold(size, sentAt, s.now()))

	key := statsKey(kind, quality)
	s.mu.Lock()
	counts := client.requests[key]
	if counts == nil {
		counts = &broadcastCounts{}
		client.requests[key] = counts
	}
	switch status {
	case statusSuccess:
		counts.Success++
		client.bytes[key] += len(data)
	case statusNotReady:
		counts.NotReady++
	case statusResync:
		counts.Resync++
	case statusJoinMissing:
		counts.JoinMissing++
	}
	s.mu.Unlock()
	return status, data, s.now().UnixMilli()
}

// statsJSON: {"requests": {key: {"success","notReady","resync","joinMissing"}}, "bytes": {key: n}}, keys "audio",
// "video_q0".."video_q2", "unified_q0".."unified_q2" (only those with traffic).
func (s *broadcastServer) statsJSON(clientID int) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	client := s.clients[clientID]
	result := struct {
		Requests map[string]*broadcastCounts `json:"requests"`
		Bytes    map[string]int              `json:"bytes"`
	}{map[string]*broadcastCounts{}, map[string]int{}}
	if client != nil {
		result.Requests = client.requests
		result.Bytes = client.bytes
	}
	data, _ := json.Marshal(result)
	return string(data)
}
