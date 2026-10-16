#!/usr/bin/env python3
"""GhostWave mesh simulator.

Compares, on identical mobility, traffic and PHY:
  flood3 / flood7  managed flooding with SNR-weighted rebroadcast + suppression
                   (models Meshtastic's default behaviour)
  pathlearn        fixed repeaters flood, destination returns a path, later
                   messages are source-routed (models MeshCore)
  ghostwave        contention-based geographic forwarding + DTN custody +
                   spray-and-wait + GPS slots + airtime budget (docs/DESIGN.md)
                   + SOS lane (scoped flood + 24 h epidemic store, §8)
  ghostwave-*      ablations with one pillar switched off

--sos adds SOS events (random mobile originators, re-sent every 5 min) on top of
the normal traffic and reports how long until any fixed node holds each one.
flood* sends them as a plain hop-limited flood (Meshtastic has no SOS lane).

Baselines are simplified models, not the real firmware. Treat results as
relative, not absolute. Pure stdlib; run `python3 ghostsim.py --help`.
"""
import argparse
import heapq
import math
import random
import statistics
from dataclasses import dataclass, field

# ---------------------------------------------------------------- PHY ------
SF, BW, CR, PREAMBLE = 9, 125e3, 1, 16
TX_DBM = 22.0
GAIN_MOBILE, GAIN_FIXED = 2.0, 5.0      # fixed nodes sit on masts / hilltops
SENS = -129.0                           # SX1262 SF9/BW125
NOISE = -117.0                          # -174 + 10log10(125k) + 6 dB NF
PL0, PL_EXP, FADE_SIGMA = 31.7, 3.5, 5.0
CAPTURE_DB = 6.0
INTERF_FLOOR = SENS - 6.0
TSYM = (2 ** SF) / BW
CAD_TIME = 2 * TSYM
BCAST = 0xFFFFFFFF


def airtime(nbytes):
    de = 1 if TSYM > 0.016 else 0
    n = 8 + max(math.ceil((8 * nbytes - 4 * SF + 28 + 16) / (4 * (SF - 2 * de))) * (CR + 4), 0)
    return (PREAMBLE + 4.25) * TSYM + n * TSYM


# slot timing (GhostWave)
GUARD = 0.040
SLOT = GUARD + airtime(128)
PRIO_EVERY = 8
PRIO_SOS = 2


def dist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x


# ------------------------------------------------------------ mobility -----
class Static:
    def __init__(self, x, y):
        self.p = (x, y)

    def pos(self, t):
        return self.p


class Waypoint:
    """Random waypoint with pause; deterministic given its own rng."""

    def __init__(self, rng, w, h, vmin, vmax, pause):
        self.rng, self.w, self.h, self.v, self.pause = rng, w, h, (vmin, vmax), pause
        self.a = (rng.uniform(0, w), rng.uniform(0, h))
        self.t0 = 0.0
        self._leg()

    def _leg(self):
        self.b = (self.rng.uniform(0, self.w), self.rng.uniform(0, self.h))
        self.ta = self.t0 + dist(self.a, self.b) / self.rng.uniform(*self.v)
        self.tp = self.ta + self.rng.uniform(0, self.pause)

    def pos(self, t):
        while t >= self.tp:
            self.a, self.t0 = self.b, self.tp
            self._leg()
        if t >= self.ta:
            return self.b
        f = (t - self.t0) / (self.ta - self.t0)
        return (self.a[0] + (self.b[0] - self.a[0]) * f, self.a[1] + (self.b[1] - self.a[1]) * f)


# --------------------------------------------------------------- frames ----
@dataclass(eq=False)
class Frame:
    kind: str
    src: int
    dst: int
    mid: int
    size: int
    hops: int = 0
    prio: int = 0
    meta: dict = field(default_factory=dict)
    tx: int = -1
    cancelled: bool = False
    sent: bool = False

    def copy(self, **kw):
        g = Frame(self.kind, self.src, self.dst, self.mid, self.size, self.hops,
                  self.prio, dict(self.meta), self.tx)
        for k, v in kw.items():
            setattr(g, k, v)
        return g


@dataclass(eq=False)
class Rec:
    start: float
    end: float
    p: float
    frame: Frame
    lost: bool = False


class Node:
    def __init__(self, nid, mob, fixed):
        self.id, self.mob, self.fixed = nid, mob, fixed
        self.gain = GAIN_FIXED if fixed else GAIN_MOBILE
        self.tx_until = 0.0
        self.recs = []
        self.heard = 0.0          # cumulative decodable airtime heard (utilization)
        self.mac = None

    def pos(self, t):
        return self.mob.pos(t)


# ------------------------------------------------------------------ MAC ----
class Budget:
    def __init__(self, sim, node, rate_fn, depth=8.0):
        self.sim, self.node, self.rate_fn, self.depth = sim, node, rate_fn, depth
        self.tokens, self.last = depth, 0.0

    def _refill(self):
        now = self.sim.t
        self.tokens = min(self.depth, self.tokens + (now - self.last) * self.rate_fn(self.node))
        self.last = now

    def wait_for(self, dur):
        self._refill()
        if self.tokens >= dur:
            return 0.0
        return (dur - self.tokens) / self.rate_fn(self.node) + 1e-3

    def consume(self, dur):
        self._refill()
        self.tokens -= dur


