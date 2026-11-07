#!/usr/bin/env python3
"""gwclient.py: reference client for the GhostWave client link (stdlib only).

Speaks docs/CLIENT_PROTOCOL.md to `ghostwaved --client-tcp [HOST:]PORT`:
frames of 0x94 0xC3 len_lo len_hi | version | protobuf (proto/ghostwave.proto).
The protobuf codec below is hand-written for exactly that schema, so this
script needs no protoc and no pip packages. It is also a worked example for
the Flutter app.

Usage:
  gwclient.py [HOST:]PORT info                 node ID, name, keys (what a QR code carries)
  gwclient.py [HOST:]PORT status | pos | contacts
  gwclient.py [HOST:]PORT send NODE TEXT       DM; waits up to --wait s for the receipt
  gwclient.py [HOST:]PORT add [NODE] ED25519_HEX [NAME]
  gwclient.py [HOST:]PORT known NODE LAT LON
  gwclient.py [HOST:]PORT setpos LAT LON       our own position (phone GPS)
  gwclient.py [HOST:]PORT sos TEXT | sosstop | sosack NODE
  gwclient.py [HOST:]PORT listen               print events until Ctrl-C (or --wait s)
Import it for scripted use: see clientlink_e2e.py.
"""
import argparse
import select
import socket
import struct
import sys
import time

MAGIC = b"\x94\xc3"
VERSION = 1
MAX_BODY = 176  # version byte + protobuf

# ----------------------------------------------------------------- schema --
# field number -> (name, type). Types: u32 u64 bool sint fx32 bytes str
# opt (proto3 optional uint32, None = absent), pos (Position sub-message,
# None = absent), and enum names map to u32.

POSITION = {1: ("lat_e7", "sint"), 2: ("lon_e7", "sint")}

TO_REQUESTS = {
    10: ("hello", {1: ("protocol_version", "u32"), 2: ("client_name", "str")}),
    11: ("send_text", {1: ("dest", "fx32"), 2: ("text", "bytes"), 3: ("client_msg_id", "u32")}),
    12: ("add_contact", {1: ("node_id", "fx32"), 2: ("ed25519_pub", "bytes"),
                         3: ("x25519_pub", "bytes"), 4: ("name", "str")}),
    13: ("list_contacts", {}),
    14: ("set_known_position", {1: ("node_id", "fx32"), 2: ("pos", "pos")}),
    15: ("sos_start", {1: ("text", "bytes"), 2: ("battery_pct", "opt"), 3: ("accuracy_m", "opt")}),
    16: ("sos_stop", {}),
    17: ("sos_ack", {1: ("node_id", "fx32")}),
    18: ("get_status", {}),
    19: ("get_position", {}),
    20: ("set_own_position", {1: ("pos", "pos")}),
}

