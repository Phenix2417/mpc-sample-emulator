#!/usr/bin/env python3
"""mpc.py - the kit's Python helpers (standard library only), called by ./mpc:

  mpc.py az0x    FILE OUTDIR          extract Akai's AZ0x firmware container (MPC Sample update.img)
  mpc.py fit     FILE OUTDIR          extract a U-Boot FIT firmware container (other inMusic devices)
  mpc.py fitscan FILE OUT             find the FIT container with the root file system inside FILE, save it as OUT
  mpc.py rootfs  PAYLOADS.json        print "file|kind" of the root file system among the extracted parts
  mpc.py defaults ROOT BACKUP [keep-template] [keep-fader]   factory defaults at every start
  mpc.py ctl     ROOT OUTDIR          the other end of the add-on's fake control chip
  mpc.py disks   BUS MOUNTPOINT LOG   stand-in for the firmware's disk service: reports the microSD card
"""
import bz2, gzip, hashlib, json, lzma, os, re, shutil, socket, struct, subprocess, sys, threading, time

# ============================== firmware containers ==============================
CHUNK = 1 << 20
FDT_MAGIC = 0xD00DFEED


def sniff(b):
    for magic, kind in ((b'\xfd7zXZ\x00', 'xz'), (b'\x1f\x8b', 'gzip'), (b'\x28\xb5\x2f\xfd', 'zstd'),
                        (b'\x04\x22\x4d\x18', 'lz4'), (b'BZh', 'bzip2'), (b'\xd0\x0d\xfe\xed', 'fit'),
                        (b'hsqs', 'squashfs'), (b'sqsh', 'squashfs'), (b'ANDROID!', 'android-boot')):
        if b.startswith(magic): return kind
    return 'ext' if b[1080:1082] == b'\x53\xef' else 'raw'


def decompress(path, kind):
    """path (compressed with kind) -> path.dec, the compressed file removed; None if it can't be done"""
    out = path.rsplit('.', 1)[0] + '.dec'
    opener = {'gzip': gzip.open, 'xz': lzma.open, 'bzip2': bz2.open}.get(kind)
    if opener:
        with opener(path, 'rb') as f, open(out, 'wb') as g: shutil.copyfileobj(f, g, CHUNK)
    elif kind in ('zstd', 'lz4') and shutil.which(kind):
        subprocess.run([kind, '-d', '-f', '-q', path, '-o', out], check=True, stdout=subprocess.DEVNULL)
    else:
        return None
    os.remove(path)
    return out


def finish(outdir, parts):
    """decompress each written part, print it and write OUTDIR/payloads.json"""
    summary = []
    for name, partition, raw, extra in parts:
        with open(raw, 'rb') as f: kind = sniff(f.read(4096))
        final, contents = raw, kind
        if kind in ('xz', 'gzip', 'zstd', 'lz4', 'bzip2'):
            print('  decompressing %s (%s, %d MB)...' % (name, kind, os.path.getsize(raw) >> 20), flush=True)
            dec = decompress(raw, kind)
            if dec:
                with open(dec, 'rb') as f: final, contents = dec, sniff(f.read(4096))
        e = dict(name=name, partition=partition, compression=kind, file=final, contents=contents,
                 bytes=os.path.getsize(final), **extra)
        summary.append(e)
        print('  %-14s -> %s (%s, %d bytes%s)' % (name, final, contents, e['bytes'],
              '' if 'sha256_ok' not in e else ', checksum ' + ('ok' if e['sha256_ok'] else 'not matched')))
    json.dump(summary, open(os.path.join(outdir, 'payloads.json'), 'w'), indent=1)


def cstr(buf, off): return buf[off:buf.index(b'\0', off)].decode('latin-1')