class MAC:
    """One frame on air at a time. Unslotted CSMA or GPS-slotted ALOHA + CAD."""

    def __init__(self, sim, node, slotted=False, budget=None):
        self.sim, self.node, self.slotted, self.budget = sim, node, slotted, budget
        self.q = []
        self.busy = False

    def send(self, f, now_ok=False):
        heapq.heappush(self.q, (-f.prio, self.sim.nseq(), f, now_ok))
        if not self.busy:
            self.busy = True
            self.sim.at(self.sim.t, self._next)

    def _slot_time(self, f, extra=0):
        now = self.sim.t
        k = math.ceil(now / SLOT - 1e-9) + extra
        while f.prio < PRIO_SOS and k % PRIO_EVERY == 0:
            k += 1
        return k * SLOT + self.sim.rng.uniform(0, GUARD)

    def _next(self):
        while self.q and self.q[0][2].cancelled:
            heapq.heappop(self.q)
        if not self.q:
            self.busy = False
            return
        _, _, f, now_ok = self.q[0]
        if not self.slotted or now_ok:
            self.sim.at(self.sim.t, self._try)
        else:
            self.sim.at(self._slot_time(f), self._try)

    def _retry_later(self, f):
        if self.slotted:
            self.sim.at(self._slot_time(f, self.sim.rng.randint(1, 4)), self._try)
        else:
            self.sim.at(self.sim.t + self.sim.rng.uniform(0.05, 0.6), self._try)

    def _try(self):
        while self.q and self.q[0][2].cancelled:
            heapq.heappop(self.q)
        if not self.q:
            self.busy = False
            return
        f = self.q[0][2]
        dur = airtime(f.size)
        if self.budget and f.prio < PRIO_SOS and f.meta.get("own"):
            w = self.budget.wait_for(dur)
            if w > 0:
                self.sim.at(self.sim.t + w, self._next_unforced)
                return
        if self.sim.channel_busy(self.node):
            self._retry_later(f)
            return
        heapq.heappop(self.q)
        if self.budget and f.meta.get("own"):
            self.budget.consume(dur)
        self.sim.transmit(self.node, f)
        self.sim.at(self.sim.t + dur, self._next)

    def _next_unforced(self):
        # after waiting for tokens, a frame loses its "transmit now" slot alignment
        if self.q:
            pr, s, f, _ = heapq.heappop(self.q)
            heapq.heappush(self.q, (pr, s, f, False))
        self._next()


# ------------------------------------------------------------------ sim ----
SCENARIOS = {
    # name: (width_m, n_nodes, n_fixed, vehicle_frac, msg_interval_s)
    "trail":       (8000, 50, 8, 0.10, 1200),
    "backcountry": (25000, 40, 4, 0.15, 1200),
    "busy":        (5000, 120, 10, 0.05, 600),
}
DURATION = 3 * 3600
TRAFFIC_START, TRAFFIC_END = 600, 2 * 3600 + 600
SOS_EVENTS = 6                  # per seed, when --sos is on
SOS_REPEAT, SOS_SIZE = 300, 54  # 19 hdr + 7 src_pos + 4 sos hdr + 20 text + 4 MIC


