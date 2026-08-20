# Engine audit — July 2026

Findings from two investigations run 2026-07-30/31:

1. **Why Cloudflare TURN-only underperformed Telegram reflectors** in an `InstanceV2Impl`
   A/B (production engine, call version 13.0.0).
2. **An audit of `InstanceV2ReferenceImpl`** and its custom PeerConnection networking stack
   (call version 11.0.0, and the `v2wasm` cores shipped as 18.0.0/19.0.0).

Every line number was verified against the tree at the time of writing. Claims are marked
**[verified]** (read in source or computed from logs directly), **[measured]** (a number from
the production corpus, with denominator), or **[unverified]** (identified but never
adversarially checked — do not act on these without confirming).

## Corpus

Production call logs collected via the debug-log inbox, at
`~/build/getlogstgcalls/logs/<cohort>/<callId>/{a,b}.txt` — 200 calls x 2 endpoints per cohort.

| cohort | engine | relay |
|---|---|---|
| `AbOurOnly` | `InstanceV2Impl` 13.0.0 | Telegram reflectors |
| `AbCfOnly3` | `InstanceV2Impl` 13.0.0 | Cloudflare TURN |
| `AbV1100SctpFast` | `InstanceV2ReferenceImpl` 11.0.0 | reflectors |
| `AbV1300SctpFast` | `InstanceV2Impl` 13.0.0 | reflectors |

**Read the caveats in "A/B comparability" before drawing any cross-arm conclusion from these.**
The corpus is truncation-selected — `DTLS handshake complete` is 0/800 in every cohort, logs
end mid-activity with no teardown marker, and no successful call exists in any cohort on disk.
So "X never happened" means "X had not happened yet at the cut". It is *not* an all-zeros wall,
though: `Started DTLS handshake` and `Received STUN BINDING response` each appear in 87/400
reflector and 63/400 Cloudflare endpoints. Beware `DTLS setup complete.` (383/400) — that is
`DtlsTransport::SetupDtls` logging that the SSL adapter was configured, not a handshake result.

---

# Part 1 — `ReflectorPort` (affects production)

`InstanceV2Impl` (13.0.0, the shipping 1:1 engine) uses `ReflectorPort` via
`ReflectorRelayPortFactory`. These are the only findings here that reach users.

## 1.1 `CreateConnection` keys signalled connections differently from inbound packets

**[verified]** `ReflectorPort::CreateConnection` applies `SetResolvedIP` only on TCP:

```cpp
// v2/ReflectorPort.cpp:541
if (server_address_.proto == cricket::PROTO_TCP) {
    rtc::SocketAddress updated_address = updated_remote_candidate.address();
    updated_address.SetResolvedIP(server_address_.address.ipaddr());
    updated_remote_candidate.set_address(updated_address);
}
```

while `HandleIncomingPacket` applies it unconditionally (`ReflectorPort.cpp:765`).
`rtc::SocketAddress::EqualIPs` (`rtc_base/socket_address.cc:256`) tests `ip_` **first** —

```cpp
return (ip_ == addr.ip_) &&
       ((!IPIsAny(ip_) && !IPIsUnspec(ip_)) || (hostname_ == addr.hostname_));
```

— and the hostname fallback is reachable only once the IPs already match. So on UDP the
signalled connection (`ip_` unspecified, hostname `reflector-<serverId>-<tag>.reflector`) and
the inbound packet (`ip_` = reflector server IP, same hostname) are permanently different keys
in `Port::connections_`. `Connection::MaybeUpdatePeerReflexiveCandidate` cannot merge them —
it compares under the same `EqualIPs`.

Consequence: `GetConnection(packet.source_address())` misses, the inbound binding *request*
goes to `P2PTransportChannel::OnUnknownAddress`, and a second peer-reflexive connection is
created for the same logical path. A binding *response* from an unmatched address is simply
dropped — only requests can mint a prflx candidate — so **the signalled connection is deaf by
construction and the path can only bootstrap when the peer pings us first.** Our own outbound
checks cannot open it.