def az0x(path, outdir):
    """Akai AZ0x (MPC Sample 1.3.0, little-endian): 0x00 "AZ0x", u32 version, u32 ?, u32 string table;
    0x14 u32 string table end, u32 entries, u32 entries end. Entries of 64 bytes: tag[4] (BOOT/PARR), u32 flags,
    u64 offset, u64 size, u32 name (into the string table), u32 ?, sha256[32]."""
    with open(path, 'rb') as f: head = f.read(65536)
    if head[:4] != b'AZ0x': raise ValueError('not an AZ0x container (starts with %r)' % head[:4])
    ver, _, strtab = struct.unpack_from('<III', head, 4)
    _, ent_off, ent_end = struct.unpack_from('<III', head, 0x14)
    if not (0 < ent_off < ent_end <= len(head)) or (ent_end - ent_off) % 64:
        raise ValueError('unexpected entry table %#x..%#x' % (ent_off, ent_end))
    os.makedirs(outdir, exist_ok=True)
    print('AZ0x container v%d' % ver)
    parts = []
    with open(path, 'rb') as src:
        for k in range((ent_end - ent_off) // 64):
            e = ent_off + 64 * k
            tag = head[e:e + 4].decode('latin-1')
            _, off, size, name_off = struct.unpack_from('<IQQI', head, e + 4)
            name = (cstr(head, strtab + name_off) if name_off and strtab + name_off < ent_off else '') or tag.lower()
            raw, h = os.path.join(outdir, '%d-%s.bin' % (k, name)), hashlib.sha256()
            src.seek(off); left = size
            with open(raw, 'wb') as dst:
                while left:
                    b = src.read(min(CHUNK, left))
                    if not b: break
                    h.update(b); dst.write(b); left -= len(b)
            parts.append((name, name, raw, {'sha256_ok': h.hexdigest() == head[e + 32:e + 64].hex()}))
    finish(outdir, parts)


def fdt(buf, base=0):
    """(totalsize, root node) of the flattened device tree at base; node = {'props': {}, 'nodes': {}}"""
    magic, total, off_struct, off_strings = struct.unpack_from('>IIII', buf, base)
    if magic != FDT_MAGIC: raise ValueError('no FDT magic at offset %d' % base)
    al = lambda x: base + ((x - base + 3) & ~3)
    p, root, stack = base + off_struct, None, []
    while True:
        tok = struct.unpack_from('>I', buf, p)[0]; p += 4
        if tok == 1:
            end = buf.index(b'\0', p); node = {'props': {}, 'nodes': {}}
            if stack: stack[-1]['nodes'][buf[p:end].decode('latin-1')] = node
            else: root = node
            stack.append(node); p = al(end + 1)
        elif tok == 2: stack.pop()
        elif tok == 3:
            ln, nameoff = struct.unpack_from('>II', buf, p); p += 8
            stack[-1]['props'][cstr(buf, base + off_strings + nameoff)] = bytes(buf[p:p + ln]); p = al(p + ln)
        elif tok == 9: return total, root
        elif tok != 4: raise ValueError('bad FDT token %d at %d' % (tok, p - 4))


def fit_images(buf, base=0):
    total, root = fdt(buf, base)
    ext = base + ((total + 3) & ~3)
    s = lambda v: v.rstrip(b'\0').decode('latin-1', 'replace') if v else ''
    n = lambda v: int.from_bytes(v, 'big') if v else 0
    out = []
    for name, node in root['nodes'].get('images', {'nodes': {}})['nodes'].items():
        pr = node['props']
        if 'data' in pr: start, size = None, len(pr['data'])
        elif 'data-offset' in pr: start, size = ext + n(pr['data-offset']), n(pr.get('data-size'))
        elif 'data-position' in pr: start, size = base + n(pr['data-position']), n(pr.get('data-size'))
        else: start, size = None, 0
        out.append({'name': name, 'start': start, 'size': size, 'data': pr.get('data'),
                    'partition': s(pr.get('partition') or pr.get('target') or pr.get('load-partition'))})
    return total, out


def fit_size(total, imgs, base):
    return max([total] + [i['start'] - base + i['size'] for i in imgs if i['start'] is not None])


def fit(path, outdir):
    buf = open(path, 'rb').read()
    _, imgs = fit_images(buf)
    os.makedirs(outdir, exist_ok=True)
    print('FIT container, %d payloads' % len(imgs))
    parts = []
    for i in imgs:
        raw = os.path.join(outdir, i['name'] + '.bin')
        data = i['data'] if i['data'] is not None else (b'' if i['start'] is None else buf[i['start']:i['start'] + i['size']])
        open(raw, 'wb').write(data)
        parts.append((i['name'], i['partition'], raw, {}))
    finish(outdir, parts)


def fitscan(path, out):
    """save the biggest FIT container holding a root file system found anywhere in path; prints out if found"""
    buf, best, pos = open(path, 'rb').read(), None, 0
    while (pos := buf.find(struct.pack('>I', FDT_MAGIC), pos)) >= 0:
        try:
            total, imgs = fit_images(buf, pos)
            if any('root' in i['name'] for i in imgs):
                size = fit_size(total, imgs, pos)
                if not best or size > best[1]: best = (pos, size)
        except Exception:
            pass
        pos += 4
    if best:
        open(out, 'wb').write(buf[best[0]:best[0] + best[1]]); print(out)


def rootfs(payloads):
    fs = [x for x in json.load(open(payloads)) if x['contents'] in ('ext', 'squashfs')]
    fs.sort(key=lambda x: ('root' not in (x['name'] + x['partition']).lower(), -x['bytes']))
    if fs: print('%s|%s' % (fs[0]['file'], fs[0]['contents']))


# ============================== factory defaults at every start ==============================
def defaults(root, backup_dir, *keep):
    """New projects start from the factory default (user templates moved to BACKUP, the "User Template" setting
    forgotten) and the FADER controls pad volume (its SHIFT + PAD 9 choice forgotten). Nothing is deleted:
    changed files are copied to BACKUP first."""
    def backup(path, move=False):
        dst = os.path.join(backup_dir, os.path.relpath(path, root))
        if not move and os.path.exists(dst): return   # keep the first (original) copy
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        (shutil.move if move else shutil.copy2)(path, dst)

    def forget(path, test):   # remove the settings whose name passes test, from a JSON or JUCE (XML) file
        text = open(path, encoding='utf-8', errors='replace').read()
        try:
            data = json.loads(text)
            def strip(o):
                if isinstance(o, dict):
                    for k in [k for k in o if test(k)]: del o[k]
                    for v in o.values(): strip(v)
                elif isinstance(o, list):
                    for v in o: strip(v)
            strip(data); out = json.dumps(data, indent=2)
        except ValueError:
            out = re.sub(r'[ \t]*<VALUE name="([^"]*)"[^>]*/>\s*\n?', lambda m: '' if test(m.group(1)) else m.group(0), text)
        if out == text: return False
        backup(path); open(path, 'w', encoding='utf-8').write(out); return True

    files = [os.path.join(d, f) for top in (os.path.join(root, 'data'), os.path.join(root, 'media', 'az01-internal'))
             for d, _, fs in os.walk(top) for f in fs]
    settings = lambda: [p for p in files if os.path.exists(p) and 'settings' in p.lower()]
    if 'keep-template' not in keep:
        moved = [p for p in files if 'template' in os.path.basename(p).lower()]
        for p in moved: backup(p, move=True)
        if moved + [p for p in settings() if forget(p, lambda k: 'templatefile' in k.lower())]:
            print('== Defaults: new projects use the factory default (your template is in %s)' % backup_dir)
    if 'keep-fader' not in keep:
        if [p for p in settings() if forget(p, lambda k: k == 'AC50FaderProperties.TargetMapping')]:
            print('== Defaults: FADER back to pad volume')


# ============================== fake control chip ==============================
def ctl(root, outdir):
    """ROOT/tmp/mpcctl-8.sock, -9.sock: raw MIDI ports (stream, one connection per open); ROOT/tmp/mpcseq.sock:
    the fake ALSA sequencer (packets of [card, MIDI]); ROOT/tmp/mpcctl-in.sock: MIDI from the on-screen panel
    (datagrams of [card, MIDI]) -> every open port of that card. Nothing ever waits for the program.
    MPC_LOG=1 also logs what the program sends to OUTDIR/ctl-<port>.log."""
    os.makedirs(outdir, exist_ok=True)
    verbose, t0, lock = os.environ.get('MPC_LOG') == '1', time.time(), threading.Lock()
    raw, seq = {8: [], 9: []}, []
    log = open(os.path.join(outdir, 'mpcctl.log'), 'a', buffering=1)
    spawn = lambda fn, *a: threading.Thread(target=fn, args=a, daemon=True).start()

    def server(name, kind):
        path = os.path.join(root, 'tmp', name)
        if os.path.lexists(path): os.unlink(path)
        s = socket.socket(socket.AF_UNIX, kind); s.bind(path); os.chmod(path, 0o777)
        if kind != socket.SOCK_DGRAM: s.listen(16)
        return s

    def drain(c, group, name):   # read (and drop, or log) what the program sends; forget the connection at close
        out = open(os.path.join(outdir, name), 'a', buffering=1) if verbose else None
        while True:
            try: d = c.recv(4096)
            except OSError: d = b''
            if not d: break
            if out: out.write('%8.3f <- %s\n' % (time.time() - t0, d.hex(' ')))
        with lock:
            if c in group: group.remove(c)
        log.write('%8.3f %s closed\n' % (time.time() - t0, name))

    def accept(srv, group, name):
        while True:
            c, _ = srv.accept()
            with lock: group.append(c)
            spawn(drain, c, group, name)

    for card in (8, 9): spawn(accept, server('mpcctl-%d.sock' % card, socket.SOCK_STREAM), raw[card], 'ctl-%d.log' % card)
    spawn(accept, server('mpcseq.sock', socket.SOCK_SEQPACKET), seq, 'ctl-seq.log')
    panel = server('mpcctl-in.sock', socket.SOCK_DGRAM)
    while True:
        rec = panel.recv(256)
        if len(rec) < 2: continue
        with lock: targets = [(c, rec[1:]) for c in raw.get(rec[0], [])] + [(c, rec) for c in seq]
        for c, d in targets:
            try: c.send(d, socket.MSG_DONTWAIT)
            except OSError: pass   # a full queue: dropped


# ============================== disk service (edisksd) on D-Bus ==============================
EDISKS = 'io.github.inmusicdev.edisksd'
BLOCK, FS, PROPS = EDISKS + '.Block', EDISKS + '.Filesystem', 'org.freedesktop.DBus.Properties'
ALIGN = dict(y=1, b=4, n=2, q=2, i=4, u=4, x=8, t=8, d=8, s=4, o=4, g=1, a=4, v=1, h=4, **{'(': 8, '{': 8})
FMT = dict(y='B', b='I', n='h', q='H', i='i', u='I', x='q', t='Q', d='d', h='I')


def type_end(sig, i):
    if sig[i] == 'a': return type_end(sig, i + 1)
    if sig[i] in '({':
        close, i = ')' if sig[i] == '(' else '}', i + 1
        while sig[i] != close: i = type_end(sig, i)
    return i + 1


def split_sig(sig):
    out, i = [], 0
    while i < len(sig): j = type_end(sig, i); out.append(sig[i:j]); i = j
    return out


def put(b, t, v):
    c = t[0]; b += b'\0' * (-len(b) % ALIGN[c])
    if c in FMT: b += struct.pack('<' + FMT[c], int(v) if c == 'b' else v)
    elif c in 'sog':
        e = v.encode(); b += struct.pack('<B' if c == 'g' else '<I', len(e)) + e + b'\0'
    elif c == 'v': put(b, 'g', v[0]); put(b, v[0], v[1])
    elif c == '(':
        for st, x in zip(split_sig(t[1:-1]), v): put(b, st, x)
    elif c == 'a':
        et, at = t[1:], len(b); b += b'\0' * 4; b += b'\0' * (-len(b) % ALIGN[et[0]]); start = len(b)
        if et == 'y': b += bytes(v)
        elif et[0] == '{':
            kt, vt = split_sig(et[1:-1])
            for k, x in v.items(): b += b'\0' * (-len(b) % 8); put(b, kt, k); put(b, vt, x)
        else:
            for x in v: put(b, et, x)
        struct.pack_into('<I', b, at, len(b) - start)


def get(m, p, t):
    """value of type t at m[p] -> (value, new p)"""
    c = t[0]; p += -p % ALIGN[c]
    if c in FMT:
        v = struct.unpack_from('<' + FMT[c], m, p)[0]; return (bool(v) if c == 'b' else v), p + struct.calcsize(FMT[c])
    if c in 'sog':
        n, p = (m[p], p + 1) if c == 'g' else (struct.unpack_from('<I', m, p)[0], p + 4)
        return m[p:p + n].decode('utf-8', 'replace'), p + n + 1
    if c == 'v':
        sig, p = get(m, p, 'g'); v, p = get(m, p, sig); return (sig, v), p
    if c == '(':
        out = []
        for st in split_sig(t[1:-1]): v, p = get(m, p, st); out.append(v)
        return tuple(out), p
    n = struct.unpack_from('<I', m, p)[0]; et = t[1:]; p += 4; p += -p % ALIGN[et[0]]; end = p + n
    if et[0] == '{':
        kt, vt = split_sig(et[1:-1]); d = {}
        while p < end: p += -p % 8; k, p = get(m, p, kt); d[k], p = get(m, p, vt)
        return d, p
    out = []
    while p < end: v, p = get(m, p, et); out.append(v)
    return out, p


def disks(sock, mountpoint, logfile):
    """Reports one microSD card (/dev/mmcblk1p1, label SD CARD), mounted at mountpoint, to the MPC program."""
    logf = open(logfile, 'a', buffering=1)
    log = lambda s: logf.write('%s %s\n' % (time.strftime('%H:%M:%S'), s))
    for _ in range(100):
        if os.path.exists(sock): break
        time.sleep(0.1)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sock)
    s.sendall(b'\0AUTH EXTERNAL ' + str(os.getuid()).encode().hex().encode() + b'\r\n')
    line = b''
    while not line.endswith(b'\r\n'):
        ch = s.recv(1)
        if not ch: raise RuntimeError('D-Bus closed')
        line += ch
    if not line.startswith(b'OK'): raise RuntimeError('D-Bus refused us: %r' % line)
    s.sendall(b'BEGIN\r\n')
    st = {'serial': 0, 'buf': b''}

    def send(mtype, fields, sig='', args=(), flags=0):
        body = bytearray()
        for t, v in zip(split_sig(sig), args): put(body, t, v)
        if sig: fields = {**fields, 8: ('g', sig)}
        st['serial'] += 1
        h = bytearray(struct.pack('<cBBBII', b'l', mtype, flags, 1, len(body), st['serial']))
        put(h, 'a(yv)', list(fields.items())); h += b'\0' * (-len(h) % 8)
        s.sendall(bytes(h) + bytes(body))
        return st['serial']

    def recv():
        while True:
            b = st['buf']
            if len(b) >= 16:
                blen, _, flen = struct.unpack_from('<III', b, 4)
                hlen = 16 + flen + (-(16 + flen) % 8)
                if len(b) >= hlen + blen:
                    m, st['buf'] = b[:hlen + blen], b[hlen + blen:]
                    f = {k: v[1] for k, v in get(m, 12, 'a(yv)')[0]}
                    args, p = [], hlen
                    try:
                        for t in split_sig(f.get(8, '')): v, p = get(m, p, t); args.append(v)
                    except Exception as e: log('bad body: %s' % e)
                    return m[1], struct.unpack_from('<I', m, 8)[0], f, args
            d = s.recv(65536)
            if not d: raise RuntimeError('D-Bus closed')
            st['buf'] += d

    pending = []
    def bus_call(member, sig='', args=()):
        serial = send(1, {1: ('o', '/org/freedesktop/DBus'), 2: ('s', 'org.freedesktop.DBus'), 3: ('s', member),
                          6: ('s', 'org.freedesktop.DBus')}, sig, args)
        while True:
            msg = recv()
            if msg[0] in (2, 3) and msg[2].get(5) == serial: return msg[3]
            pending.append(msg)

    log('connected as %s' % (bus_call('Hello') or ['?'])[0])
    log('own %s: %s' % (EDISKS, bus_call('RequestName', 'su', (EDISKS, 4))))
    path = '/io/github/inmusicdev/edisksd/dev/mmcblk1p1'
    objs = {path: {
        BLOCK: {'ConnectionBus': ('s', 'mmc'), 'HintIgnore': ('b', False), 'Removable': ('b', True),
                'DeviceLinks': ('aay', [b'/dev/disk/by-path/platform-fe2c0000.mmc-part1\0', b'/dev/disk/by-label/SD\\x20CARD\0'])},
        FS: {'Label': ('s', 'SD CARD'), 'Mountpoints': ('aay', [mountpoint.encode() + b'\0'])}}}
    log('drive %s -> %s' % (path, mountpoint))

    def introspect(p):
        x = ['<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN" '
             '"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">\n<node>']
        if p in objs:
            x.append('<interface name="%s"><method name="GetAll"><arg type="s" direction="in"/><arg type="a{sv}" '
                     'direction="out"/></method><method name="Get"><arg type="s" direction="in"/><arg type="s" '
                     'direction="in"/><arg type="v" direction="out"/></method></interface>' % PROPS)
            for iface, props in objs[p].items():
                x.append('<interface name="%s">' % iface)
                if iface == FS:
                    x.append('<method name="Mount"><arg name="options" type="a{sv}" direction="in"/><arg name="mount_path" '
                             'type="s" direction="out"/></method><method name="Unmount"><arg name="options" type="a{sv}" '
                             'direction="in"/></method>')
                x += ['<property name="%s" type="%s" access="read"/>' % (k, t) for k, (t, _) in props.items()]
                x.append('</interface>')
        else:
            x.append('<interface name="org.freedesktop.DBus.ObjectManager"><method name="GetManagedObjects">'
                     '<arg type="a{oa{sa{sv}}}" direction="out"/></method></interface>')
            pre = p.rstrip('/') + '/'
            x += ['<node name="%s"/>' % k for k in sorted({o[len(pre):].split('/')[0] for o in objs if o.startswith(pre)})]
        return ''.join(x + ['</node>'])

    while True:
        t, serial, f, args = pending.pop(0) if pending else recv()
        if t != 1: continue
        p, iface, member, to = f.get(1, ''), f.get(2, ''), f.get(3, ''), f.get(7, '')
        reply = lambda sig='', a=(): send(2, {5: ('u', serial), 6: ('s', to)}, sig, a, flags=1)
        error = lambda name, text: send(3, {4: ('s', name), 5: ('u', serial), 6: ('s', to)}, 's', (text,), flags=1)
        log('call %s %s.%s %r from %s' % (p, iface, member, args, to))
        try:
            if member == 'GetManagedObjects':
                reply('a{oa{sa{sv}}}', ({o: {i: dict(pr) for i, pr in ifs.items()} for o, ifs in objs.items()},))
            elif member == 'Introspect': reply('s', (introspect(p),))
            elif member in ('Ping', 'Unmount'): reply()   # Unmount: the phone folder stays attached
            elif iface == PROPS and member == 'GetAll': reply('a{sv}', (dict(objs.get(p, {}).get(args[0], {})),))
            elif iface == PROPS and member == 'Get':
                v = objs.get(p, {}).get(args[0], {}).get(args[1])
                reply('v', (v,)) if v else error('org.freedesktop.DBus.Error.InvalidArgs', 'No such property')
            elif member == 'Mount':
                reply('s', (mountpoint,)) if p in objs else error(EDISKS + '.Failed', 'No such device')
            else: error('org.freedesktop.DBus.Error.UnknownMethod', 'Unknown method %s' % member)
        except Exception as e:
            log('error answering %s: %s' % (member, e))
            try: error(EDISKS + '.Failed', str(e))
            except Exception: pass


# ============================== dispatcher ==============================
CMDS = {'az0x': (az0x, 2), 'fit': (fit, 2), 'fitscan': (fitscan, 2), 'rootfs': (rootfs, 1),
        'defaults': (defaults, -2), 'ctl': (ctl, 2), 'disks': (disks, 3)}

if __name__ == '__main__':
    a = sys.argv[1:]
    fn, n = CMDS.get(a[0] if a else '', (None, 0))
    if not fn or (len(a) - 1 != n if n >= 0 else len(a) - 1 < -n): print(__doc__); sys.exit(2)
    try: fn(*a[1:])
    except (ValueError, lzma.LZMAError, OSError, EOFError) as e:
        print('mpc.py %s: %s' % (a[0], e), file=sys.stderr); sys.exit(1)
