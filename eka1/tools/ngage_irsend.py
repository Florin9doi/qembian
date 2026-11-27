"""Send files to an emulated phone over its IrDA link (qembian: -M nokia6600, nokia6260, nokia9210 ... with --irda).

The phone's 2nd serial port carries the raw SIR byte stream (docs/devices/nokia-6600-nhl10/infrared.md). This
script is the other phone: a small IrDA primary written from the specs -- SIR framing + FCS, IrLAP (XID
discovery, SNRM/UA, I/RR frames with go-back-N), IrLMP with an IAS client (looks up OBEX's LSAP) and a stub IAS
server, TinyTP credit flow, and an OBEX client (CONNECT, PUT, DISCONNECT). Items land in the phone's Messaging
Inbox as "Infrared device: ...".

  python3 eka1/tools/ngage_phone.py --model nokia6600 --phone ir-1 --irda a
  # on the phone: Menu, Connect., Infrared (it listens for about two minutes)
  python3 ngage_irsend.py photo.jpg card.vcf
  python3 ngage_irsend.py --out inbox/       # no files: receive (on the phone: Send, Via infrared)

Receiving, the script answers the phone's discovery as "qembian" (--nick), accepts its connection, offers OBEX in
its IAS and saves each pushed item in --out (never overwriting: name-1.ext, ...). It keeps listening for more
until Ctrl-C (--once: stop after one connection).

--link (default b) is the end of the link the script takes; the phone takes the other one:
  a / b                        the local UDP pair of ngage_sess.py (phone --irda a <-> script b, and the reverse)
  udp:HOST:PORT@LHOST:LPORT    send to HOST:PORT, receive on LHOST:LPORT (the QEMU udp chardev form)
  tcp:HOST:PORT                connect to a phone started with --irda tcp:HOST:PORT,server=on,wait=off
  tcp-listen:HOST:PORT         listen; start the phone with --irda tcp:HOST:PORT afterwards
"""
import argparse, mimetypes, os, random, select, socket, struct, sys, time

# ---------------------------------------------------------------- transport

IRDA_ENDS = {'a': 'udp:127.0.0.1:7471@127.0.0.1:7470', 'b': 'udp:127.0.0.1:7470@127.0.0.1:7471'}


class Link:
    """A byte pipe to the phone's SIR UART."""

    def __init__(self, spec):
        spec = IRDA_ENDS.get(spec, spec)
        kind, _, rest = spec.partition(':')
        if kind == 'udp':
            remote, _, local = rest.partition('@')
            self.remote = self._hp(remote)
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.bind(self._hp(local) if local else ('', 0))
            self.udp = True
        elif kind == 'tcp':
            self.sock = socket.create_connection(self._hp(rest))
            self.udp = False
        elif kind == 'tcp-listen':
            srv = socket.socket()
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind(self._hp(rest))
            srv.listen(1)
            log('waiting for the phone to connect to %s' % rest)
            self.sock, _ = srv.accept()
            srv.close()
            self.udp = False
        else:
            raise SystemExit('unknown link %r' % spec)
        if not self.udp:
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    @staticmethod
    def _hp(s):
        h, _, p = s.rpartition(':')
        return (h or '127.0.0.1', int(p))

    def write(self, data):
        if self.udp:
            for i in range(0, len(data), 1024):      # QEMU's udp chardev reads datagrams of up to 4096 bytes
                self.sock.sendto(data[i:i + 1024], self.remote)
        else:
            self.sock.sendall(data)

    def read(self, timeout):
        r, _, _ = select.select([self.sock], [], [], max(0.0, timeout))
        if not r:
            return b''
        d = self.sock.recv(65536)
        if not d and not self.udp:
            raise LinkLost('the phone closed the connection')
        return d


class LinkLost(Exception):
    pass


class Disconnected(LinkLost):
    """The phone closed the link the normal way (DISC)."""


VERBOSE = 0


