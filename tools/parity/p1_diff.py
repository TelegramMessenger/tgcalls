#!/usr/bin/env python3
"""p1_diff.py RUN_A.log RUN_B.log — compare offer/answer SDP at the JSON layer.
Extracts outbound signaling JSON from stock ('sendSignalingMessage: ') and
pump ('[core] signaling out: ') log lines; normalizes per-run-random SDP
lines; MATCH required for offer and answer."""
import json, re, sys

PATTERNS = [re.compile(r"sendSignalingMessage: (\{.*)$"),
            re.compile(r"\[core\] signaling out: (\{.*)$")]
STRIP = re.compile(r"^(o=|a=ice-ufrag|a=ice-pwd|a=fingerprint|a=ssrc|a=msid-semantic|a=candidate)")

def extract(path):
    out = {}
    for line in open(path, errors="replace"):
        for pat in PATTERNS:
            m = pat.search(line)
            if not m:
                continue
            try:
                msg = json.loads(m.group(1))
            except ValueError:
                continue
            t = msg.get("@type")
            if t in ("offer", "answer") and t not in out:
                sdp = msg.get("sdp", "")
                out[t] = "\n".join(l for l in sdp.split("\r\n") if l and not STRIP.match(l))
    return out

a, b = extract(sys.argv[1]), extract(sys.argv[2])
ok = True
for t in ("offer", "answer"):
    if a.get(t) and a.get(t) == b.get(t):
        print(f"{t}: MATCH")
    else:
        ok = False
        print(f"{t}: MISMATCH")
        for name, d in (("A", a), ("B", b)):
            print(f"--- {name} ---")
            print(d.get(t, "<missing>")[:2000])
sys.exit(0 if ok else 1)