FROM_EVENTS = {
    10: ("node_info", {1: ("protocol_version", "u32"), 2: ("node_id", "fx32"), 3: ("name", "bytes"),
                       4: ("ed25519_pub", "bytes"), 5: ("x25519_pub", "bytes"), 6: ("legacy", "bool"),
                       7: ("firmware", "str"), 8: ("max_text_bytes", "u32")}),
    11: ("text", {1: ("src", "fx32"), 2: ("msg_id", "fx32"), 3: ("text", "bytes"), 4: ("src_pos", "pos")}),
    12: ("delivered", {1: ("dest", "fx32"), 2: ("msg_id", "fx32"), 3: ("client_msg_id", "u32")}),
    13: ("sos", {1: ("src", "fx32"), 2: ("seq", "u32"), 3: ("pos", "pos"), 4: ("accuracy_m", "opt"),
                 5: ("battery_pct", "opt"), 6: ("text", "bytes")}),
    14: ("sos_ack", {1: ("responder", "fx32"), 2: ("seq", "u32")}),
    15: ("neighbor", {1: ("node_id", "fx32"), 2: ("pos", "pos"), 3: ("is_new", "bool")}),
    16: ("status", {1: ("tx", "u32"), 2: ("rx", "u32"), 3: ("rx_bad", "u32"), 4: ("rx_auth_fail", "u32"),
                    5: ("delivered", "u32"), 6: ("forwarded", "u32"), 7: ("stored", "u32"),
                    8: ("air_ms", "u64"), 9: ("store_size", "u32"), 10: ("neighbors", "u32"),
                    11: ("sos_held", "u32"), 12: ("sos_active", "bool"), 13: ("uptime_s", "u32"),
                    14: ("clients", "u32"), 15: ("queued", "u32")}),
    17: ("position", {1: ("pos", "pos")}),
    18: ("error", {1: ("code", "u32"), 2: ("detail", "str")}),
    19: ("contact", {1: ("node_id", "fx32"), 2: ("name", "bytes"), 3: ("x25519_pub", "bytes"),
                     4: ("ed25519_pub", "bytes"), 5: ("pinned", "bool")}),
    20: ("contacts_end", {1: ("count", "u32")}),
    21: ("send_queued", {1: ("dest", "fx32"), 2: ("msg_id", "fx32"), 3: ("client_msg_id", "u32")}),
    22: ("ok", {}),
    23: ("peer_ident", {1: ("node_id", "fx32"), 2: ("name", "bytes"), 3: ("is_new", "bool")}),
    24: ("key_change", {1: ("node_id", "fx32"), 2: ("ed25519_pub", "bytes"), 3: ("name", "bytes")}),
}

ERRORS = ["NONE", "BAD_FRAME", "BAD_VERSION", "UNKNOWN_REQUEST", "NEED_HELLO", "BAD_ARGUMENT",
          "TOO_LONG", "RATE_LIMITED", "NOT_FOUND", "KEY_REJECTED", "SEND_FAILED",
          "TOO_MANY_CLIENTS", "NO_IDENTITY"]

# ------------------------------------------------------------- protobuf --


def _varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def _tag(f, wt):
    return _varint(f << 3 | wt)


def _encode(fields, values):
    out = bytearray()
    for f in sorted(fields):
        name, typ = fields[f]
        v = values.get(name)
        if typ == "opt":
            if v is not None:
                out += _tag(f, 0) + _varint(v)
        elif typ == "pos":
            if v is not None:  # (lat, lon) in degrees
                sub = _encode(POSITION, {"lat_e7": round(v[0] * 1e7), "lon_e7": round(v[1] * 1e7)})
                out += _tag(f, 2) + _varint(len(sub)) + sub
        elif not v:
            continue  # proto3: defaults are not sent
        elif typ in ("u32", "u64", "bool"):
            out += _tag(f, 0) + _varint(int(v))
        elif typ == "sint":
            out += _tag(f, 0) + _varint(((v << 1) ^ (v >> 31)) & 0xFFFFFFFF)
        elif typ == "fx32":
            out += _tag(f, 5) + struct.pack("<I", v)
        elif typ in ("bytes", "str"):
            b = v.encode() if isinstance(v, str) else bytes(v)
            out += _tag(f, 2) + _varint(len(b)) + b
    return bytes(out)


def _fields(buf):
    """Yields (field, wire type, value). Raises ValueError on malformed input."""
    i = 0
    n = len(buf)

    def varint():
        nonlocal i
        v = shift = 0
        while True:
            if i >= n or shift >= 70:
                raise ValueError("bad varint")
            c = buf[i]
            i += 1
            v |= (c & 0x7F) << shift
            shift += 7
            if not c & 0x80:
                return v

    while i < n:
        key = varint()
        f, wt = key >> 3, key & 7
        if f == 0:
            raise ValueError("field 0")
        if wt == 0:
            yield f, wt, varint()
        elif wt == 1:
            if i + 8 > n:
                raise ValueError("short fixed64")
            yield f, wt, buf[i:i + 8]
            i += 8
        elif wt == 5:
            if i + 4 > n:
                raise ValueError("short fixed32")
            yield f, wt, struct.unpack_from("<I", buf, i)[0]
            i += 4
        elif wt == 2:
            ln = varint()
            if i + ln > n:
                raise ValueError("length past end")
            yield f, wt, buf[i:i + ln]
            i += ln
        else:
            raise ValueError("wire type %d" % wt)


