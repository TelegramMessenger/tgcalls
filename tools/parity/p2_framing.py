#!/usr/bin/env python3
"""p2_framing.py LOG — V1 framing invariants (applies to stock AND pump logs):
every sent requiring-ack message is eventually acked, at least one ACK was
appended, and no framing/decrypt errors occurred."""
import re, sys

log = open(sys.argv[1], errors="replace").read()
sends = set(re.findall(r"(?:Add|Enqueue) SEND:type127#(\d+)", log))
acks = set(re.findall(r"Got ACK:type127#(\d+)", log))
missing = sends - acks
errors = [l for l in log.splitlines()
          if "ERROR!" in l or "Bad incoming data hash" in l or "could not decrypt signaling" in l]
print(f"sends={len(sends)} acked={len(acks & sends)} missing={sorted(missing)} "
      f"added_acks={'Add ACK#' in log} errors={len(errors)}")
for l in errors[:10]:
    print("ERR:", l.strip())
ok = sends and not missing and "Add ACK#" in log and not errors
sys.exit(0 if ok else 1)