**[measured]** Parsing every `Conn[...]` line in `AbOurOnly` (400 logs) by remote candidate type:

| remote type | connections | pings sent | responses | conns with >=1 response |
|---|---|---|---|---|
| `relay` (signalled) | 749 | 15,985 | **0** | **0 (0.0%)** |
| `prflx` (twin) | 503 | 9,596 | 151 | 149 (29.6%) |

Cloudflare, same parse: 260 `relay` connections, 13,741 pings, 90 responses, 90/260 = 34.6%
with a response — and **zero** prflx connections, because its signalled connection is keyed
correctly. `Adding connection from peer reflexive candidate` appears in 168/400 reflector logs
and 0/400 Cloudflare logs.

**62.5% of all reflector ICE pings (15,985/25,581) go into connections that cannot answer.**
The doubled check list also halves the live connection's ping cadence — `P2PTransportChannel`
pings one connection per ~48 ms tick round-robin — to a median ~506 ms against Cloudflare's
~219 ms.

**Do not read the prflx connections as a reflector advantage.** 93.0% (467/502) of them
duplicate an address the peer had already signalled; they are the repair path for this bug, not
a discovery mechanism. Genuinely signalling-independent wins: 3/400 endpoints (0.75 pp,
Fisher p=0.075).

**Fix:** drop the `PROTO_TCP` gate at `:541` so UDP resolves the IP the same way
`HandleIncomingPacket` already does. Land it behind a flag with the prflx-connection count as
the readout — it should go to zero.

## 1.2 The local relay candidate carries no resolved IP, so `packet_overhead_bytes` is wrong

**[verified]** `ReflectorPort::PrepareAddress` calls `SetResolvedIP` only under
`if (standaloneReflectorMode_)` (`ReflectorPort.cpp:490`), and the app passes `false`
(`NetworkManager.cpp:119`). The candidate is therefore the synthetic hostname
`reflector-<serverId>-<randomTag>.reflector` with `ip_` unset.

`IPAddress::overhead()` returns 0 for an unspecified address, so
`p2p_transport_channel.cc` computes `packet_overhead_bytes = 0 + GetProtocolOverhead(UDP=8) = 8`.
**[measured]** `packet_overhead_bytes: 8` in 247/247 reflector route-change lines, `28` in
168/168 Cloudflare lines — zero variance in both. True on-wire overhead is ~52 B
(20 IP + 8 UDP + 24 ReflectorPort header).

Until this is corrected, **no BWE or bitrate metric is comparable across relay backends.**

## 1.3 Unverified `ReflectorPort` leads

**[unverified]** Identified during the audit, dropped before adversarial verification. Confirm
before acting:

- `ReflectorPort::SendTo` always returns success, hiding every relayed send failure from the
  ICE agent (`ReflectorPort.cpp:649-656`, and `Send` at `:908-912`).
- `~ReflectorPort` deletes the socket and then dereferences it on the TCP path
  (`ReflectorPort.cpp:231-236`: `if (!SharedSocket()) { delete socket_; }` at `:231-233`,
  immediately followed by
  `if (server_address_.proto == cricket::PROTO_TCP) { socket_->UnsubscribeCloseEvent(this); }`
  at `:235-236`).
  Likely dormant — 0 TCP reflector attempts in either cohort — and unreachable from
  `InstanceV2ReferenceImpl`, which drops TCP servers.

---

# Part 2 — `InstanceV2ReferenceImpl` (test-only)

**Scope note (2026-07-31): `InstanceV2ReferenceImpl` and versions 18.0.0/19.0.0 are not used in
production except for A/B testing, and no backwards-compatibility with them is required.**
Every defect below is therefore a **measurement** defect — it corrupts the experiment the engine
exists to serve, rather than harming callers. Ranked accordingly: arm-asymmetric defects first,
crash/latency defects last.

## 2.1 No baseline network record — ~77% of failing calls upload an empty timeline