def _decode(fields, buf):
    out = {}
    for name, typ in fields.values():  # defaults
        out[name] = None if typ in ("opt", "pos") else (b"" if typ == "bytes" else
                                                       "" if typ == "str" else False if typ == "bool" else 0)
    for f, wt, v in _fields(buf):
        if f not in fields:
            continue
        name, typ = fields[f]
        want = {"fx32": 5, "bytes": 2, "str": 2, "pos": 2}.get(typ, 0)
        if wt != want:
            raise ValueError("field %s: wire type %d" % (name, wt))
        if typ == "pos":
            p = _decode(POSITION, v)
            out[name] = (p["lat_e7"] / 1e7, p["lon_e7"] / 1e7)
        elif typ == "sint":
            v &= 0xFFFFFFFF
            out[name] = (v >> 1) ^ -(v & 1)
        elif typ == "str":
            out[name] = bytes(v).decode("utf-8", "replace")
        elif typ == "bytes":
            out[name] = bytes(v)
        elif typ == "bool":
            out[name] = bool(v)
        elif typ in ("u32", "opt"):
            out[name] = v & 0xFFFFFFFF
        else:
            out[name] = v
    return out


def encode_to_radio(kind, req_id=0, **values):
    for num, (name, fields) in TO_REQUESTS.items():
        if name == kind:
            sub = _encode(fields, values)
            return _encode({1: ("req_id", "u32")}, {"req_id": req_id}) + _tag(num, 2) + _varint(len(sub)) + sub
    raise KeyError(kind)


def decode_from_radio(buf):
    """-> dict with req_id, replayed, kind (None if unknown) and the event fields."""
    msg = {"req_id": 0, "replayed": False, "kind": None}
    for f, wt, v in _fields(buf):
        if f == 1 and wt == 0:
            msg["req_id"] = v & 0xFFFFFFFF
        elif f == 2 and wt == 0:
            msg["replayed"] = bool(v)
        elif f in FROM_EVENTS:
            if wt != 2:
                raise ValueError("event as wire type %d" % wt)
            name, fields = FROM_EVENTS[f]
            msg = {"req_id": msg["req_id"], "replayed": msg["replayed"], "kind": name}
            msg.update(_decode(fields, v))
    if msg["kind"] == "error":
        c = msg["code"]
        msg["code_name"] = ERRORS[c] if c < len(ERRORS) else "ERROR_%d" % c
    return msg


def frame(pb, version=VERSION):
    if len(pb) + 1 > MAX_BODY:
        raise ValueError("message too long for a frame")
    return MAGIC + struct.pack("<H", len(pb) + 1) + bytes([version]) + pb


class FrameReader:
    def __init__(self):
        self.buf = bytearray()

    def push(self, data):
        self.buf += data

    def frames(self):
        """Yields (version, body) for complete frames; skips garbage and bad lengths."""
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                keep = 1 if self.buf[-1:] == MAGIC[:1] else 0
                del self.buf[:len(self.buf) - keep]
                return
            del self.buf[:i]
            if len(self.buf) < 4:
                return
            ln = self.buf[2] | self.buf[3] << 8
            if ln == 0 or ln > MAX_BODY:
                del self.buf[:1]
                continue
            if len(self.buf) < 4 + ln:
                return
            body = bytes(self.buf[4:4 + ln])
            del self.buf[:4 + ln]
            yield body[0], body[1:]

# ---------------------------------------------------------------- client --