class Sim:
    def __init__(self, scenario, proto, seed, sos=False):
        w, n, nfixed, vfrac, interval = SCENARIOS[scenario]
        self.rng = random.Random(seed * 7919 + 1)
        self.t, self.q, self._seq, self._mid = 0.0, [], 0, 0
        self.nodes = []
        # fixed nodes on a jittered grid; deterministic per seed across protocols
        mrng = random.Random(seed)
        g = math.ceil(math.sqrt(nfixed))
        for i in range(nfixed):
            cx, cy = (i % g + 0.5) * w / g, (i // g + 0.5) * w / g
            self.nodes.append(Node(i, Static(cx + mrng.uniform(-0.15, 0.15) * w / g,
                                             cy + mrng.uniform(-0.15, 0.15) * w / g), True))
        for i in range(nfixed, n):
            r = random.Random(seed * 100003 + i)
            if r.random() < vfrac:
                mob = Waypoint(r, w, w, 8, 15, 900)
            else:
                mob = Waypoint(r, w, w, 1.0, 1.6, 1200)
            self.nodes.append(Node(i, mob, False))
        # traffic, identical across protocols
        trng = random.Random(seed * 31 + 7)
        self.msgs = {}
        for nd in self.nodes:
            t = TRAFFIC_START + trng.expovariate(1 / interval)
            while t < TRAFFIC_END:
                dst = trng.randrange(n - 1)
                dst += dst >= nd.id
                mid = self.new_mid()
                self.msgs[mid] = {"src": nd.id, "dst": dst, "t0": t, "t1": None}
                self.at(t, self._originate, nd, dst, mid)
                t += trng.expovariate(1 / interval)
        self.stats = {"tx": 0, "air": 0.0, "lost": 0, "kinds": {}}
        self.proto = proto
        proto.attach(self)
        for nd in self.nodes:
            proto.start(nd)
        # SOS events: distinct mobile originators, identical across protocols
        self.sos = {}
        if sos:
            srng = random.Random(seed * 13 + 5)
            for nd in srng.sample([x for x in self.nodes if not x.fixed], SOS_EVENTS):
                t0 = srng.uniform(TRAFFIC_START, TRAFFIC_END)
                self.sos[nd.id] = {"t0": t0, "t1": None}
                self.at(t0, proto.start_sos, nd)

    def nseq(self):
        self._seq += 1
        return self._seq

    def new_mid(self):
        self._mid += 1
        return self._mid

    def at(self, t, fn, *a):
        heapq.heappush(self.q, (t, self.nseq(), fn, a))

    def run(self):
        while self.q and self.q[0][0] <= DURATION:
            t, _, fn, a = heapq.heappop(self.q)
            self.t = t
            fn(*a)
        return self.metrics()

    def _originate(self, nd, dst, mid):
        self.proto.originate(nd, dst, mid)

    def sos_hold(self, nd, src):
        """Node `nd` now holds (or has heard) the SOS from `src`."""
        e = self.sos.get(src)
        if e and nd.fixed and e["t1"] is None:
            e["t1"] = self.t

    def deliver(self, nd, f):
        m = self.msgs.get(f.mid)
        if m and m["dst"] == nd.id and m["t1"] is None:
            m["t1"] = self.t

    # radio
    def channel_busy(self, nd):
        now = self.t
        if nd.tx_until > now:
            return True
        for r in nd.recs:
            if r.p >= SENS - 3 and r.start <= now - CAD_TIME and r.end > now:
                return True
        return False

    def transmit(self, n, f):
        t = self.t
        dur = airtime(f.size)
        end = t + dur
        f.sent = True
        f.tx = n.id
        n.tx_until = end
        for r in n.recs:
            if r.end > t:
                r.lost = True
        st = self.stats
        st["tx"] += 1
        st["air"] += dur
        st["kinds"][f.kind] = st["kinds"].get(f.kind, 0) + 1
        pp = n.pos(t)
        for m in self.nodes:
            if m is n:
                continue
            d = max(1.0, dist(pp, m.pos(t)))
            p = (TX_DBM + n.gain + m.gain - (PL0 + 10 * PL_EXP * math.log10(d))
                 + self.rng.gauss(0, FADE_SIGMA))
            if p < INTERF_FLOOR:
                continue
            rec = Rec(t, end, p, f)
            if m.tx_until > t:
                rec.lost = True
            m.recs = [r for r in m.recs if r.end > t]
            for r in m.recs:
                if r.p - p < CAPTURE_DB:
                    r.lost = True
                if p - r.p < CAPTURE_DB:
                    rec.lost = True
            m.recs.append(rec)
            if p >= SENS:
                self.at(end, self._rx_end, m, rec)

    def _rx_end(self, m, rec):
        m.heard += rec.end - rec.start
        if rec.lost:
            self.stats["lost"] += 1
            return
        self.proto.on_rx(m, rec.frame, rec.p)

    def metrics(self):
        ms = list(self.msgs.values())
        lat = [m["t1"] - m["t0"] for m in ms if m["t1"] is not None]
        n = len(ms)
        reach = [e["t1"] - e["t0"] for e in self.sos.values() if e["t1"] is not None]
        ns = len(self.sos)
        return {
            "sos_n": ns,
            "sos_reach": len(reach) / ns if ns else float("nan"),
            "sos_reach60": sum(1 for x in reach if x <= 60) / ns if ns else float("nan"),
            "sos_times": reach,
            "msgs": n,
            "pdr": len(lat) / n if n else 0,
            "pdr60": sum(1 for x in lat if x <= 60) / n if n else 0,
            "lat_med": statistics.median(lat) if lat else float("nan"),
            "air_per_msg": self.stats["air"] / max(1, len(lat)),
            "util": self.stats["air"] / DURATION,
            "tx": self.stats["tx"],
            "lost": self.stats["lost"],
            "kinds": self.stats["kinds"],
        }


# ============================================================ protocols ====
class Proto:
    slotted = False

    def attach(self, sim):
        self.sim = sim
        for nd in sim.nodes:
            nd.mac = MAC(sim, nd, slotted=self.slotted, budget=self.make_budget(nd))

    def make_budget(self, nd):
        return None

    def start_sos(self, n):
        pass        # protocol has no SOS model: never reaches anyone


# ------------------------------------------------- Meshtastic-like flood ---
class Flood(Proto):
    CWMIN, CWMAX, SLOTT = 3, 7, 0.018
    DATA, POS, ACK = 64, 46, 32
    POS_INTERVAL, RETRY_T, RETRIES = 900, 25, 3

    def __init__(self, hop_limit=3):
        self.H = hop_limit
        self.name = f"flood{hop_limit}"

    def start(self, n):
        n.seen, n.pend, n.wait = set(), {}, {}
        self.sim.at(self.sim.rng.uniform(0, self.POS_INTERVAL), self._pos, n)

    def _send_new(self, n, f):
        n.seen.add((f.kind, f.src, f.mid))
        n.mac.send(f)

    def _pos(self, n):
        self._send_new(n, Frame("pos", n.id, BCAST, self.sim.new_mid(), self.POS, hops=self.H))
        self.sim.at(self.sim.t + self.POS_INTERVAL, self._pos, n)

    def start_sos(self, n):
        # baseline: an emergency broadcast, re-sent every 5 min with a new id
        self._send_new(n, Frame("sos", n.id, BCAST, self.sim.new_mid(), SOS_SIZE, hops=self.H))
        self.sim.at(self.sim.t + SOS_REPEAT, self.start_sos, n)

    def originate(self, n, dst, mid):
        f = Frame("data", n.id, dst, mid, self.DATA, hops=self.H)
        n.wait[mid] = 0
        self._send_new(n, f)
        self.sim.at(self.sim.t + self.RETRY_T, self._retry, n, f)

    def _retry(self, n, f):
        if f.mid not in n.wait:
            return
        n.wait[f.mid] += 1
        if n.wait[f.mid] > self.RETRIES:
            del n.wait[f.mid]
            return
        n.mac.send(f.copy())
        self.sim.at(self.sim.t + self.RETRY_T, self._retry, n, f)

    def on_rx(self, n, f, p):
        key = (f.kind, f.src, f.mid)
        if f.kind == "sos":
            self.sim.sos_hold(n, f.src)
        if f.kind == "ack" and f.dst == n.id:
            n.wait.pop(f.mid, None)
            return
        if f.kind == "data" and f.dst == n.id:
            self.sim.deliver(n, f)
            if key not in n.seen:
                n.seen.add(key)
                self._send_new(n, Frame("ack", n.id, f.src, f.mid, self.ACK, hops=self.H))
            return
        if key in n.seen:
            g = n.pend.pop(key, None)
            if g is not None and not n.fixed:      # routers never cancel
                g.cancelled = True
            return
        n.seen.add(key)
        if f.hops <= 0:
            return
        g = f.copy(hops=f.hops - 1)
        if n.fixed:
            delay = self.sim.rng.uniform(0, 2 ** self.CWMIN) * self.SLOTT
        else:
            cw = self.CWMIN + (self.CWMAX - self.CWMIN) * clamp((p - NOISE + 20) / 30, 0, 1)
            delay = self.sim.rng.uniform(0, 2 ** cw) * self.SLOTT
        n.pend[key] = g
        self.sim.at(self.sim.t + delay, self._rebroadcast, n, g)

    def _rebroadcast(self, n, g):
        if not g.cancelled:
            n.mac.send(g)


# ---------------------------------------------- MeshCore-like path learn ---
class PathLearn(Proto):
    name = "pathlearn"
    DM, ADVERT, PATHRET, ACK = 54, 110, 24, 12
    ADVERT_INTERVAL = 3 * 3600

    def start(self, n):
        n.seen, n.paths, n.wait, n.acked_sent = set(), {}, {}, set()
        if n.fixed:
            self.sim.at(self.sim.rng.uniform(0, 1800), self._advert, n)

    def _advert(self, n):
        f = Frame("advert", n.id, BCAST, self.sim.new_mid(), self.ADVERT,
                  meta={"route": "flood", "trace": [], "att": 0})
        n.seen.add(self._key(f))
        n.mac.send(f)
        self.sim.at(self.sim.t + self.ADVERT_INTERVAL, self._advert, n)

    @staticmethod
    def _key(f):
        return (f.kind, f.src, f.mid, f.meta.get("att", 0))

    def originate(self, n, dst, mid):
        n.wait[mid] = {"tries": 0, "dst": dst}
        self._attempt(n, mid)

    def _attempt(self, n, mid):
        w = n.wait.get(mid)
        if not w:
            return
        w["tries"] += 1
        if w["tries"] > 3:
            del n.wait[mid]
            return
        dst = w["dst"]
        path = n.paths.get(dst)
        if path is not None and w["tries"] <= 2:
            f = Frame("dm", n.id, dst, mid, self.DM + len(path),
                      meta={"route": "direct", "path": list(path), "trace": [], "att": w["tries"]})
            timeout = 3 * (len(path) + 1) * airtime(f.size) + 3
        else:
            n.paths.pop(dst, None)
            f = Frame("dm", n.id, dst, mid, self.DM,
                      meta={"route": "flood", "trace": [], "att": w["tries"]})
            timeout = 20
        n.seen.add(self._key(f))
        n.mac.send(f)
        self.sim.at(self.sim.t + timeout, self._check, n, mid, w["tries"])

    def _check(self, n, mid, tries):
        w = n.wait.get(mid)
        if w and w["tries"] == tries:
            self._attempt(n, mid)

    def on_rx(self, n, f, p):
        key = self._key(f)
        if f.meta["route"] == "flood":
            if key in n.seen:
                return
            n.seen.add(key)
            if f.dst == n.id:
                self._final(n, f)
                return
            if n.fixed and len(f.meta["trace"]) < 64:
                g = f.copy(size=f.size + 1)
                g.meta["trace"] = f.meta["trace"] + [n.id]
                self.sim.at(self.sim.t + self.sim.rng.uniform(0, 2.5 * airtime(g.size)),
                            n.mac.send, g)
            return
        # direct (source-routed)
        if f.dst == n.id:
            self._final(n, f)
            return
        path = f.meta["path"]
        if n.fixed and path and path[0] == n.id and key not in n.seen:
            n.seen.add(key)
            g = f.copy(size=f.size - 1)
            g.meta["path"] = path[1:]
            g.meta["trace"] = f.meta["trace"] + [n.id]
            self.sim.at(self.sim.t + self.sim.rng.uniform(0, 0.2 * airtime(g.size)), n.mac.send, g)

    def _final(self, n, f):
        if f.kind == "dm":
            self.sim.deliver(n, f)
            k = (f.src, f.mid, f.meta["att"])
            if k in n.acked_sent:
                return
            n.acked_sent.add(k)
            trace = f.meta["trace"]
            if f.meta["route"] == "flood":
                n.paths[f.src] = list(reversed(trace))
                g = Frame("pathret", n.id, f.src, f.mid, self.PATHRET + len(trace),
                          meta={"route": "flood", "trace": [], "fwd": list(trace),
                                "att": f.meta["att"]})
            else:
                back = list(reversed(trace))
                g = Frame("ack", n.id, f.src, f.mid, self.ACK + len(back),
                          meta={"route": "direct", "path": back, "trace": [], "att": f.meta["att"]})
            n.seen.add(self._key(g))
            n.mac.send(g)
        elif f.kind == "pathret":
            n.paths[f.src] = f.meta["fwd"]
            n.wait.pop(f.mid, None)
        elif f.kind == "ack":
            n.wait.pop(f.mid, None)


# ------------------------------------------------------------ GhostWave ----
class GhostWave(Proto):
    R_EST, THR, K, HOPS, COPIES = 6000.0, 150.0, 3, 15, 8
    D_AREA = 2500.0     # forwarding-area diameter: candidates inside hear each other
    AREA_MIN_NB = 10    # only restrict to the area when the sender is in a dense spot
    DATA, ACK, LACK, BEACON = 85, 40, 23, 64
    STORE_CAP, NB_FRESH, LOC_FRESH = 64, 1200, 3 * 3600
    BEACON_MOVE, BEACON_MAX, BEACON_MIN, UTIL_TARGET = 250, 900, 60, 0.25
    SOS_TTL, SOS_REOFFER, SOS_DUP_CANCEL, BEACON_REPLY_MIN = 24 * 3600, 900, 2, 10
    SOS_RELAY_GAP = 900     # a relay re-floods a superseding seq at most this often
    SOS_RETRY, SOS_RETRY_MAX = 60, 4   # re-offer to a known neighbor still lacking it: 60, 120, 240, 480 s
    SOS_PROBE_CAP = 8

    def __init__(self, slots=False, dtn=True, budget=True, area=True, name="ghostwave"):
        self.slotted, self.dtn, self.use_budget, self.name = slots, dtn, budget, name
        self.area = area

    def make_budget(self, nd):
        if not self.use_budget:
            return None
        return Budget(self.sim, nd, lambda n: max(0.02, 0.30 / (self._nb_count(n) + 1)))

    def _nb_count(self, n):
        now = self.sim.t
        return sum(1 for v in n.nb.values() if now - v[2] < self.NB_FRESH)

    # ---- lifecycle
    def start(self, n):
        n.loc, n.nb = {}, {}
        n.seen, n.pend, n.awaiting, n.store = set(), {}, {}, {}
        n.acked, n.acks_sent = set(), {}
        n.custody, n.lack_t = {}, {}
        n.lastb_pos, n.lastb_t = None, -1e9
        n.util, n.heard_mark, n.util_t = 0.0, 0.0, 0.0
        n.sos, n.sos_seq, n.beacon_soon = {}, 0, False
        n.sos_probe = {}        # neighbor we offered to -> when to ask for its beacon
        self.sim.at(self.sim.rng.uniform(0, 30), self._tick, n)

    def _me(self, n):
        return n.pos(self.sim.t)

    # ---- beacons
    def _tick(self, n):
        now = self.sim.t
        if now > n.util_t:
            n.util = (n.heard - n.heard_mark) / (now - n.util_t)
        n.heard_mark, n.util_t = n.heard, now
        scale = max(1.0, n.util / self.UTIL_TARGET)
        me = self._me(n)
        el = now - n.lastb_t
        moved = dist(me, n.lastb_pos) if n.lastb_pos else 1e9
        if el >= self.BEACON_MIN * scale and (moved >= self.BEACON_MOVE or el >= self.BEACON_MAX * scale):
            self._beacon(n)
        self._retry_store(n)
        for src in [s for s, e in n.sos.items() if now - e["t"] > self.SOS_TTL]:
            del n.sos[src]
        if not n.sos:
            n.sos_probe.clear()
        self.sim.at(now + 30, self._tick, n)

    def _beacon(self, n, ask=None):
        now, me = self.sim.t, self._me(n)
        fresh = [(k, v) for k, v in n.loc.items() if now - v[2] < 1800]
        gossip = self.sim.rng.sample(fresh, min(3, len(fresh)))
        bloom = self._own_bloom(n)
        n.mac.send(Frame("beacon", n.id, BCAST, 0, self.BEACON,
                         meta={"pos": me, "t": now, "gossip": gossip, "own": True,
                               "bloom": bloom, "sos": bool(n.sos), "ask": ask}))
        n.lastb_pos, n.lastb_t = me, now

    def _own_bloom(self, n):
        bloom = 0
        for src, mid in list(n.store) + [(s, e["f"].mid) for s, e in n.sos.items()]:
            bloom |= _bloom_bits(src, mid)
        return bloom

    def _beacon_reply(self, n):
        n.beacon_soon = False
        if self.sim.t - n.lastb_t >= self.BEACON_REPLY_MIN:
            self._beacon(n)

    def _sos_probe(self, n, nb, due):
        """Beacon with next_hop = nb so that it answers with its Bloom filter."""
        if n.sos_probe.get(nb) != due:
            return                       # cancelled or rescheduled
        if self.sim.t - n.lastb_t < self.BEACON_REPLY_MIN:
            self.sim.at(n.lastb_t + self.BEACON_REPLY_MIN, self._sos_probe, n, nb, due)
            return
        del n.sos_probe[nb]
        self._beacon(n, ask=nb)

    def _probe_at(self, n, nb, at, jitter):
        cur = n.sos_probe.get(nb)
        if (cur >= at) if cur is not None else len(n.sos_probe) >= self.SOS_PROBE_CAP:
            return
        due = at + self.sim.rng.uniform(0, jitter)
        n.sos_probe[nb] = due
        self.sim.at(due, self._sos_probe, n, nb, due)

    @classmethod
    def _retry_gap(cls, tries):
        return min(cls.SOS_RETRY << min(tries, 8), cls.SOS_REOFFER)

    # ---- SOS lane (DESIGN §8)
    def start_sos(self, n):
        n.sos_seq += 1
        f = Frame("sos", n.id, BCAST, self.sim.new_mid(), SOS_SIZE, hops=self.HOPS, prio=PRIO_SOS,
                  meta={"seq": n.sos_seq, "own": True})
        old = n.sos.get(n.id)
        n.sos[n.id] = {"f": f, "seq": n.sos_seq, "t": self.sim.t, "flood": None, "dups": 0,
                       "offer": None, "offered": old["offered"] if old else {}, "relay_t": self.sim.t}
        n.mac.send(f)            # prio 2: jumps the queue and bypasses the budget
        self.sim.at(self.sim.t + SOS_REPEAT, self.start_sos, n)

    def _on_sos(self, n, f):
        if f.src == n.id:
            return
        e = n.sos.get(f.src)
        if e and f.meta["seq"] <= e["seq"]:
            if f.meta["seq"] == e["seq"]:
                g = e["flood"]
                if g is not None and not g.sent:
                    e["dups"] += 1
                    if e["dups"] >= self.SOS_DUP_CANCEL:
                        g.cancelled = True
                o = e["offer"]
                if o is not None and not o.sent:
                    o.cancelled = True           # someone else already offered it
                n.sos_probe.pop(f.tx, None)   # its sender has it
            else:
                self._offer_sos(n, None, 0)      # sender is behind
            return
        now = self.sim.t
        if e:
            for g in (e["flood"], e["offer"]):
                if g is not None:
                    g.cancelled = True
        relay = e is None or now - e["relay_t"] >= self.SOS_RELAY_GAP
        n.sos[f.src] = {"f": f, "seq": f.meta["seq"], "t": now, "flood": None, "dups": 0,
                        "offer": None, "offered": e["offered"] if e else {},
                        "relay_t": now if relay else e["relay_t"]}
        e = n.sos[f.src]
        self.sim.sos_hold(n, f.src)
        if f.hops > 0 and relay:
            g = f.copy(hops=f.hops - 1)
            g.meta["own"] = False
            e["flood"] = g
            self.sim.at(self.sim.t + self.sim.rng.uniform(0, 2 * airtime(g.size) + 0.12),
                        self._sos_fire, n, g)

    def _sos_fire(self, n, g):
        if not g.cancelled:
            n.mac.send(g)

    def _offer_sos(self, n, nb, bloom, known=False):
        """Offer every SOS we hold that the neighbor's beacon Bloom filter lacks.
        known: heard within NB_FRESH, so an earlier offer was probably lost:
        re-offer with backoff and probe for its Bloom filter. Returns whether
        the neighbor lacks anything we hold."""
        now = self.sim.t
        lacks = False
        for src, e in n.sos.items():
            if src == nb:
                continue
            o = e["offered"].get(nb)
            if bloom is not None and _bloom_has(bloom, src, e["f"].mid):
                if o:
                    o[1] = 0
                continue
            lacks = True
            tries, ready = 0, now
            if o:
                ready = o[0] + (self._retry_gap(o[1]) if known else self.SOS_REOFFER)
                if known:
                    tries = o[1] + 1
            if known:
                ready = max(ready, e["t"] + self.SOS_RETRY)   # our own flood may still reach it
            if now < ready:
                if known and tries <= self.SOS_RETRY_MAX:
                    self._probe_at(n, nb, ready, self.SOS_RETRY / 4)
                continue
            e["offered"][nb] = [now, tries]
            if nb is not None and tries < self.SOS_RETRY_MAX:
                gap = self._retry_gap(tries)
                self._probe_at(n, nb, now + gap, gap / 4)
            o = e["offer"]
            if o is not None and not o.sent and not o.cancelled:
                continue
            # a fresh scoped flood in the neighbor's partition; jittered so that
            # when several holders meet the same newcomer only one offers
            g = e["f"].copy(hops=self.HOPS)
            g.meta["own"] = False
            e["offer"] = g
            self.sim.at(now + self.sim.rng.uniform(0, 2 * airtime(g.size) + 0.12), self._sos_fire, n, g)
        return lacks

    def _learn(self, n, nid, pos, t):
        if nid == n.id:
            return
        cur = n.loc.get(nid)
        if cur is None or cur[2] < t:
            n.loc[nid] = (pos[0], pos[1], t)

    def _on_beacon(self, n, f):
        now = self.sim.t
        pos = f.meta["pos"]
        old = n.nb.get(f.src)
        is_new = old is None or now - old[2] >= self.NB_FRESH
        if not self._offer_sos(n, f.src, f.meta["bloom"], known=not is_new):
            n.sos_probe.pop(f.src, None)
        # answer an SOS carrier that is new, or whose filter holds something
        # ours lacks (once per filter), or a probe addressed to us
        theirs = f.meta["bloom"]
        answered = 0 if is_new else old[3]
        ahead = (theirs & ~self._own_bloom(n)) and theirs != answered
        if (f.meta["sos"] and (is_new or ahead)) or f.meta.get("ask") == n.id:
            answered = theirs
            if not n.beacon_soon:
                n.beacon_soon = True
                self.sim.at(now + self.sim.rng.uniform(0.2, 2.0), self._beacon_reply, n)
        n.nb[f.src] = (pos[0], pos[1], now, answered)
        self._learn(n, f.src, pos, now)
        for nid, v in f.meta["gossip"]:
            self._learn(n, nid, (v[0], v[1]), v[2])
        if not self.dtn:
            return
        me = self._me(n)
        for key, e in list(n.store.items()):
            f0 = e["f"]
            if f0.dst == f.src:
                self._unicast(n, f0, f.src, e.get("copies", 1), key)
                continue
            if e["mode"] == "spray" and f0.dst in n.loc:
                self._to_geo(n, key, e)
                continue
            if e["mode"] == "geo":
                tgt = f0.meta["tgt"]
                if self._store_ready(e) and dist(pos, tgt) < dist(me, tgt) - self.THR:
                    self._reattempt(n, key, e)
            elif e["copies"] > 1 and f.src not in e["given"]:
                give = e["copies"] // 2
                e["given"].add(f.src)
                self._unicast(n, f0, f.src, give, key)

    def _retry_store(self, n):
        if not self.dtn:
            return
        now, me = self.sim.t, self._me(n)
        for key, e in list(n.store.items()):
            if e["mode"] == "spray" and e["f"].dst in n.loc:
                self._to_geo(n, key, e)
            elif e["mode"] == "geo" and self._store_ready(e):
                tgt = e["f"].meta["tgt"]
                if any(now - v[2] < 180 and dist((v[0], v[1]), tgt) < dist(me, tgt) - self.THR
                       for v in n.nb.values()):
                    self._reattempt(n, key, e)

    def _store_ready(self, e):
        return self.sim.t - e["last"] >= min(600, 30 * 2 ** e["f"].meta.get("satt", 0))

    def _reattempt(self, n, key, e):
        del n.store[key]
        f = e["f"]
        f.meta["satt"] = f.meta.get("satt", 0) + 1
        self._geo_send(n, f, tries=2, wide=True)

    # ---- store
    def _store(self, n, f, mode, copies=1):
        key = (f.src, f.mid)
        if not self.dtn or key in n.acked:
            return
        if len(n.store) >= self.STORE_CAP:
            oldest = min(n.store, key=lambda k: n.store[k]["t"])
            del n.store[oldest]
        n.store[key] = {"f": f, "mode": mode, "copies": copies, "given": set(),
                        "last": self.sim.t, "t": self.sim.t}

    def _to_geo(self, n, key, e):
        f = e["f"]
        x, y, t = n.loc[f.dst]
        f.meta["tgt"] = (x, y)
        f.meta["tgt_t"] = t
        del n.store[key]
        self._geo_send(n, f)

    # ---- origination
    def originate(self, n, dst, mid):
        me = self._me(n)
        f = Frame("data", n.id, dst, mid, self.DATA, hops=self.HOPS,
                  meta={"srcpos": me, "src_t": self.sim.t})
        key = (n.id, mid)
        n.seen.add(key)
        if dst in n.loc:
            x, y, t = n.loc[dst]
            f.meta.update(tgt=(x, y), tgt_t=t)
            self._geo_send(n, f)
        elif self.dtn:
            self._store(n, f, "spray", self.COPIES)
            now = self.sim.t
            e = n.store[key]
            for nid, v in list(n.nb.items()):
                if now - v[2] < 300 and e["copies"] > 1:
                    if nid == dst:
                        self._unicast(n, f, nid, 1, key)
                        break
                    e["given"].add(nid)
                    self._unicast(n, f, nid, e["copies"] // 2, key)
        # no location and no DTN: nothing we can do

    # ---- GEO forwarding (CBF)
    def _geo_send(self, n, f, tries=None, wide=False):
        key = (f.src, f.mid) if f.kind == "data" else ("A", f.src, f.mid)
        me, now = self._me(n), self.sim.t
        tgt = f.meta["tgt"]
        if tries is None:
            closer = any(now - v[2] < 600 and dist((v[0], v[1]), tgt) < dist(me, tgt) - self.THR
                         for v in n.nb.values())
            tries = 3 if closer or f.dst in n.nb else 2
        g = f.copy()
        g.meta["mode"] = "geo"
        g.meta["fpos"] = me
        g.meta["nh"] = 0
        g.meta["own"] = f.src == n.id
        g.meta["wide"] = wide or not self.area or self._nb_count(n) < self.AREA_MIN_NB
        if f.tx != -1 and f.tx != n.id:
            g.hops = f.hops - 1
        n.mac.send(g, now_ok=f.__dict__.pop("_aligned", False))
        tok = self.sim.nseq()
        n.awaiting[key] = {"f": f, "g": g, "tries": tries - 1, "tok": tok, "pos": me}
        wait = (self.K + 2) * SLOT + airtime(g.size) if self.slotted else \
            self.K * airtime(self.DATA) + 2 * airtime(g.size) + 0.5
        self.sim.at(now + wait + (2.0 if not self.slotted else 0), self._await_to, n, key, tok)

    def _await_to(self, n, key, tok):
        a = n.awaiting.get(key)
        if not a or a["tok"] != tok:
            return
        if not a["g"].sent:      # still queued (budget/CSMA): check again later
            self.sim.at(self.sim.t + 2 * SLOT, self._await_to, n, key, tok)
            return
        del n.awaiting[key]
        if a["tries"] > 0 or not a["g"].meta.get("wide"):
            # a failed narrow-area attempt always gets at least one wide retry
            self._geo_send(n, a["f"], max(1, a["tries"]), wide=True)
        elif a["f"].kind == "data":
            self._store(n, a["f"], "geo")

    def _overheard(self, n, key, f):
        """Someone transmitted a frame we already know. Decide whether it is an
        implicit ack for us, and link-ack an upstream that is retrying."""
        tgt = f.meta["tgt"]
        a = n.awaiting.get(key)
        if a:
            if not a["g"].sent:
                if f.tx != a["f"].tx:                 # someone else took custody first
                    self._implicit_ack(n, key)
            elif dist(f.meta["fpos"], tgt) < dist(a["pos"], tgt) - self.THR / 2:
                self._implicit_ack(n, key)
        elif key in n.pend:
            if f.tx != n.pend[key].tx:
                self._implicit_ack(n, key)
        elif key in n.store and dist(f.meta["fpos"], tgt) < dist(self._me(n), tgt):
            n.store.pop(key, None)
        up = n.custody.get(key)
        if up is not None and f.tx == up and key not in n.awaiting and key not in n.pend:
            if self.sim.t - n.lack_t.get(key, -1e9) > 5:
                n.lack_t[key] = self.sim.t
                n.mac.send(Frame("lack", n.id, up, f.mid, self.LACK, meta={"key": key}))

    def _implicit_ack(self, n, key):
        a = n.awaiting.pop(key, None)
        if a:
            a["g"].cancelled = True
        g = n.pend.pop(key, None)
        if g:
            g.cancelled = True
        n.store.pop(key, None)

    def _schedule_fwd(self, n, f, p):
        me = self._me(n)
        tgt = f.meta["tgt"]
        fp = f.meta["fpos"]
        prog = dist(fp, tgt) - dist(me, tgt)
        if prog < self.THR or f.hops <= 0:
            return
        if not f.meta.get("wide"):
            # first attempt: only nodes inside a circle of diameter D toward the
            # target may contend, so every candidate hears every other one
            dtot = dist(fp, tgt)
            d = min(self.D_AREA, dtot)
            c = (fp[0] + (tgt[0] - fp[0]) * d / 2 / dtot, fp[1] + (tgt[1] - fp[1]) * d / 2 / dtot)
            if dist(me, c) > d / 2:
                return
        q = clamp((p - SENS) / 10, 0.2, 1.0)
        s = 1 - clamp(prog * q / self.R_EST, 0, 1)
        now = self.sim.t
        # destination-first window: if the target is probably in direct range of
        # the transmitter, give it one contention round to ack before relaying
        dest_first = dist(f.meta["fpos"], tgt) < 0.4 * self.R_EST
        if self.slotted:
            k0 = math.ceil(now / SLOT - 1e-9) + (self.K + 1 if dest_first else 0)
            kk = int(s * self.K)
            k = k0 + kk
            while f.prio < PRIO_SOS and k % PRIO_EVERY == 0:
                k += 1
            micro = (s * self.K - kk) * GUARD * 0.9 + self.sim.rng.uniform(0, 0.002)
            t = k * SLOT + micro
        else:
            t = now + (s * self.K + (self.K + 1 if dest_first else 0)) * airtime(self.DATA) \
                + self.sim.rng.uniform(0, 0.12)
        g = f.copy()
        g._aligned = self.slotted
        key = (f.src, f.mid) if f.kind == "data" else ("A", f.src, f.mid)
        n.pend[key] = g
        self.sim.at(t, self._fire, n, key, g)

    def _fire(self, n, key, g):
        if n.pend.get(key) is not g or g.cancelled:
            return
        del n.pend[key]
        n.custody[key] = g.tx
        # refresh target if we know better
        if g.dst in n.loc and n.loc[g.dst][2] > g.meta.get("tgt_t", -1):
            x, y, t = n.loc[g.dst]
            g.meta["tgt"], g.meta["tgt_t"] = (x, y), t
        self._geo_send(n, g)

    # ---- unicast handoff (spray / direct delivery from store)
    def _unicast(self, n, f, nh, copies, key):
        g = f.copy()
        g.meta.update(mode="uni", nh=nh, copies=copies, fpos=self._me(n), own=f.src == n.id)
        n.mac.send(g)
        ukey = ("U", key, nh)
        tok = self.sim.nseq()
        n.awaiting[ukey] = {"f": f, "g": g, "tries": 2, "tok": tok, "copies": copies, "nh": nh}
        self.sim.at(self.sim.t + 3 * SLOT + airtime(g.size) + 1.0, self._uni_to, n, ukey, tok)

    def _uni_to(self, n, ukey, tok):
        a = n.awaiting.get(ukey)
        if not a or a["tok"] != tok:
            return
        if not a["g"].sent:
            self.sim.at(self.sim.t + 2 * SLOT, self._uni_to, n, ukey, tok)
            return
        if a["tries"] <= 0:
            del n.awaiting[ukey]
            e = n.store.get(ukey[1])
            if e:
                e["given"].discard(a["nh"])
            return
        a["tries"] -= 1
        g = a["g"].copy()
        a["g"] = g
        n.mac.send(g)
        a["tok"] = tok2 = self.sim.nseq()
        self.sim.at(self.sim.t + 3 * SLOT + airtime(g.size) + 1.0, self._uni_to, n, ukey, tok2)

    def _on_link_ack(self, n, f):
        key = (f.meta["dsrc"], f.mid)
        ukey = ("U", key, f.src)
        a = n.awaiting.pop(ukey, None)
        if not a:
            return
        e = n.store.get(key)
        if f.src == a["f"].dst:
            n.store.pop(key, None)
            n.acked.add(key)
        elif e:
            e["copies"] -= a["copies"]
            if e["copies"] < 1:
                n.store.pop(key, None)

    # ---- receive
    def on_rx(self, n, f, p):
        now = self.sim.t
        if f.kind == "beacon":
            self._on_beacon(n, f)
            return
        if f.kind == "sos":
            self._on_sos(n, f)
            return
        if "fpos" in f.meta and f.tx >= 0:
            self._learn(n, f.tx, f.meta["fpos"], now)
        if f.kind == "lack":
            if f.dst == n.id:
                if "key" in f.meta:
                    self._implicit_ack(n, f.meta["key"])
                else:
                    self._on_link_ack(n, f)
            elif "key" in f.meta and f.meta["key"] in n.pend:
                self._implicit_ack(n, f.meta["key"])     # it already arrived
            return
        if f.kind == "ack":
            dkey = (f.dst, f.mid)          # ack.dst = data src
            n.acked.add(dkey)
            self._implicit_ack(n, dkey)
            akey = ("A", f.src, f.mid)
            if f.dst == n.id:
                if now - n.lack_t.get(akey, -1e9) > 5:
                    n.lack_t[akey] = now
                    n.mac.send(Frame("lack", n.id, f.tx, f.mid, self.LACK, meta={"key": akey}))
                return
            if akey in n.seen:
                self._overheard(n, akey, f)
                return
            n.seen.add(akey)
            self._schedule_fwd(n, f, p)
            return
        # DATA
        key = (f.src, f.mid)
        if "srcpos" in f.meta:
            self._learn(n, f.src, f.meta["srcpos"], f.meta["src_t"])
        mode = f.meta.get("mode")
        if f.dst == n.id:
            self.sim.deliver(n, f)
            if mode == "uni":
                n.mac.send(Frame("lack", n.id, f.tx, f.mid, self.LACK, meta={"dsrc": f.src}))
            last = n.acks_sent.get(key, -1e9)
            if now - last > 10:
                n.acks_sent[key] = now
                n.seen.add(("A", n.id, f.mid))
                ack = Frame("ack", n.id, f.src, f.mid, self.ACK, hops=self.HOPS,
                            meta={"tgt": f.meta["srcpos"], "tgt_t": f.meta["src_t"]})
                self._geo_send(n, ack, tries=2)
            return
        if key in n.acked:
            if mode == "uni" and f.meta["nh"] == n.id:
                n.mac.send(Frame("lack", n.id, f.tx, f.mid, self.LACK, meta={"dsrc": f.src}))
            return
        if mode == "uni":
            if f.meta["nh"] != n.id:
                return
            n.mac.send(Frame("lack", n.id, f.tx, f.mid, self.LACK, meta={"dsrc": f.src}))
            if key in n.store or key in n.awaiting:
                return
            n.seen.add(key)
            g = f.copy()
            g.meta.pop("nh", None)
            if f.dst in n.loc:
                x, y, t = n.loc[f.dst]
                g.meta.update(tgt=(x, y), tgt_t=t)
                g.tx = -1
                self._geo_send(n, g)
            else:
                self._store(n, g, "spray", f.meta["copies"])
            return
        # GEO
        if key in n.seen:
            self._overheard(n, key, f)
            return
        n.seen.add(key)
        if f.dst in n.loc and n.loc[f.dst][2] > f.meta.get("tgt_t", -1):
            x, y, t = n.loc[f.dst]
            f = f.copy()
            f.meta["tgt"], f.meta["tgt_t"] = (x, y), t
            # progress is still measured from the transmitter's position
        self._schedule_fwd(n, f, p)


def _bloom_bits(src, mid):
    """The two bits (k = 2) of the 64-bit beacon Bloom filter for (src, mid)."""
    h = ((src * 0x9E3779B1) ^ (mid * 0x85EBCA77)) & 0xFFFFFFFF
    h = ((h ^ (h >> 15)) * 0x2C1B3C6D) & 0xFFFFFFFF
    h ^= h >> 13
    return (1 << (h & 63)) | (1 << ((h >> 6) & 63))


def _bloom_has(bloom, src, mid):
    b = _bloom_bits(src, mid)
    return bloom & b == b


# ---------------------------------------------------------------- main -----
PROTOS = {
    "flood3": lambda: Flood(3),
    "flood7": lambda: Flood(7),
    "pathlearn": PathLearn,
    "ghostwave": lambda: GhostWave(),
    "ghostwave-slots": lambda: GhostWave(slots=True, name="ghostwave-slots"),
    "ghostwave-nobudget": lambda: GhostWave(budget=False, name="ghostwave-nobudget"),
    "ghostwave-nodtn": lambda: GhostWave(dtn=False, name="ghostwave-nodtn"),
    "ghostwave-noarea": lambda: GhostWave(area=False, name="ghostwave-noarea"),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", nargs="*", default=list(SCENARIOS))
    ap.add_argument("--proto", nargs="*", default=list(PROTOS))
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--markdown", action="store_true")
    ap.add_argument("--kinds", action="store_true", help="print tx count per frame kind")
    ap.add_argument("--sos", action="store_true",
                    help=f"add {SOS_EVENTS} SOS events per seed; report reach time to any fixed node")
    a = ap.parse_args()
    rows = []
    for sc in a.scenario:
        for pn in a.proto:
            res = [Sim(sc, PROTOS[pn](), seed, sos=a.sos).run() for seed in range(1, a.seeds + 1)]
            avg = {k: statistics.mean(r[k] for r in res)
                   for k in ("msgs", "pdr", "pdr60", "lat_med", "air_per_msg", "util", "tx", "lost")}
            rows.append((sc, pn, avg))
            print(f"{sc:12s} {pn:20s} pdr={avg['pdr']:.3f} pdr60={avg['pdr60']:.3f} "
                  f"lat_med={avg['lat_med']:7.1f}s air/msg={avg['air_per_msg']:6.2f}s "
                  f"util={avg['util']:.3f} tx={avg['tx']:.0f} lost={avg['lost']:.0f}", flush=True)
            if a.sos:
                times = sorted(x for r in res for x in r["sos_times"])
                ns = sum(r["sos_n"] for r in res)
                med = statistics.median(times) if times else float("nan")
                p90 = times[int(0.9 * (len(times) - 1))] if times else float("nan")
                sos_air = statistics.mean(r["kinds"].get("sos", 0) for r in res) * airtime(SOS_SIZE)
                print(f"    SOS -> any fixed node: reached {len(times)}/{ns} "
                      f"({len(times) / ns * 100:.0f} %), <=60 s {sum(x <= 60 for x in times)}/{ns}, "
                      f"median {med:.0f} s, p90 {p90:.0f} s, max {max(times, default=float('nan')):.0f} s; "
                      f"SOS airtime {sos_air:.0f} s/run", flush=True)
                if a.kinds:
                    print("    reach times (s):", [round(x) for x in times])
            if a.kinds:
                print("    kinds (seed 1):", res[0]["kinds"])
    if a.markdown:
        print("\n| scenario | protocol | delivered | delivered ≤60 s | median latency | airtime per delivered msg | channel util |")
        print("|---|---|---|---|---|---|---|")
        for sc, pn, v in rows:
            print(f"| {sc} | {pn} | {v['pdr']*100:.1f} % | {v['pdr60']*100:.1f} % | "
                  f"{v['lat_med']:.1f} s | {v['air_per_msg']:.2f} s | {v['util']*100:.1f} % |")


if __name__ == "__main__":
    main()
