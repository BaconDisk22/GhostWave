#!/usr/bin/env python3
"""clientlink_e2e.py: the client-link scenario run by clientlink_e2e.sh.

  clientlink_e2e.py WORKDIR A_ADDR B_ADDR C_ADDR

Three ghostwaved nodes on the virtual radio (A 0 km, B 1.5 km, C 3 km), each
with --client-tcp. Drives them only through the client link (gwclient.py):
handshake, contact exchange (what a QR scan does), a DM from A's client to B's
client with a delivery receipt keyed by client_msg_id, a DM to C while C has no
client (queued, then replayed on connect), an SOS that B's client sees and
acks, and the error paths. Exits nonzero on the first failed check.
"""
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gwclient as g  # noqa: E402

work, addr_a, addr_b, addr_c = sys.argv[1:5]
t0 = time.time()
failed = []


def say(msg):
    print("[%3ds] %s" % (time.time() - t0, msg), flush=True)


def check(cond, what):
    say(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        failed.append(what)
    return cond


def must(cond, what):
    if not check(cond, what):
        say("stopping: later steps depend on this")
        sys.exit(1)


def log_has(node, text):
    try:
        with open(os.path.join(work, node + ".log"), errors="replace") as f:
            return text in f.read()
    except OSError:
        return False


def raw(addr):
    host, _, port = addr.rpartition(":")
    s = socket.create_connection((host, int(port)), timeout=5)
    return s


_raw = {}


def raw_reply(s, timeout=5):
    """Next message on a raw socket, or None on timeout / close."""
    r, pending = _raw.setdefault(s, (g.FrameReader(), []))
    end = time.time() + timeout
    while not pending and time.time() < end:
        s.settimeout(max(0.1, end - time.time()))
        try:
            data = s.recv(4096)
        except socket.timeout:
            break
        if not data:
            return None
        r.push(data)
        pending.extend(g.decode_from_radio(body) for _, body in r.frames())
    return pending.pop(0) if pending else None


# ------------------------------------------------------------- handshake --
a = g.Client(addr_a, "e2e A")
b = g.Client(addr_b, "e2e B")
ia, ib = a.info, b.info
say(g.fmt(ia))
say(g.fmt(ib))
must(ia["protocol_version"] == 1 and ia["node_id"] and len(ia["ed25519_pub"]) == 32 and len(ia["x25519_pub"]) == 32,
     "A: Hello -> NodeInfo with version, node ID and both public keys")
check(ia["name"] == b"A" and ib["name"] == b"B", "NodeInfo carries the IDENT name")
check(ia["max_text_bytes"] == 73, "NodeInfo.max_text_bytes = 73")

# C: read its card, then disconnect so C has no client
c = g.Client(addr_c, "e2e C (card only)")
ic = c.info
c.close()
say(g.fmt(ic))

# ---------------------------------------------------------- error paths --
s = raw(addr_a)
s.sendall(g.frame(g.encode_to_radio("get_status", 9)))
m = raw_reply(s)
check(m and m["kind"] == "error" and m["code_name"] == "NEED_HELLO" and m["req_id"] == 9,
      "request before Hello -> NEED_HELLO, req_id echoed")
s.sendall(g.frame(g.encode_to_radio("hello", 10, protocol_version=1)))
m = raw_reply(s)
check(m and m["kind"] == "node_info", "raw Hello -> NodeInfo")
s.sendall(g.frame(bytes([0x08, 11]) + g._tag(99, 2) + b"\x00"))  # a request from the future
m = raw_reply(s)
check(m and m["code_name"] == "UNKNOWN_REQUEST" and m["req_id"] == 11, "unknown request -> UNKNOWN_REQUEST")
s.sendall(b"\x94\xc3\xff\x00" + g.frame(g.encode_to_radio("get_position", 12)))  # bad length, then a good frame
m1, m2 = raw_reply(s), raw_reply(s)
got = [x for x in (m1, m2) if x]
check(any(x["kind"] == "error" and x["code_name"] == "BAD_FRAME" for x in got) and
      any(x["kind"] == "position" and x["req_id"] == 12 for x in got),
      "oversized frame length -> BAD_FRAME, stream resyncs on the next frame")
s.sendall(g.frame(g.encode_to_radio("get_status", 13), version=2))
m = raw_reply(s)
check(m and m["code_name"] == "BAD_VERSION", "frame version 2 -> BAD_VERSION")
check(raw_reply(s, 3) is None, "...and the node closes the connection")
s.close()

r = a.request("send_text", dest=ib["node_id"], text=b"x" * 74, client_msg_id=1)
check(r[0]["kind"] == "error" and r[0]["code_name"] == "TOO_LONG", "74-byte text -> TOO_LONG")
r = a.request("add_contact", node_id=ib["node_id"], ed25519_pub=ic["ed25519_pub"])
check(r[0]["code_name"] == "KEY_REJECTED", "contact key that does not hash to the ID -> KEY_REJECTED")
r = a.request("sos_ack", node_id=0x12345678)
check(r[0]["code_name"] == "NOT_FOUND", "SOS ack with no SOS held -> NOT_FOUND")

# ---------------------------------------------- contacts (the QR exchange) --
for cl, other, nm in ((a, ib, "Bee"), (b, ia, "Ay"), (a, ic, "Cee")):
    r = cl.request("add_contact", node_id=other["node_id"], ed25519_pub=other["ed25519_pub"], name=nm)
    must(r[0]["kind"] == "ok", "add contact %08x (%s)" % (other["node_id"], nm))
r = a.request("list_contacts")
ct = {m["node_id"]: m for m in r if m["kind"] == "contact"}
check(r[-1]["kind"] == "contacts_end" and r[-1]["count"] == len(ct) >= 2, "ListContacts -> Contact..., ContactsEnd")
cb = ct.get(ib["node_id"])
check(cb is not None and cb["pinned"] and cb["ed25519_pub"] == ib["ed25519_pub"] and cb["name"] == b"Bee"
      and cb["x25519_pub"] == ib["x25519_pub"], "B listed as pinned, with its keys and our label")
# C learns A's key the same way, through a short-lived client
c = g.Client(addr_c, "e2e C (contact)")
must(c.request("add_contact", node_id=ia["node_id"], ed25519_pub=ia["ed25519_pub"])[0]["kind"] == "ok",
     "C pins A's key")
c.close()

# ----------------------------------------------------- position requests --
r = a.request("get_position")
check(r[0]["kind"] == "position" and r[0]["pos"] and abs(r[0]["pos"][0] - 46.87) < 1e-6, "GetPosition -> A's fix")
r = a.request("set_known_position", node_id=ib["node_id"], pos=(46.87 + 1.5 * 0.0089932, -113.99))
check(r[0]["kind"] == "ok", "SetKnownPosition for B")

# -------------------------------------------- DM A -> B, receipt to A --
time.sleep(3)   # start-up beacons
CMID = 4242
r = a.request("send_text", dest=ib["node_id"], text="hello B, from A's app".encode(), client_msg_id=CMID)
must(r[0]["kind"] == "send_queued" and r[0]["client_msg_id"] == CMID and r[0]["msg_id"],
     "SendText -> SendQueued with the mesh msg_id")
mid = r[0]["msg_id"]
e = b.wait_event(lambda e: e["kind"] == "text" and e["src"] == ia["node_id"], 60)
check(e is not None and e["text"] == b"hello B, from A's app" and e["msg_id"] == mid,
      "B's client got the DM (msg_id %08x)" % mid)
e = a.wait_event(lambda e: e["kind"] == "delivered" and e["msg_id"] == mid, 60)
check(e is not None and e["client_msg_id"] == CMID and e["dest"] == ib["node_id"],
      "A's client got Delivered with client_msg_id %d" % CMID)
check(not log_has("A", "hello B, from A") and log_has("B", "MSG from %08x" % ia["node_id"]),
      "stdout logging unchanged: B's log prints the MSG line")

# ------------------------------- DM A -> C while C has no client: replay --
# C is 3 km out, past A's reliable range: give A its position (as a contact
# card would), so the DM goes geographic via B instead of waiting in spray mode
a.request("set_known_position", node_id=ic["node_id"], pos=(46.87 + 3 * 0.0089932, -113.99))
r = a.request("send_text", dest=ic["node_id"], text=b"for C, held until its app connects", client_msg_id=7)
must(r[0]["kind"] == "send_queued", "SendText to C queued")
end = time.time() + 90
while time.time() < end and not log_has("C", "MSG from %08x" % ia["node_id"]):
    time.sleep(0.5)
must(log_has("C", "MSG from %08x" % ia["node_id"]), "C's daemon received the DM (no client connected)")
c = g.Client(addr_c, "e2e C (late)")
e = c.wait_event(lambda e: e["kind"] == "text", 5)
check(e is not None and e["replayed"] and e["text"] == b"for C, held until its app connects",
      "C's client got the held DM on Hello, flagged replayed")
st = c.request("get_status")[0]
check(st["kind"] == "status" and st["queued"] == 0 and st["clients"] == 1, "C status: queue drained, 1 client")
c.close()

# -------------------------------------------- SOS from A's client, B acks --
r = a.request("sos_start", text=b"e2e: twisted ankle", battery_pct=55, accuracy_m=10)
must(r[0]["kind"] == "ok", "SosStart -> Ok")
e = b.wait_event(lambda e: e["kind"] == "sos" and e["src"] == ia["node_id"], 70)
must(e is not None, "B's client got the SOS")
check(e["text"] == b"e2e: twisted ankle" and e["battery_pct"] == 55 and e["accuracy_m"] == 12 and e["pos"]
      and abs(e["pos"][0] - 46.87) < 1e-4, "SOS carries text, battery, accuracy (4 m steps) and position")
r = b.request("sos_ack", node_id=ia["node_id"])
check(r[0]["kind"] == "ok", "B's client acks the SOS")
e = a.wait_event(lambda e: e["kind"] == "sos_ack" and e["responder"] == ib["node_id"], 60)
check(e is not None, "A's client got SosAckReceived from B")
st = a.request("get_status")[0]
check(st["sos_active"] and st["tx"] > 0 and st["clients"] == 1, "A status: SOS active, frames sent")
check(a.request("sos_stop")[0]["kind"] == "ok" and not a.request("get_status")[0]["sos_active"], "SosStop")
r = a.request("sos_start", text=b"again")
check(r[0]["code_name"] == "RATE_LIMITED", "second SOS within a minute -> RATE_LIMITED")

# ---------------------------------------------------- other live events --
nb = [e for e in a.events + b.events if e["kind"] == "neighbor"]
say("neighbor events seen: %d (beacons are minutes apart; not required)" % len(nb))
for e in nb[:4]:
    say("  " + g.fmt(e))

a.close()
b.close()
print()
if failed:
    say("FAIL: %d check(s) failed: %s" % (len(failed), "; ".join(failed)))
    sys.exit(1)
say("PASS: client link end to end (handshake, contacts, DM + receipt, offline replay, SOS + ack, errors)")