class ClientError(Exception):
    pass


class Client:
    def __init__(self, addr, name="gwclient.py", timeout=5.0):
        host, _, port = addr.rpartition(":")
        self.sock = socket.create_connection((host.strip("[]") or "127.0.0.1", int(port)), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.reader = FrameReader()
        self.events = []  # unsolicited events (req_id 0), in arrival order
        self.replies = {}  # req_id -> list of messages
        self.next_req = 1
        self.info = self.request("hello", protocol_version=VERSION, client_name=name)[0]
        if self.info["kind"] != "node_info":
            raise ClientError("hello failed: %r" % self.info)

    def close(self):
        self.sock.close()

    def send(self, kind, **values):
        req = self.next_req
        self.next_req += 1
        self.sock.sendall(frame(encode_to_radio(kind, req, **values)))
        return req

    def pump(self, timeout):
        """Reads whatever arrives within `timeout` s; returns False if the node closed."""
        r, _, _ = select.select([self.sock], [], [], max(0.0, timeout))
        if not r:
            return True
        data = self.sock.recv(4096)
        if not data:
            return False
        self.reader.push(data)
        for ver, body in self.reader.frames():
            if ver != VERSION:
                raise ClientError("node sent frame version %d" % ver)
            m = decode_from_radio(body)
            if m["req_id"]:
                self.replies.setdefault(m["req_id"], []).append(m)
            else:
                self.events.append(m)
        return True

    def request(self, kind, timeout=5.0, **values):
        """Sends a request; returns its replies (a list; ends at the first final one)."""
        req = self.send(kind, **values)
        final = {"list_contacts": ("contacts_end", "error")}.get(kind)
        end = time.time() + timeout
        while time.time() < end:
            got = self.replies.get(req, [])
            if got and (final is None or got[-1]["kind"] in final):
                return self.replies.pop(req)
            if not self.pump(end - time.time()):
                raise ClientError("node closed the connection")
        raise ClientError("no reply to %s" % kind)

    def wait_event(self, pred, timeout):
        """Waits for an unsolicited event matching pred; returns it (consumed) or None."""
        end = time.time() + timeout
        while True:
            for i, e in enumerate(self.events):
                if pred(e):
                    return self.events.pop(i)
            left = end - time.time()
            if left <= 0:
                return None
            if not self.pump(min(left, 0.5)):
                raise ClientError("node closed the connection")


def fmt(m):
    """One line per message, for humans and for grepping in scripts."""
    k = m["kind"]
    tag = " (replayed)" if m.get("replayed") else ""
    if k == "text":
        return "TEXT from %08x id %08x: %s%s" % (m["src"], m["msg_id"], m["text"].decode("utf-8", "replace"), tag)
    if k == "delivered":
        return "DELIVERED to %08x id %08x client_msg_id %d%s" % (m["dest"], m["msg_id"], m["client_msg_id"], tag)
    if k == "sos":
        where = "at %.6f %.6f" % m["pos"] if m["pos"] else "no fix"
        return "SOS from %08x seq %d %s: %s%s" % (m["src"], m["seq"], where, m["text"].decode("utf-8", "replace"), tag)
    if k == "sos_ack":
        return "SOSACK from %08x seq %d%s" % (m["responder"], m["seq"], tag)
    if k == "neighbor":
        where = "at %.6f %.6f" % m["pos"] if m["pos"] else "no fix"
        return "NEIGHBOR %08x %s%s" % (m["node_id"], where, " (new)" if m["is_new"] else "")
    if k == "node_info":
        return "NODE %08x name \"%s\" ed25519 %s x25519 %s max_text %d%s" % (
            m["node_id"], m["name"].decode("utf-8", "replace"), m["ed25519_pub"].hex() or "-",
            m["x25519_pub"].hex(), m["max_text_bytes"], " legacy" if m["legacy"] else "")
    if k == "contact":
        return "CONTACT %08x \"%s\" %s ed25519 %s" % (m["node_id"], m["name"].decode("utf-8", "replace"),
                                                     "pinned" if m["pinned"] else "learned",
                                                     m["ed25519_pub"].hex() or "-")
    if k == "error":
        return "ERROR %s: %s" % (m["code_name"], m["detail"])
    if k == "position":
        return "POS %.7f %.7f" % m["pos"] if m["pos"] else "POS nofix"
    if k == "peer_ident":
        return "IDENT %s %08x \"%s\"" % ("new peer" if m["is_new"] else "renamed", m["node_id"],
                                         m["name"].decode("utf-8", "replace"))
    if k == "key_change":
        return "KEY CHANGE %08x ed25519 %s" % (m["node_id"], m["ed25519_pub"].hex())
    fields = " ".join("%s=%s" % (a, b) for a, b in m.items() if a not in ("kind", "req_id", "replayed"))
    return "%s %s" % ((k or "UNKNOWN").upper(), fields)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addr", help="[HOST:]PORT of ghostwaved --client-tcp")
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--wait", type=float, default=None, help="seconds to wait for events")
    a = ap.parse_args()
    addr = a.addr if ":" in a.addr else "127.0.0.1:" + a.addr
    c = Client(addr)
    hexid = lambda s: int(s, 16)

    def show(msgs):
        for m in msgs:
            print(fmt(m))
        return 1 if any(m["kind"] == "error" for m in msgs) else 0

    rc = 0
    cmd, args = a.cmd, a.args
    if cmd == "info":
        print(fmt(c.info))
    elif cmd == "status":
        rc = show(c.request("get_status"))
    elif cmd == "pos":
        rc = show(c.request("get_position"))
    elif cmd == "contacts":
        rc = show(c.request("list_contacts"))
    elif cmd == "send" and len(args) >= 2:
        r = c.request("send_text", dest=hexid(args[0]), text=" ".join(args[1:]).encode(), client_msg_id=1)
        rc = show(r)
        if not rc:
            msg_id = r[0]["msg_id"]
            e = c.wait_event(lambda e: e["kind"] == "delivered" and e["msg_id"] == msg_id,
                             60 if a.wait is None else a.wait)
            print(fmt(e) if e else "no delivery receipt yet")
            rc = 0 if e else 3
    elif cmd == "add" and args:
        node = hexid(args[0]) if len(args[0]) <= 8 else 0
        rest = args[1:] if node else args
        rc = show(c.request("add_contact", node_id=node, ed25519_pub=bytes.fromhex(rest[0]),
                            name=" ".join(rest[1:])))
    elif cmd == "known" and len(args) == 3:
        rc = show(c.request("set_known_position", node_id=hexid(args[0]), pos=(float(args[1]), float(args[2]))))
    elif cmd == "setpos" and len(args) == 2:
        rc = show(c.request("set_own_position", pos=(float(args[0]), float(args[1]))))
    elif cmd == "sos":
        rc = show(c.request("sos_start", text=" ".join(args).encode()))
    elif cmd == "sosstop":
        rc = show(c.request("sos_stop"))
    elif cmd == "sosack" and len(args) == 1:
        rc = show(c.request("sos_ack", node_id=hexid(args[0])))
    elif cmd != "listen":
        ap.error("unknown command or wrong arguments: %s" % cmd)
    if cmd == "listen" or (a.wait and cmd != "send"):
        end = None if a.wait is None else time.time() + a.wait
        try:
            while end is None or time.time() < end:
                while c.events:
                    print(fmt(c.events.pop(0)), flush=True)
                if not c.pump(0.5 if end is None else min(0.5, end - time.time())):
                    print("node closed the connection")
                    break
        except KeyboardInterrupt:
            pass
    while c.events:
        print(fmt(c.events.pop(0)))
    c.close()
    sys.exit(rc)


if __name__ == "__main__":
    main()