**[verified]** `_networkStateLogRecords` has exactly one writer
(`InstanceV2ReferenceImpl.cpp:1257`), reached only from four state-*transition* callers:
`onCandidatePairChangeEvent` (`:616`), the 20 s watchdog (`:847`), `updateIsConnected` on an
actual change (`:861`), and the 2 s debounced disconnect (`:882`). `start()` ends at `:746-750`
without emitting a baseline. `InstanceV2Impl.cpp:1196-1198` does exactly that as the last
statement of its `start()`, so 13.0.0 structurally cannot emit an empty array.

Separately, `stop()`'s `baseTimestamp` (`:1553-1555`) is the timestamp of the first *surviving*
record, so `t` has no call-start origin.

**[measured]** The `Changing IceConnectionState` histogram over 400 endpoints is 332 x `0 => 1`
and nothing else; 323/400 never log `New selected connection`. Result: ~77% of failing 11.0.0
calls upload `"network": []` on a mis-anchored time axis. **The treatment arm's telemetry is
systematically poorer than the control's** — this corrupts whatever the A/B reads.

## 2.2 `AddTransceiver` instead of `AddTrack` costs every callee a round trip

**[verified]** Outgoing media is created with `AddTransceiver(track, init)` at `:700` (audio)
and `:1414` (video). `created_by_addtrack()` is set only on the AddTrack path
(`pc/rtp_transmission_manager.cc:228`; also the reuse path at `pc/sdp_offer_answer.cc:3137`),
and `FindAvailableTransceiverToReceive` (`pc/sdp_offer_answer.cc:4051-4058`) requires it. So
stock refuses to associate the callee's own transceiver with the offerer's m= section and mints
a fresh recvonly one instead.

**[measured]** `in response to the remote description` in 331/400 v1100 logs vs 0/400 v1300;
`in response to a call to AddTrack` **0/400**. Cost: median 303 ms / p90 733 ms of extra
negotiation before the callee can send, treatment arm only — which biases any
establishment-latency or connect-rate comparison against ReferenceImpl by construction. Every
session also permanently carries two unidirectional audio m-lines (five on a video call).

**These are `sendrecv` transceivers, not directional.** `RtpTransceiverInit::direction` defaults
to `kSendRecv` (`api/rtp_transceiver_interface.h:39`) and the file never sets it — a grep for
`RtpTransceiverDirection|.direction =` across all 1866 lines returns one hit, the word
"directions" in a comment at `:894`. Same in `InstanceV2CompatImpl`.
`v2wasm/ReferenceCallCore.cpp` explicitly requests `{"direction", "sendrecv"}` at `:97` and
`:253`. The directional design lives in the **group** engine
(`GroupInstanceReferenceImpl.cpp:646` `kSendOnly`, `:1212`/`:1346` `kRecvOnly`) and in the ABI's
capability (`CallCoreHost.cpp:1091-1097`), not in the 1:1 path.

### Making it directional in one round trip

The intended design for 1:1 is directional (two unidirectional m-lines). Getting that **without**
the extra round trip is not obvious, because the association gate reads the direction the
*offerer wrote*:

```cpp
// pc/sdp_offer_answer.cc:3836
if (!transceiver &&
    RtpTransceiverDirectionHasRecv(media_desc->direction()) &&   // kSendRecv || kRecvOnly only
    !media_desc->HasSimulcast()) {
  transceiver = FindAvailableTransceiverToReceive(media_desc->type());
}
```

(`RtpTransceiverDirectionHasRecv` is `pc/rtp_media_utils.cc:35`.) Therefore:

- A **sendonly** offer section skips the search entirely. The answerer always mints a fresh
  recvonly transceiver for it — correct and free, nothing needs pre-creating.
- A **recvonly** offer section *does* run the search, and it still requires
  `created_by_addtrack()`.

The working recipe:

| side | action |
|---|---|
| both | `AddTrack(track, {"0"})` — sets `created_by_addtrack()` — then `SetDirectionWithError(kSendOnly)` (`api/rtp_transceiver_interface.h:115`) |
| offerer only (`_encryptionKey.isOutgoing`) | additionally `AddTransceiver(MEDIA_TYPE_AUDIO, init{direction = kRecvOnly})` to pre-declare the peer's send slot |
| answerer | **nothing extra** |