def log(msg, level=0):
    if VERBOSE >= level:
        print(msg, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- SIR framing

BOF, EOF, CE = 0xC0, 0xC1, 0x7D


def _fcs_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
        t.append(c)
    return t


FCS_TABLE = _fcs_table()


def fcs16(data, fcs=0xFFFF):
    for b in data:
        fcs = (fcs >> 8) ^ FCS_TABLE[(fcs ^ b) & 0xFF]
    return fcs


def sir_wrap(frame, xbofs):
    f = frame + struct.pack('<H', fcs16(frame) ^ 0xFFFF)
    out = bytearray([BOF] * (xbofs + 1))
    for b in f:
        if b in (BOF, EOF, CE):
            out += bytes([CE, b ^ 0x20])
        else:
            out.append(b)
    out.append(EOF)
    return bytes(out)


class SirReader:
    def __init__(self):
        self.buf = None          # None = hunting for a BOF
        self.esc = False

    def feed(self, data):
        frames = []
        for b in data:
            if b == BOF:
                self.buf, self.esc = bytearray(), False
            elif self.buf is None:
                continue
            elif b == EOF:
                f, self.buf = bytes(self.buf), None
                if len(f) >= 4 and fcs16(f) == 0xF0B8:
                    frames.append(f[:-2])
                elif f:
                    log('  rx: bad FCS, dropped %d bytes' % len(f), 2)
            elif b == CE:
                self.esc = True
            else:
                self.buf.append(b ^ 0x20 if self.esc else b)
                self.esc = False
        return frames


# ---------------------------------------------------------------- IrLAP

XID_CMD, XID_RSP, SNRM, UA, DISC, DM, FRMR, UI, TEST = 0x2F, 0xAF, 0x83, 0x63, 0x43, 0x0F, 0x87, 0x03, 0xE3
RR, RNR, REJ, SREJ = 0x01, 0x05, 0x09, 0x0D
PF = 0x10

# QoS: bit tables (value of bit i)
BAUDS = [2400, 9600, 19200, 38400, 57600, 115200, 576000, 1152000, 4000000]
DATA_SIZES = [64, 128, 256, 512, 1024, 2048]
XBOFS = [48, 24, 12, 5, 3, 2, 1, 0]
MIN_TURN_US = [10000, 5000, 1000, 500, 100, 50, 10, 0]


def qos_params(data):
    out, i = {}, 0
    while i + 2 <= len(data):
        pi, pl = data[i], data[i + 1]
        out[pi] = int.from_bytes(data[i + 2:i + 2 + pl], 'little')
        i += 2 + pl
    return out


def highest_bit(v):
    return v.bit_length() - 1 if v else -1


def lowest_bit(v):
    return (v & -v).bit_length() - 1 if v else -1


# what we offer: baud 9600 | 115200 (the bytes on a chardev do not care), max turnaround 500 ms, any data size up
# to 2048, window 7, no extra BOFs, no min turnaround, link disconnect up to 40 s
OUR_QOS = {0x01: 0x22, 0x82: 0x01, 0x83: 0x3F, 0x84: 0x7F, 0x85: 0x80, 0x86: 0x80, 0x08: 0xFF}


def qos_bytes(q):
    return b''.join(bytes([pi, 1, v]) for pi, v in q.items())


# Discovery hints: ext, then OBEX; charset ASCII. Never the Computer bit (0x04 in the first byte): a 6600 with
# Infrared on takes a computer for a PC Suite host, and its mRouter probes us in the middle of a push (IAS mRouter,
# IrCOMM, IrModem, IrLPT, then LM-Connect to LSAP 2); when all of that is refused the phone drops the link
# without answering the OBEX DISCONNECT, sends SNRMs of its own and freezes. A phone advertises 92 25 and is
# never probed.
DISCOVERY_HINTS = bytes([0x80, 0x20])


def discovery_info(nickname, hints=None):
    return (DISCOVERY_HINTS if hints is None else hints) + b'\x00' + nickname


def discovery_nickname(di):
    """The device name in an XID discovery-info field: hint bytes (bit 7 = another follows), the charset, the name.
    Charset 0xFF is Unicode: the 9210 sends its "EPOC" as UTF-16 little-endian (the spec leaves the byte order open:
    the zero bytes tell). Other charsets are ASCII / ISO-8859-x."""
    j = 0
    while j < len(di) and di[j] & 0x80:
        j += 1
    if len(di) <= j + 1:
        return '?'
    cs, name = di[j + 1], di[j + 2:]
    if cs == 0xFF:
        name = name[:len(name) & ~1]
        le = sum(1 for b in name[1::2] if b == 0) >= sum(1 for b in name[0::2] if b == 0)
        return name.decode('utf-16-le' if le else 'utf-16-be', 'replace').rstrip('\0')
    return name.decode('latin-1', 'replace').rstrip('\0')


class IrLap:
    def __init__(self, link, slots=6, slot_time=0.25, ftimer=3.0, retries=10, idle=0.02, link_timeout=30.0):
        self.link, self.rd = link, SirReader()
        self.primary = True
        self.link_timeout = link_timeout
        self.saddr = struct.pack('<I', random.randint(1, 0xFFFFFFFE))
        self.slots, self.slot_time, self.ftimer, self.retries, self.idle = slots, slot_time, ftimer, retries, idle
        self.xbofs = 10
        self.data_size_ours = DATA_SIZES[highest_bit(OUR_QOS[0x83])]
        self.pending = []        # received frames not yet consumed
        self.connected = False

    # -- raw frames
    def send(self, frame):
        log('  tx %s' % frame.hex(), 3)
        self.link.write(sir_wrap(frame, self.xbofs))

    def recv(self, deadline):
        while True:
            if self.pending:
                return self.pending.pop(0)
            t = deadline - time.monotonic()
            if t <= 0:
                return None
            for f in self.rd.feed(self.link.read(t)):
                log('  rx %s' % f.hex(), 3)
                self.pending.append(f)

    def flush(self):
        self.pending.clear()
        while self.link.read(0):
            pass
        self.rd = SirReader()

    # -- discovery
    def discover(self, timeout, nickname=b'qembian'):
        flag = {1: 0, 6: 1, 8: 2, 16: 3}[self.slots]
        info = discovery_info(nickname)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            found = {}
            for slot in list(range(self.slots)) + [0xFF]:
                body = bytes([0x01]) + self.saddr + b'\xff\xff\xff\xff' + bytes([flag, slot, 0x00])
                self.send(bytes([0xFF, XID_CMD | PF]) + body + (info if slot == 0xFF else b''))
                dl = time.monotonic() + (0.05 if slot == 0xFF else self.slot_time)
                while True:
                    f = self.recv(dl)
                    if f is None:
                        break
                    if len(f) >= 14 and f[1] & ~PF & 0xFF == XID_RSP and f[3 + 4:3 + 8] == self.saddr:
                        daddr, di = f[3:7], f[14:]
                        found[daddr] = discovery_nickname(di)
            if found:
                return list(found.items())[0]
            time.sleep(0.3)
        return None

    # -- connection
    def connect(self, daddr):
        self.daddr = daddr
        self.ca = random.randint(1, 0x7E)
        ours = qos_bytes(OUR_QOS)
        frame = bytes([0xFF, SNRM | PF]) + self.saddr + daddr + bytes([self.ca << 1]) + ours
        for _ in range(self.retries):
            self.send(frame)
            dl = time.monotonic() + self.ftimer
            while True:
                f = self.recv(dl)
                if f is None:
                    break
                if f[0] == self.ca << 1 and f[1] == UA | PF:
                    q = qos_params(f[10:])
                    self._apply_qos(q)
                    self._link_up(True)
                    return
                if f[1] & ~PF & 0xFF == DM:
                    raise LinkLost('the phone refused the connection (DM)')
        raise LinkLost('no answer to SNRM')

    def _link_up(self, primary):
        self.primary = primary
        self.vs = self.vr = 0
        self.unacked = []        # [(ns, info)]
        self.peer_busy = False
        self.connected = True

    def _apply_qos(self, q):
        baud = q.get(0x01, 0x02) & OUR_QOS[0x01]
        self.baud = BAUDS[highest_bit(baud)] if baud else 9600
        self.data_size = DATA_SIZES[min(highest_bit(q.get(0x83, 1)), 5)]
        self.window = max(1, min(7, highest_bit(q.get(0x84, 1)) + 1))
        self.xbofs = XBOFS[lowest_bit(q.get(0x85, 0x80))] if q.get(0x85) else 0
        self.min_turn = MIN_TURN_US[lowest_bit(q.get(0x86, 0x80))] / 1e6 if q.get(0x86) else 0
        log('link up: %d baud, data size %d, window %d, %d extra BOFs, min turnaround %g ms'
            % (self.baud, self.data_size, self.window, self.xbofs, self.min_turn * 1e3), 1)

    def disconnect(self):
        if not self.connected or not self.primary:
            return
        self.connected = False
        for _ in range(3):
            self.send(bytes([self.ca << 1 | 1, DISC | PF]))
            dl = time.monotonic() + self.ftimer
            while True:
                f = self.recv(dl)
                if f is None:
                    break
                if f[0] >> 1 == self.ca and f[1] & ~PF & 0xFF in (UA, DM):
                    return

    # -- the secondary side: answer the phone's discovery, accept its SNRM
    def listen(self, deadline, nickname=b'qembian'):
        """Wait for a phone to discover us and connect. Returns (its address, its nickname) or None."""
        info = discovery_info(nickname)
        chosen, responded, last_slot, nick = None, False, -1, {}
        while True:
            f = self.recv(deadline)
            if f is None:
                return None
            if len(f) < 2 or f[0] != 0xFF:
                continue
            if f[1] == XID_CMD | PF and len(f) >= 14 and f[7:11] in (b'\xff' * 4, self.saddr):
                peer, flags, slot = f[3:7], f[11], f[12]
                if slot == 0xFF:                      # end of the round: the phone's own name is in it
                    nick[peer] = discovery_nickname(f[14:])
                    chosen, last_slot = None, -1
                    continue
                if chosen is None or slot <= last_slot:   # a new round
                    chosen, responded = random.randrange({0: 1, 1: 6, 2: 8, 3: 16}[flags & 3]), False
                last_slot = slot
                if not responded and slot >= chosen:
                    self.send(bytes([0xFE, XID_RSP | PF, 0x01]) + self.saddr + peer + bytes([flags, slot, 0x00]) + info)
                    responded = True
                    log('  answered discovery from %s in slot %d' % (peer[::-1].hex(), slot), 2)
            elif f[1] == SNRM | PF and len(f) >= 11 and f[6:10] == self.saddr:
                self.daddr, self.ca = f[2:6], f[10] >> 1
                q = qos_params(f[11:])
                neg = dict(OUR_QOS)
                neg[0x01] = (q.get(0x01, 0x02) & OUR_QOS[0x01]) or 0x02    # type 0: the common bits
                neg[0x08] = (q.get(0x08, 0x01) & OUR_QOS[0x08]) or 0x01
                self.ua = bytes([self.ca << 1, UA | PF]) + self.saddr + self.daddr + qos_bytes(neg)
                self._apply_qos(q)
                self._link_up(False)
                time.sleep(self.min_turn)
                self.send(self.ua)
                return self.daddr, nick.get(self.daddr, '?')

    def respond(self, outq):
        """Secondary: take the phone's frames up to its poll (P), answer it with our I-frames (a window from
        outq, go-back-N on what it has not acknowledged) or an RR, F on the last. Returns the infos received."""
        got = []
        dl = time.monotonic() + self.link_timeout
        while True:
            f = self.recv(dl)
            if f is None:
                self.connected = False
                raise LinkLost('the phone went quiet')
            if len(f) < 2:
                continue
            if f[0] == 0xFF:
                if f[1] == SNRM | PF and f[2:6] == self.daddr and f[6:10] == self.saddr and f[10] >> 1 == self.ca:
                    self._link_up(False)                           # our UA was lost: the phone asks again
                    self.send(self.ua)
                    continue
                if f[1] in (XID_CMD | PF, SNRM | PF):
                    # the phone discovers or connects anew: its side of this link is gone (it gave up without a
                    # DISC we saw). Hand the frame to listen() now instead of staying deaf until link_timeout.
                    self.pending.insert(0, f)
                    self.connected = False
                    raise LinkLost('the phone started over (new discovery or connection)')
                continue
            if f[0] >> 1 != self.ca or not f[0] & 1:
                continue
            c = f[1]
            if c & 1 == 0:
                if (c >> 1) & 7 == self.vr:
                    got.append(f[2:])
                    self.vr = (self.vr + 1) % 8
                self._ack(c >> 5)
            elif c & 3 == 1:
                self._ack(c >> 5)
                self.peer_busy = c & 0x0F == RNR
            else:
                u = c & ~PF & 0xFF
                if u == DISC:
                    self.send(bytes([self.ca << 1, UA | PF]))
                    self.connected = False
                    raise Disconnected('the phone closed the link')
                if u == SNRM:
                    self._link_up(False)
                    self.send(self.ua)
                continue
            if not c & PF:
                continue
            time.sleep(self.min_turn)
            while not self.peer_busy and len(self.unacked) < self.window and outq:
                self.unacked.append((self.vs, outq.pop(0)))
                self.vs = (self.vs + 1) % 8
            frames = [] if self.peer_busy else self.unacked
            for k, (ns, info) in enumerate(frames):
                fb = PF if k == len(frames) - 1 else 0
                self.send(bytes([self.ca << 1, self.vr << 5 | fb | ns << 1]) + info)
            if not frames:
                self.send(bytes([self.ca << 1, self.vr << 5 | PF | RR]))
            return got

    def cycle(self, outq):
        return self.exchange(outq) if self.primary else self.respond(outq)

    # -- data transfer: one poll/response cycle
    def exchange(self, outq):
        """Send up to a window of I-frames from outq (a list, consumed from the front) -- or an RR poll when
        there is nothing new -- with P on the last; collect the phone's frames up to its F. Returns the
        in-sequence I-frame infos received."""
        if not self.connected:
            raise LinkLost('the link is down')
        got, tries = [], 0
        busy = False
        while True:
            time.sleep(self.min_turn)
            while not busy and len(self.unacked) < self.window and outq:
                self.unacked.append((self.vs, outq.pop(0)))
                self.vs = (self.vs + 1) % 8
            frames = self.unacked if not busy else []
            if frames:
                for k, (ns, info) in enumerate(frames):
                    p = PF if k == len(frames) - 1 else 0
                    self.send(bytes([self.ca << 1 | 1, self.vr << 5 | p | ns << 1]) + info)
            else:
                self.send(bytes([self.ca << 1 | 1, self.vr << 5 | PF | RR]))
            dl = time.monotonic() + self.ftimer
            final, busy = False, False
            while not final:
                f = self.recv(dl)
                if f is None:
                    break
                if len(f) >= 10 and f[0] == 0xFF and f[1] == SNRM | PF and f[6:10] == self.saddr:
                    # the phone dropped our link on its side and asks for a new one: refuse it, so it stops
                    # retrying, and end ours
                    self.send(bytes([0xFE, DM | PF]))
                    self.connected = False
                    raise LinkLost('the phone dropped the link and asked for a new one (SNRM)')
                if len(f) < 2 or f[0] >> 1 != self.ca or f[0] & 1:
                    continue                          # not ours, or someone else's command
                c = f[1]
                final = bool(c & PF)
                if c & 1 == 0:                        # I-frame
                    if (c >> 1) & 7 == self.vr:
                        got.append(f[2:])
                        self.vr = (self.vr + 1) % 8
                    self._ack(c >> 5)
                elif c & 3 == 1:                      # S-frame
                    self._ack(c >> 5)
                    if c & 0x0F == RNR:
                        busy = True
                else:
                    u = c & ~PF & 0xFF
                    if u in (DM, DISC, FRMR):
                        self.connected = False
                        raise LinkLost('the phone dropped the link (%s)'
                                       % {DM: 'DM', DISC: 'DISC', FRMR: 'FRMR'}[u])
            if not final:
                tries += 1
                if tries > self.retries:
                    raise LinkLost('the phone stopped answering')
                log('  F-timer expired, retry %d' % tries, 2)
                continue
            if busy:
                time.sleep(0.1)
                continue
            # unacked frames left after a final = lost: go back N (next cycle resends them)
            if self.unacked:
                tries += 1
                if tries > self.retries:
                    raise LinkLost('frames not acknowledged')
                continue
            return got

    def _ack(self, nr):
        while self.unacked and self.unacked[0][0] != nr:
            self.unacked.pop(0)


# ---------------------------------------------------------------- IrLMP + IAS + TinyTP

LM_CONNECT, LM_CONNECT_CNF, LM_DISCONNECT = 0x01, 0x81, 0x02
IAS_GET_VALUE_BY_CLASS = 0x04


class IrLmp:
    def __init__(self, lap, nickname=b'qembian'):
        self.lap, self.outq, self.conns = lap, [], {}   # conns: (our lsap, their lsap) -> handler
        self.ctl = []                                    # control frames for us: (ours, theirs, opcode, data)
        self.ias_srv = set()                             # their lsaps connected to our IAS server
        self.listeners = {}                              # our lsap -> accept(their lsap, user data) -> handler
        # our IAS: (class, attribute) -> value (int = integer, bytes = user string)
        self.ias = {(b'Device', b'DeviceName'): nickname, (b'Device', b'IrLMPSupport'): None}

    def send(self, dlsap, slsap, data, ctl=False):
        self.outq.append(bytes([(0x80 if ctl else 0) | dlsap, slsap]) + data)

    def pump(self):
        had = bool(self.outq)
        for f in self.lap.cycle(self.outq):
            self._rx(f)
        if self.lap.primary and not had and not self.outq:
            time.sleep(self.lap.idle)

    def _rx(self, f):
        if len(f) < 2:
            return
        ctl, ours, theirs, data = f[0] & 0x80, f[0] & 0x7F, f[1] & 0x7F, f[2:]
        if ctl:
            op = data[0] if data else 0
            if op == LM_CONNECT and ours == 0:            # the phone asks our IAS
                self.send(theirs, 0, bytes([LM_CONNECT_CNF, 0]), ctl=True)
                self.ias_srv.add(theirs)
            elif op == LM_CONNECT and ours in self.listeners and (ours, theirs) not in self.conns:
                self.conns[(ours, theirs)] = self.listeners[ours](theirs, data[2:])
            elif op == LM_CONNECT and (ours, theirs) not in self.conns:
                self.send(theirs, ours, bytes([LM_DISCONNECT, 0x02]), ctl=True)   # no such service
            elif op == LM_DISCONNECT and ours == 0:
                self.ias_srv.discard(theirs)
            else:
                self.ctl.append((ours, theirs, op, data[1:]))
                h = self.conns.get((ours, theirs))
                if op == LM_DISCONNECT and h:
                    h.closed = True
            return
        if ours == 0 and theirs in self.ias_srv:
            if data and data[0] & 0x80:                   # last frame of a query
                self.send(theirs, 0, self._ias_answer(data))
            return
        h = self.conns.get((ours, theirs))
        if h:
            h.data(data)

    def _ias_answer(self, q):
        op = q[0] & 0x3F
        if op != IAS_GET_VALUE_BY_CLASS:
            return bytes([0x80 | op, 0xFF])                       # unsupported operation
        try:
            n = q[1]
            cls = q[2:2 + n]
            attr = q[3 + n:3 + n + q[2 + n]]
        except IndexError:
            return bytes([0x80 | op, 0xFF])
        log('  IAS query %s %s' % (cls.decode('latin-1'), attr.decode('latin-1')), 2)
        if not any(c == cls for c, _ in self.ias):
            return bytes([0x80 | op, 0x01])                       # no such class
        if (cls, attr) not in self.ias:
            return bytes([0x80 | op, 0x02])                       # no such attribute
        v = self.ias[(cls, attr)]
        if isinstance(v, int):
            val = bytes([1]) + struct.pack('>I', v)
        elif isinstance(v, bytes):
            val = bytes([3, 0, len(v)]) + v                       # user string, ASCII
        else:
            val = bytes([2]) + struct.pack('>H', 3) + b'\x01\x00\x00'   # IrLMPSupport: version 1, no extras
        return bytes([0x80 | op, 0x00]) + struct.pack('>HH', 1, 0) + val

    def wait(self, cond, timeout, what):
        end = time.monotonic() + timeout
        while not cond():
            if time.monotonic() > end:
                raise LinkLost('timed out waiting for %s' % what)
            self.pump()

    def _ctl_for(self, ours, theirs, ops):
        for i, (o, t, op, d) in enumerate(self.ctl):
            if o == ours and t == theirs and op in ops:
                return self.ctl.pop(i)

    def connect(self, ours, theirs, udata, timeout):
        self.send(theirs, ours, bytes([LM_CONNECT, 0]) + udata, ctl=True)
        res = []

        def answered():
            if not res:
                r = self._ctl_for(ours, theirs, (LM_CONNECT_CNF, LM_DISCONNECT))
                if r:
                    res.append(r)
            return bool(res)
        self.wait(answered, timeout, 'LM-Connect')
        o, t, op, d = res[0]
        if op != LM_CONNECT_CNF:
            raise LinkLost('LSAP %d refused the connection (reason %d)' % (theirs, d[0] if d else 0))
        return d[1:]                 # past the reserved parameter byte: the user data

    def disconnect(self, ours, theirs):
        self.send(theirs, ours, bytes([LM_DISCONNECT, 0x01]), ctl=True)
        self.conns.pop((ours, theirs), None)
        while self.outq:
            self.pump()

    def ias_lsap(self, cls, attr, timeout):
        ours = 0x10
        box = type('Box', (), {'buf': b'', 'done': False, 'closed': False})()

        def data(d):
            box.buf += d[1:]
            box.done = bool(d and d[0] & 0x80)
            box.op = d[0] if d else 0
        box.data = data
        self.conns[(ours, 0)] = box
        self.connect(ours, 0, b'', timeout)
        q = bytes([0x80 | IAS_GET_VALUE_BY_CLASS, len(cls)]) + cls + bytes([len(attr)]) + attr
        self.send(0, ours, q)
        self.wait(lambda: box.done or box.closed, timeout, 'the IAS answer')
        self.disconnect(ours, 0)
        r = box.buf
        if not r or r[0] != 0:
            return None
        n = struct.unpack('>H', r[1:3])[0]
        i = 3
        for _ in range(n):
            typ = r[i + 2]
            if typ == 1:
                return struct.unpack('>I', r[i + 3:i + 7])[0]
            i += 3 + {0: 0, 2: 2 + (struct.unpack('>H', r[i + 3:i + 5])[0] if typ == 2 else 0),
                      3: 2 + (r[i + 4] if typ == 3 else 0)}.get(typ, 0)
        return None


class TinyTp:
    """A TinyTP connection used as a byte stream (no SAR)."""
    INITIAL_CREDIT = 14

    def __init__(self, lmp, ours, theirs, timeout=None, accept=None):
        """Connect to the phone's LSAP, or (accept = the LM-Connect user data) take the phone's connection."""
        self.lmp, self.ours, self.theirs = lmp, ours, theirs
        self.rx, self.closed = b'', False
        self.backlog = []                     # PDUs waiting for credit (queue / service)
        self.give = 0                         # credit owed to the phone
        self.peer_left = self.INITIAL_CREDIT  # PDUs the phone may still send us
        lmp.conns[(ours, theirs)] = self
        if accept is None:
            d = lmp.connect(ours, theirs, bytes([self.INITIAL_CREDIT]), timeout)
        else:
            d = accept
            lmp.send(theirs, ours, bytes([LM_CONNECT_CNF, 0, self.INITIAL_CREDIT]), ctl=True)
        self.credit = d[0] & 0x7F if d else 0
        self.max_sdu = 0
        if d and d[0] & 0x80 and len(d) > 1:
            p = d[2:2 + d[1]]
            i = 0
            while i + 2 <= len(p):
                pi, pl = p[i], p[i + 1]
                if pi == 0x01:
                    self.max_sdu = int.from_bytes(p[i + 2:i + 2 + pl], 'big')
                i += 2 + pl
        self.pdu_max = lmp.lap.data_size - 3
        if self.max_sdu:
            self.pdu_max = min(self.pdu_max, self.max_sdu)
        log('TinyTP to LSAP %d: credit %d, PDU up to %d bytes' % (theirs, self.credit, self.pdu_max), 1)

    def data(self, d):
        if not d:
            return
        self.credit = min(127, self.credit + (d[0] & 0x7F))
        if len(d) > 1:
            self.rx += d[1:]
            self.peer_left -= 1
            self.give += 1

    def _grant(self):
        g = min(self.give, 127)
        self.give -= g
        self.peer_left += g
        return g

    def write(self, data, timeout):
        for i in range(0, len(data), self.pdu_max):
            self.lmp.wait(lambda: self.credit > 0 or self.closed, timeout, 'TinyTP credit')
            if self.closed:
                raise LinkLost('the phone closed the OBEX connection')
            self.credit -= 1
            self.lmp.send(self.theirs, self.ours, bytes([self._grant()]) + data[i:i + self.pdu_max])
        while self.lmp.outq:
            self.lmp.pump()

    def queue(self, data):
        for i in range(0, len(data), self.pdu_max):
            self.backlog.append(data[i:i + self.pdu_max])

    def service(self):
        """Send what credit allows from the backlog; return credit with a dataless PDU when the phone runs low."""
        while self.backlog and self.credit > 0:
            self.credit -= 1
            self.lmp.send(self.theirs, self.ours, bytes([self._grant()]) + self.backlog.pop(0))
        if self.peer_left < self.INITIAL_CREDIT // 2 and self.give and not self.lmp.outq:
            self.lmp.send(self.theirs, self.ours, bytes([self._grant()]))

    def pump(self):
        if self.peer_left < self.INITIAL_CREDIT // 2 and self.give and not self.lmp.outq:
            self.lmp.send(self.theirs, self.ours, bytes([self._grant()]))    # dataless credit PDU
        self.lmp.pump()

    def next_obex(self):
        """A whole OBEX packet from what has arrived, or None."""
        if len(self.rx) < 3 or len(self.rx) < struct.unpack('>H', self.rx[1:3])[0]:
            return None
        n = max(3, struct.unpack('>H', self.rx[1:3])[0])
        pkt, self.rx = self.rx[:n], self.rx[n:]
        return pkt

    def read_obex(self, timeout):
        end = time.monotonic() + timeout
        while True:
            pkt = self.next_obex()
            if pkt:
                return pkt
            if self.closed:
                raise LinkLost('the phone closed the OBEX connection')
            if time.monotonic() > end:
                raise LinkLost('timed out waiting for an OBEX response')
            self.pump()


# ---------------------------------------------------------------- OBEX

OBEX_CONNECT, OBEX_DISCONNECT, OBEX_PUT, OBEX_FINAL = 0x80, 0x81, 0x02, 0x80
H_NAME, H_TYPE, H_LENGTH, H_BODY, H_END_BODY = 0x01, 0x42, 0xC3, 0x48, 0x49
RSP_CONTINUE, RSP_SUCCESS = 0x90, 0xA0
RSP_NAMES = {0xC0: 'Bad Request', 0xC1: 'Unauthorized', 0xC3: 'Forbidden', 0xC4: 'Not Found',
             0xC6: 'Not Acceptable', 0xCD: 'Request Entity Too Large', 0xCF: 'Unsupported Media Type',
             0xD0: 'Internal Server Error', 0xD3: 'Service Unavailable'}


def hdr_unicode(hi, s):
    v = s.encode('utf-16-be') + b'\0\0'
    return bytes([hi]) + struct.pack('>H', 3 + len(v)) + v


def hdr_bytes(hi, v):
    return bytes([hi]) + struct.pack('>H', 3 + len(v)) + v


def obex_pkt(op, body):
    return bytes([op]) + struct.pack('>H', 3 + len(body)) + body


class Obex:
    def __init__(self, ttp, timeout):
        self.ttp, self.timeout = ttp, timeout
        mine = min(ttp.pdu_max, 0xFFFF)
        self._req(obex_pkt(OBEX_CONNECT, bytes([0x10, 0x00]) + struct.pack('>H', mine)))
        r = self._rsp()
        if r[0] != RSP_SUCCESS:
            raise LinkLost('OBEX CONNECT refused: %s' % self._rname(r[0]))
        theirs = struct.unpack('>H', r[5:7])[0] if len(r) >= 7 else 255
        self.pkt_max = max(255, min(theirs, mine))
        log('OBEX connected, packets up to %d bytes' % self.pkt_max, 1)

    @staticmethod
    def _rname(c):
        return RSP_NAMES.get(c & 0x7F | 0x80, '0x%02x' % c)

    def _req(self, p):
        self.ttp.write(p, self.timeout)

    def _rsp(self):
        return self.ttp.read_obex(self.timeout)

    def put(self, name, data, mime, progress):
        hdrs = hdr_unicode(H_NAME, name) + bytes([H_LENGTH]) + struct.pack('>I', len(data))
        if mime:
            hdrs += hdr_bytes(H_TYPE, mime.encode('ascii') + b'\0')
        pos = 0
        while True:
            room = self.pkt_max - 3 - len(hdrs) - 3
            if room < 0:
                raise LinkLost('headers do not fit in one OBEX packet (%d bytes)' % self.pkt_max)
            chunk = data[pos:pos + room]
            last = pos + len(chunk) >= len(data)
            body = hdrs + hdr_bytes(H_END_BODY if last else H_BODY, chunk)
            self._req(obex_pkt(OBEX_PUT | (OBEX_FINAL if last else 0), body))
            r = self._rsp()
            pos += len(chunk)
            hdrs = b''
            progress(pos)
            if last:
                if r[0] != RSP_SUCCESS:
                    raise LinkLost('the phone rejected %s: %s' % (name, self._rname(r[0])))
                return
            if r[0] != RSP_CONTINUE:
                raise LinkLost('the phone stopped %s: %s' % (name, self._rname(r[0])))

    def disconnect(self):
        self._req(obex_pkt(OBEX_DISCONNECT, b''))
        try:
            self._rsp()
        except LinkLost:
            pass


OBEX_LSAP = 0x11
RSP_BAD_REQUEST, RSP_NOT_IMPLEMENTED = 0xC0, 0xD1
TYPE_EXT = {'text/x-vcard': '.vcf', 'text/x-vcalendar': '.vcs', 'text/x-vnote': '.vnt', 'text/x-vmsg': '.vmg',
            'image/jpeg': '.jpg', 'image/gif': '.gif', 'image/png': '.png', 'audio/amr': '.amr',
            'application/java-archive': '.jar', 'application/vnd.symbian.install': '.sis'}


def obex_headers(b):
    """(id, value) pairs: unicode headers decoded to str, 1- and 4-byte ones to int, the rest bytes"""
    out, i = [], 0
    while i < len(b):
        hi, kind = b[i], b[i] & 0xC0
        if kind == 0x80:
            out.append((hi, b[i + 1]))
            i += 2
        elif kind == 0xC0:
            out.append((hi, struct.unpack('>I', b[i + 1:i + 5])[0]))
            i += 5
        else:
            n = struct.unpack('>H', b[i + 1:i + 3])[0]
            if n < 3:
                raise ValueError('bad header length')
            v = b[i + 3:i + n]
            out.append((hi, v.decode('utf-16-be', 'replace').rstrip('\0') if kind == 0 else v))
            i += n
    return out


class ObexServer:
    """Takes OBEX PUTs and saves them in out_dir."""

    def __init__(self, out_dir, max_pkt):
        self.out_dir, self.max_pkt = out_dir, max_pkt
        self.saved = []
        self._reset()

    def _reset(self):
        self.name, self.mime, self.length, self.body, self.t0 = None, None, None, bytearray(), None

    def handle(self, pkt):
        op = pkt[0]
        if op == OBEX_CONNECT:
            return obex_pkt(RSP_SUCCESS, bytes([0x10, 0x00]) + struct.pack('>H', self.max_pkt))
        if op == OBEX_DISCONNECT:
            return obex_pkt(RSP_SUCCESS, b'')
        if op == 0xFF:                                    # ABORT
            self._reset()
            return obex_pkt(RSP_SUCCESS, b'')
        if op & 0x7F != OBEX_PUT:
            return obex_pkt(RSP_NOT_IMPLEMENTED, b'')     # GET, SETPATH, ...
        try:
            hdrs = obex_headers(pkt[3:])
        except (ValueError, IndexError, struct.error):
            self._reset()
            return obex_pkt(RSP_BAD_REQUEST, b'')
        if self.t0 is None:
            self.t0 = time.monotonic()
        for hi, v in hdrs:
            if hi == H_NAME:
                self.name = v
            elif hi == H_TYPE:
                self.mime = v.rstrip(b'\0').decode('ascii', 'replace')
            elif hi == H_LENGTH:
                self.length = v
            elif hi in (H_BODY, H_END_BODY):
                self.body += v
        if self.length:
            sys.stderr.write('\r%s: %d/%d bytes' % (self.name or '(no name)', len(self.body), self.length))
            sys.stderr.flush()
        if not op & OBEX_FINAL:
            return obex_pkt(RSP_CONTINUE, b'')
        path = self._save()
        sys.stderr.write('\n' if self.length else '')
        log('received %s (%d bytes%s) -> %s' % (self.name or '(no name)', len(self.body),
                                                ', ' + self.mime if self.mime else '', path))
        self._reset()
        return obex_pkt(RSP_SUCCESS, b'')

    def _save(self):
        name = os.path.basename((self.name or '').replace('\\', '/')).strip().lstrip('.')
        name = ''.join(ch for ch in name if ch >= ' ' and ch not in '<>:"|?*')
        if not name:
            name = 'received' + TYPE_EXT.get((self.mime or '').lower(), '.bin')
        base, ext = os.path.splitext(name)
        path, n = os.path.join(self.out_dir, name), 1
        while os.path.exists(path):
            path = os.path.join(self.out_dir, '%s-%d%s' % (base, n, ext))
            n += 1
        with open(path, 'wb') as fh:
            fh.write(self.body)
        self.saved.append(path)
        return path


def receive_session(lap, nickname, out_dir):
    """Serve one IrLAP connection the phone opened: IAS + OBEX on LSAP OBEX_LSAP. Returns the files saved."""
    lmp = IrLmp(lap, nickname)
    for cls in (b'OBEX', b'OBEX:IrXfer'):
        lmp.ias[(cls, b'IrDA:TinyTP:LsapSel')] = OBEX_LSAP
    server = ObexServer(out_dir, lap.data_size_ours - 3)
    ttps = []

    def accept(theirs, udata):
        t = TinyTp(lmp, OBEX_LSAP, theirs, accept=udata)
        ttps.append(t)
        return t
    lmp.listeners[OBEX_LSAP] = accept
    try:
        while True:
            for t in ttps:
                while not t.closed:
                    pkt = t.next_obex()
                    if not pkt:
                        break
                    t.queue(server.handle(pkt))
                t.service()
            ttps[:] = [t for t in ttps if not t.closed]
            lmp.pump()
    except Disconnected:
        pass
    if server.name is not None or server.body:
        log('the link closed in the middle of %s: not saved' % (server.name or 'a file'))
    return server.saved


# ---------------------------------------------------------------- main

EXT_TYPES = {'.vcf': 'text/x-vcard', '.vcs': 'text/x-vcalendar', '.jad': 'text/vnd.sun.j2me.app-descriptor',
             '.jar': 'application/java-archive', '.sis': 'application/vnd.symbian.install'}


def send_files(a, link):
    items = []
    for i, path in enumerate(a.files):
        with open(path, 'rb') as fh:
            data = fh.read()
        name = a.name[i] if i < len(a.name) else os.path.basename(path)
        if a.type is not None:
            mime = a.type
        else:
            mime = EXT_TYPES.get(os.path.splitext(name)[1].lower()) or mimetypes.guess_type(name)[0] or ''
        items.append((name, data, mime))

    lap = IrLap(link, slots=a.slots, ftimer=a.ftimer)
    wait = 120 if a.wait is None else a.wait
    log('looking for the phone (turn Infrared on: Menu, Connect., Infrared)...')
    lap.flush()
    dev = lap.discover(wait, a.nick.encode('latin-1'))
    if not dev:
        log('no phone answered in %g s' % wait)
        return 2
    daddr, nick = dev
    log('found "%s" (%s)' % (nick, daddr[::-1].hex()))
    try:
        lap.connect(daddr)
        lmp = IrLmp(lap, a.nick.encode('latin-1'))
        lsap = lmp.ias_lsap(b'OBEX', b'IrDA:TinyTP:LsapSel', a.timeout)
        if lsap is None:
            raise LinkLost('the phone has no OBEX service in its IAS')
        log('OBEX is on LSAP %d' % lsap, 1)
        ttp = TinyTp(lmp, 0x11, lsap, a.timeout)
        obex = Obex(ttp, a.timeout)
        for name, data, mime in items:
            t0 = time.monotonic()

            def progress(n, name=name, total=len(data), t0=t0):
                dt = max(time.monotonic() - t0, 1e-3)
                sys.stderr.write('\r%s: %d/%d bytes (%.1f kB/s)' % (name, n, total, n / dt / 1e3))
                sys.stderr.flush()
            obex.put(name, data, mime, progress)
            sys.stderr.write('  sent\n')
        try:
            obex.disconnect()
            lmp.disconnect(0x11, lsap)
        except LinkLost as e:
            log('every file was delivered, but closing the connection failed: %s' % e)
    except LinkLost as e:
        sys.stderr.write('\n')
        log('error: %s' % e)
        return 1
    finally:
        try:
            lap.disconnect()
        except (LinkLost, OSError):
            pass
    log('done: %d file(s) in the phone\'s Inbox' % len(items))
    return 0


def receive_files(a, link):
    os.makedirs(a.out, exist_ok=True)
    nick = a.nick.encode('latin-1')
    end = None if a.wait is None else time.monotonic() + a.wait
    total = []
    log('waiting for the phone as "%s" (on the phone: Send, Via infrared); files go to %s' % (a.nick, a.out))
    prev = None
    while True:
        lap = IrLap(link, ftimer=a.ftimer, link_timeout=a.timeout)
        if prev is not None:
            # keep what already arrived: a discovery that came with (or ended) the last link is the next attempt
            lap.pending, lap.rd = prev.pending, prev.rd
            lap.saddr = prev.saddr          # the same device to the phone: a reconnect to it still reaches us
        dl = end if end is not None else time.monotonic() + 3600
        dev = lap.listen(dl, nick)
        if not dev:
            if end is not None:
                break
            continue
        daddr, pnick = dev
        log('"%s" (%s) connected' % (pnick, daddr[::-1].hex()), 1)
        try:
            total += receive_session(lap, nick, a.out)
            log('link closed', 1)
        except LinkLost as e:
            log('link lost: %s' % e)
        if a.once:
            break
        prev = lap
    log('%d file(s) received' % len(total))
    return 0 if total or not a.once else 1


def main():
    global VERBOSE, DISCOVERY_HINTS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*', help='files to send; none = receive what the phone sends')
    ap.add_argument('--link', default='b', help='our end of the link: a, b, udp:..., tcp:..., tcp-listen:... '
                    '(default b: the phone runs with --irda a)')
    ap.add_argument('--name', action='append', default=[], help='send: name shown on the phone (once per file, in '
                    'order; default the file name)')
    ap.add_argument('--type', default=None, help='send: MIME type for every file ("" = no Type header; default: '
                    'guessed from the name)')
    ap.add_argument('--out', default='.', help='receive: folder for the files (default: the current one)')
    ap.add_argument('--once', action='store_true', help='receive: stop after the first connection')
    ap.add_argument('--nick', default='qembian', help='our device name, as the phone lists it (default qembian)')
    ap.add_argument('--wait', type=float, default=None, help='seconds to look for (send, default 120) or wait '
                    'for (receive, default for ever) the phone')
    ap.add_argument('--timeout', type=float, default=60, help='seconds to wait for any one answer (default 60)')
    ap.add_argument('--hints', default=DISCOVERY_HINTS.hex(), help='discovery hint bytes in hex (default %s: OBEX; '
                    'a "computer" bit makes the phone probe for PC Suite and drop the link)' % DISCOVERY_HINTS.hex())
    ap.add_argument('--slots', type=int, default=6, choices=(1, 6, 8, 16), help='send: discovery slots (default 6)')
    ap.add_argument('--ftimer', type=float, default=3.0, help='send: IrLAP F-timer in seconds (default 3; the '
                    'emulated phone is slower than a real one)')
    ap.add_argument('-v', '--verbose', action='count', default=0, help='-v link details, -vv retries, -vvv frames')
    a = ap.parse_args()
    VERBOSE = a.verbose + 1
    DISCOVERY_HINTS = bytes.fromhex(a.hints)
    link = Link(a.link)
    try:
        return send_files(a, link) if a.files else receive_files(a, link)
    except KeyboardInterrupt:
        sys.stderr.write('\n')
        return 130


if __name__ == '__main__':
    sys.exit(main())