Exchange: caller offers m0=sendonly, m1=recvonly. Answerer mints a recvonly transceiver for m0
(HasRecv false, search skipped) and binds its AddTrack-created send transceiver to m1 (HasRecv
true, search runs, flag present). Answer is m0=recvonly, m1=sendonly. Both directions live after
one exchange.

**Two traps, both of which we hit while deriving this:**

- **Never pre-create a recvonly transceiver on the answerer.** It can never be matched (the
  peer's sendonly section skips the search), so it dangles unassociated and adds a dead m-line
  to every subsequent offer.
- **`AddTrack` is a prerequisite for the directional design, not an alternative to it.** The
  answerer's send transceiver is bound only via the offerer's recvonly section, which is gated
  on the flag.

The recv-slot pre-declaration belongs to whoever is *making the offer*, not whoever placed the
call — relevant for the video path at `:1395-1414`, which is renegotiated mid-call by whichever
side toggles its camera. Pre-declaring a recv video slot is speculative; audio-only-directional
is the recommended scope.

For 18.0.0/19.0.0 this needs an ABI addition: `CallCoreABI.h:122` exposes only
`pc_add_transceiver`, so the send side needs a `pc_add_track` command or an `addTrack` flag.

## 2.3 The v2wasm cores lack the state-semantics fixes

**[verified]** `ReferenceCallCore` (18.0.0/19.0.0) contains none of the 20 s watchdog,
non-terminal ICE-failed handling, 2 s debounce, or caller-side `RestartIce`, and never emits
`pc_restart_ice`. **The A/B that claims to isolate "the substrate alone" is therefore also
varying state semantics** — which defeats its stated purpose more directly than anything else
in this document.

## 2.4 Renegotiation is unserialized — every outgoing call offers twice

**[verified]** `CreateDataChannelOrError` (`:687`) fires `OnRenegotiationNeeded()` synchronously
(`pc/sdp_offer_answer.cc:3294`); the adapter posts it to the media thread. `start()` reaches
`beginSignaling()` (`:1011-1017`), sets `_didBeginNegotiation` and issues SLD #1; the posted task
then passes the gate at `:490-492` and issues SLD #2. WebRTC's spec-compliant suppression
(`ShouldFireNegotiationNeededEvent`) does not apply because the code overrides the **legacy**
`OnRenegotiationNeeded()` at `:178-182`. The `is_negotiation_needed_` latch bounds it at exactly
one duplicate. `InstanceV2CompatImpl` serializes properly with
`_isRenegotiating`/`_pendingRenegotiation`.

**[measured]** `Ingoring remote sdp` (source typo included) in 174/400 v1100 logs vs 0/400
v1300; 158,618 of 1,034,548 gzipped outbound signalling bytes = **15.3%**, injected into the
SCTP signalling association during setup. Clean causal control: 0/200 callees call
`SetLocalDescription` before their first inbound SDP despite firing the identical event — the
only difference is the `isOutgoing` gate at `:491`.

## 2.5 SDP observer errors are bound and never read

**[verified]** Both completion lambdas (`:961-974`, `:1213-1228`) bind `webrtc::RTCError error`
and never read it, so the success path runs on failure. The forwarding observer at `:110-130`
does deliver the real error — the information is present and discarded.

Worst consequence: a failed `SetRemoteDescription(offer)` leaves signaling state at `kStable`,
and the completion unconditionally calls `sendLocalDescription()`, whose implicit
`SetLocalDescription` overload picks offer-vs-answer from `signaling_state()`
(`pc/sdp_offer_answer.cc:1617-1624`: kStable -> DoCreateOffer). The peer receives an offer where
it awaits an answer, drops it via the glare path, and **both peers sit in `have-local-offer` with
neither retransmitting** — renegotiation is deadlocked for the rest of the call and the only
ICE-restart path is disabled. Trigger unobserved in 400 endpoints. Fix is two capture lists.

## 2.6 Incoming video sink is registered raw and never removed

**[verified]** `connectIncomingVideoSink` hands `_currentStrongSink.get()` to
`VideoTrack::AddOrUpdateSink` (`:1492`), which stores it as an unowned raw pointer in
`rtc::VideoBroadcaster`. `RemoveSink` is never called anywhere in the file, and
`disconnectIncomingVideoSink()` — the function `onTransceiverRemoved` (`:598`) calls to do
exactly that — is an empty body:

```cpp
// v2/InstanceV2ReferenceImpl.cpp:1496
void disconnectIncomingVideoSink() {
}
```

Meanwhile `setIncomingVideoOutput` (`:1499`) promotes the caller's `weak_ptr` parameter into a
strong `shared_ptr` member (`:1720`), so the engine is the sink's owner of last resort. Every
point that drops that reference — the reassignment at `:1500`, `_currentStrongSink.reset()` as
the first destructor statement at `:389` (ahead of `_peerConnection = nullptr` at `:401`), or the
app releasing a sink the engine already displaced — frees an object whose address is still live
in `sink_pairs()`. `VideoBroadcaster::OnFrame` then does virtual dispatch through a dangling
vtable on the video decode thread.

**The correct construct is sitting dead in the same file.** `VideoSinkImpl` at `:242-286` is a
verbatim copy of the weak-sink indirection that makes 13.0.0 structurally immune
(`InstanceV2Impl.cpp:710-745`, `_currentSink` as `std::weak_ptr` at `:2272`) — pasted in and then
bypassed. Prefer reviving it over the three-part minimal patch. The same shape exists in
`InstanceV2CompatImpl.cpp:1243-1261` and `v2wasm/CallCoreHost.cpp:1622`.

## 2.7 PeerConnection-creation failure is swallowed — but is unreachable absent a bad server

**[verified]** `:677-680` has no else branch and no early return:

```cpp
auto peerConnectionOrError = _peerConnectionFactory->CreatePeerConnectionOrError(...);
if (peerConnectionOrError.ok()) {
    _peerConnection = peerConnectionOrError.value();
}
if (_peerConnection) { ... }
```

Twelve sites then dereference `_peerConnection` unguarded (`:491`, `:907`, `:912`, `:976`,
`:995`, `:1202`, `:1230`, `:1238`, `:1241`, `:1396`, `:1414`, `:1533`), and `start()` continues
past the guarded block to `beginSignaling()` (`:746`) and two self-rescheduling timers
(`:748`, `:750`).

**Reachability was traced exhaustively.** `CreatePeerConnectionOrError` has exactly five failure
modes; four are statically impossible in this configuration:

| failure mode | reachable | why not |
|---|---|---|
| `sdp_semantics == kPlanB_DEPRECATED` | no | explicitly `kUnifiedPlan` at `:639` |
| `ValidateIceConfig` (5 ICE timing bounds) | no | none of the five knobs is ever set; all take valid `_or_default()` values |
| null `dependencies.allocator` | no | provided at `:629` |
| null `dependencies.observer` | no | provided |
| `ParseIceServersOrError` | **yes** | empty TURN username/password only |

The surviving path is `pc/ice_server_parsing.cc:267` — and note that
`ParseIceServersOrError` returns on the **first** failure (`:333-336`), so one malformed entry
kills the whole list with no degradation to the good servers. Two protocol routes reach it:

- Classic `phoneConnection`: `username` is the literal `"reflector"`
  (`OngoingCallContext.swift:86`) so it cannot be empty, but `password` is
  `hexString(reflector.peerTag)` and `hexString(Data())` is `""`. `peerTag` is carried raw from
  the TL `bytes` field (`CallSessionManager.swift:334-336`) with **no length or emptiness
  validation anywhere**.
- `phoneConnectionWebrtc` with the turn flag (bit 0) and an empty credential
  (`CallSessionManager.swift:346-347` -> `OngoingCallContext.swift:100`). The STUN-only sibling
  is safe — the empty-credential check lives only in the TURN/TURNS branch.

**[verified]** `_peerConnection` is written in exactly two places — `:679` (success) and `:401`
(destructor) — so it can never become null mid-life; there is no teardown race, and both timers
additionally guard with `weak.lock()`. **[measured]** `Adding audio transceiver in response to a
call to AddTransceiver` (`peer_connection.cc:1164`, only emissible by a live PeerConnection)
appears in 400/400 endpoints, and `ICE server parsing failed` in 0/1600 across all four cohorts.

So this is a **server-trust bug, not a protocol-shape bug**: latent, zero observed rate, but
fleet-simultaneous if a server ever emits the degenerate value. The real fix is the input filter
(skip the bad entry, matching the per-entry rejection idiom three lines above at `:652-656`), not
just the missing `else` — the filter lets the call proceed on the remaining servers.

---

# Part 3 — Custom networking stack vs the PeerConnection default

`InstanceV2ReferenceImpl` hand-builds a port allocator and injects it rather than letting
`PeerConnectionFactory` construct the default (`:624-628`).

## 3.1 DEAD HYPOTHESIS: the injected allocator does not lose configuration

**[verified] Stop looking for a forgotten `set_*` call.**
`PeerConnection::InitializePortAllocator_n` (`pc/peer_connection.cc:2116-2175`) runs
unconditionally for injected **and** default allocators — its own comment at `:2123` says so —
applying `Initialize()`, the IPv6/shared-socket/TCP flags, `set_step_delay(kMinimumStepDelay)`,
`SetCandidateFilter`, `set_max_ipv6_networks`, the TLS cert verifier, and the full
`SetConfiguration`. `SetNetworkIgnoreMask`/`SetVpnList` are applied outside the
`if (!allocator)` block (`pc/peer_connection_factory.cc:256-257`). The only two calls confined to
the default branch, `SetPortRange` and `set_flags`, are provably no-ops — both stock defaults
are 0.

Threading and lifetime were also examined and cleared: `PortAllocator`'s constructor ends with
`thread_checker_.Detach()`, `BasicNetworkManager` binds only in `StartUpdating()` on the network
thread, `BasicPacketSocketFactory` has no sequence checker, and stock builds the same objects on
the signaling thread. Destruction order is correct twice over — `_peerConnection = nullptr` at
`:401` precedes every member destructor, and the declaration order at `:1704-1711` would make it
correct anyway.

## 3.2 Capability loss in the ICE-server mapping loop

**[verified]** `InstanceV2ReferenceImpl.cpp:645-676`:

```cpp
if (server.isTcp) { continue; }                      // :648  TCP servers dropped outright
rtc::SocketAddress address(server.host, server.port);
if (!address.IsComplete()) { ...; continue; }        // :652  hostname servers dropped
```

`IsComplete()` is `!IPIsAny(ip_) && port_ != 0` (`rtc_base/socket_address.cc:78-80`), and
`SetIP(hostname)` leaves `ip_` unset for non-literals (`:105-111`). **So a DNS-named relay never
reaches the configuration — 11.0.0/14.0.0/18.0.0/19.0.0 cannot use Cloudflare TURN at all**,
since it arrives as `turn.cloudflare.com:3478`. `InstanceV2Impl` has no such gate
(`NativeNetworkingImpl.cpp:621-627` lets `TurnPort::ResolveTurnAddress` do the DNS).

Related, same class: `descriptor.proxy` is stored at `:366` and never read again — a proxied
call ignores the proxy and keeps leaking direct UDP, where `NativeNetworkingImpl.cpp:604-610`
forces relay-only. `descriptor.config.customParameters` is never parsed, so the whole
`network_*` tuning surface is inert.

## 3.3 PeerConnection forces shared-socket, turning every reflector into a "STUN server"

**[verified]** `PORTALLOCATOR_ENABLE_SHARED_SOCKET` is set unconditionally by
`pc/peer_connection.cc:2126` on any allocator; `NativeNetworkingImpl.cpp:589-594` sets it only
behind the off-by-default `network_enable_shared_socket` parameter. Two consequences unique to
the PeerConnection engines: all reflector ports on a network share one UDP socket with the host
port and use a `ReflectorPort` constructor nothing else in production exercises; and every UDP
reflector is auto-promoted into the STUN server set (`p2p/client/basic_port_allocator.cc:1720-1735`)
and sent real STUN Binding Requests out of the reflector session's own 5-tuple. Those can never
parse — the reflector expects a 16-byte peer tag first.

**[measured]** `AllocationSequence: UDPPort will be handling the STUN candidate generation` in
374/400 ReferenceImpl endpoints and 0/400 `InstanceV2Impl` endpoints; `srflx` appears **0 times
in all 800**. The waste is proven. Mitigation is one line —
`WebRTC-UseTurnServerAsStunServer/Disabled/` in the field-trial string.

**Open question, unanswerable from client logs:** whether the reflector penalises or rate-limits
a source sending it junk. A `tgcalls_cli` run against a real reflector with and without the flag
settles it.

---

# Part 4 — A/B comparability

**The recurring finding across both investigations is not any single defect: it is that the arms
are not measurable against each other.** If these engines exist to be A/B arms, comparability is
the product, and it is what is currently broken.

## 4.1 Structural telemetry asymmetries

| field | reflector arm | Cloudflare arm | cause |
|---|---|---|---|
| `packet_overhead_bytes` | 8 (247/247) | 28 (168/168) | Part 1.2 — unresolved candidate IP |
| route line, **remote** `turn:` | 0 (247/247) | 1 | reflector wins are always on a peer-reflexive remote candidate, so `remote_candidate().is_relay()` is false |
| `"network": []` | — | — | Part 2.1 — ~77% of failing 11.0.0 calls |

The route line carries **two** `turn:` tokens (local and remote); local is `1` in both arms. Any
server-side metric keyed on BWE/bitrate, or on "was this a relay call", is comparing
incommensurable quantities. Key such metrics on an explicit `relay_backend` field emitted by the
client instead.

## 4.2 The Cloudflare cohorts are not time-matched

**[measured]** `AbOurOnly` is 163/200 calls on Jul-29; `AbCfOnly3` is 193/200 on Jul-28, and the
hour profiles barely overlap (reflector spans all 24 h; Cloudflare sits 11:00-19:00). Under
day-matching the headline relay-candidate deficit **disappears** — 68.9% vs 68.1%, z=+0.15,
against -5.0 pp unmatched. Per-network relay availability is an exact dead heat, 55.0% vs 55.0%;
per relay *port* Cloudflare is better, 54.8% vs 45.9%.

The within-arm day-to-day swing (reflector binding-response rate 75.0% on Jul-28 -> 42.1% on
Jul-29) is **larger than the between-arm difference under investigation**. Client build mix also
differs (`processSignalingData` at `InstanceV2Impl.cpp:1713` vs `:1692`), and the older build is
worse in both arms. Log lifetime is imbalanced too (Cloudflare median 8.1 s vs reflector 10.9 s),
so crude funnels overstate the Cloudflare penalty.

**Conclusion: no relay-backend conclusion should be drawn from that A/B, and no relay-side code
should ship on its strength.** Fix the experiment first: randomize per call (hash of call id)
rather than per window, export a sample of successful calls, and pin or record the client build.

## 4.3 Asymmetric observability inflates any Cloudflare hypothesis

Stock `turn_port.cc` narrates DNS, the 401 challenge, allocation and permissions with STUN
transaction ids and `rtt=` fields. `ReflectorPort` emits one hello line and has no transaction
id, no error path, no timeout and no give-up — it just pings forever. So Cloudflare failures
present as named buckets while the reflector's equivalent silent failures are invisible.
**Comparing named Cloudflare buckets against unnamed reflector silence is the single easiest way
to overstate a Cloudflare hypothesis**, and it happened repeatedly during the investigation
before being caught.

## 4.4 Instrumentation that would make the next round decisive

- **`RELAY_PORT_OUTCOME`** — one terminal line per relay port, identical schema in both backends:
  `backend=<reflector|turn> net=<iface> family=<v4|v6> server=<ip:port>
  outcome=<ready|silent|dns_failed|error> t_first_send=<ms> t_ready=<ms> datagrams_sent=<n>
  responses=<n>`. This single line makes the two arms bucket-comparable for the first time.
- **`ReflectorPort` hello sequence numbers** and an explicit give-up line. Without a sequence
  number it is impossible to distinguish "the retransmit got through" from "the reply to hello
  #1 arrived late" — that ambiguity is what made the retransmit-ladder hypothesis unfalsifiable.
- **`NET_CENSUS` at fixed t = 2/5/8 s** regardless of state, so funnels are immune to the log-length
  imbalance rather than needing censoring gymnastics.
- **`NET_TEARDOWN reason=<timeout|user_hangup|peer_hangup|error|connected>`** — logs currently
  hard-truncate with no marker, so "never connected" and "log ended" are indistinguishable.

---

# Part 5 — Examined and killed

Do not chase these again. Each was pursued and refuted with data.

- **The retransmit ladder** ("6 Cloudflare tries per 10 s vs the reflector's 20"). Remapping all
  423 Cloudflare allocate chains onto a flat 500 ms grid changes the outcome by +0.00 pp at 8 s
  and 10 s. The reverse experiment already ran: 1,182 reflector ports sent >=6 hellos and never
  became ready, 440 of them on endpoints where a sibling port did. The founding assumption —
  iid per-datagram loss — is falsified: fitted p=0.222/0.237 predicts 6 consecutive drops in 0.26
  of 2,219 and 0.11 of 621 ports; observed 1,182 and 148. Loss is bimodal; the stuck population
  is dead paths, not lossy ones.
- **Path quality / anycast detour.** Cloudflare is *faster* on every leg once up: first ICE
  binding-response RTT median 227 ms vs 742 ms; relay-edge RTT 132 ms for two round trips vs
  221 ms for the reflector's one.
- **Network-mix confound.** Standardising both arms to the pooled mix moves ICE-connection
  creation by 0.3 pp. Inert.
- **TURN Refresh.** Cloudflare grants a lifetime scheduling the first refresh at 540000 ms
  (byte-identical in all 423 occurrences), ~50x any log lifetime.
- **Candidate priority asymmetry.** `ReflectorPort::AddAddress` is byte-identical in argument
  shape to `turn_port.cc`'s; observed differences are entirely the `network_cost` term.
- **`skip_relay_to_non_relay_connections:true`.** Present in 400/400 logs of *both* arms, fires
  0/800 — with zero host and zero srflx candidates it never removes anything.
- **DTLS role deadlock.** Role pairs are 17/14 and 17/7; 0 same-role in either arm. Note the
  trap: the `active=1` token in `Started DTLS handshake active=1` is `IsDtlsActive()`
  (DTLS-is-enabled), 1 in 150/150 lines corpus-wide — it is not a role.
- **`ice_candidate_pool_size` = 0.** Looked strong on timing evidence; refuted outright.
- **The `writeStateLogRecords` `Call*` use-after-free.** The documented fix is present and
  airtight — `_isStopped` at `:1718` is set at `:1532` before `Close()` at `:1533`, and
  `PeerConnection::Close` resets `call_` inside a worker `BlockingCall` so the reset queues
  behind any running log task. A deliberate hunt found no siblings.

---

# Part 6 — Calibration

- The `AbV1100SctpFast` corpus is from commit **212a874**, not HEAD. It predates the 20 s
  watchdog, the non-terminal ICE-failed handling, the 2 s debounce and caller-side `RestartIce`.
  **Absence of those markers is not evidence about current code.**
- `rtt=` in STUN lines is measured from the **last** send (`tstamp_` set in `SendInternal`,
  `stun_request.cc:272`), not the first.
- Redaction: last octets are `x`. Cloudflare's `Conn[...]` prints the remote relayed IP redacted
  while the signalling line prints it in full, and asymmetrically between arms — build any
  identity-keyed statistic from the signalling line, not `Conn[]`.
- No server-side data exists for any of this. Whether a silent ALLOCATE ever reached Cloudflare,
  was rate-limited, was ACL-dropped, or which PoP served it, is unanswerable from client logs.
