# Generates src/unit_hitboxes.h: for every playable unit with an action table, its melee hit spheres per action,
# keyed by the action hash the game keeps live at [unit + 0x3218] + 0x55c (= word +0xb4 of the action record).
# Each move: start animation hash (+0x6c, e.g. the homing approach, 0 = none), hit animation hash (+0x70),
# first / last active frame of the hit animation (+0x80 / +0x84), next stage (+0xbc: the live hash stays the
# first stage's while a chain goes on, the serial changes), spheres on body bones (live bone array order) or on a
# weapon part's own skeleton (e.g. the long beam saber): the part is found live by its bone layout (distance of
# every bone to its parent in the rest pose), so each part used gets a signature.
# Moves with no hits in the table but a script handler (found through the script's dispatcher by action id) take
# the hits of that function's 'if frame >= X' blocks. Read with the EXVSIB Mod Manager's code.
# Usage: py tools/make_hitboxes.py "<game folder>\data\x64\dplcache_release" [Mod Manager src folder]
import os, sys, struct, re
data = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
mm = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, '..', '..', 'EXVSModManager', 'src')
sys.path[:0] = [os.path.join(mm, d) for d in ('app', 'formats', 'extract', 'modding', 'translation', 'online', 'blender')]
from fhm2d import Fhm2d
import hitboxes as H, model_blender

ids_wanted = [int(m) for m in re.findall(r'\{ (\d+), "', open(os.path.join(here, '..', 'src', 'unit_names.h'), encoding='utf-8').read())]


def read(fid):
    p = os.path.join(data, '0x%08X.fhm2d' % fid)
    return open(p, 'rb').read() if os.path.exists(p) else None


def part_index(raw):
    """SHLL: part hash -> index of the part (entries of 32 bytes: hash, kind, part, ...)."""
    f = Fhm2d(raw)
    for k in range(len(f.records)):
        sb = f.subfile(k)
        if sb[:4] == b'SHLL':
            n = struct.unpack_from('<I', sb, 0xc)[0]
            return dict((struct.unpack_from('<I', sb, 0x10 + 32 * e)[0], struct.unpack_from('<I', sb, 0x10 + 32 * e + 8)[0])
                        for e in range(n) if 0x10 + 32 * e + 12 <= len(sb))
    return {}


def part_bone_map(raw, part):
    """Local bone index (as the shapes table uses it) -> index in the part's live bone array, from its JNTT."""
    f = Fhm2d(raw)
    jn = [sb for sb in (f.subfile(k) for k in range(len(f.records))) if sb[:4] == b'JNTT']
    if part['k'] >= len(jn):
        return {}
    d = jn[part['k']]
    n = struct.unpack_from('<I', d, 8)[0]
    out = {}
    for i in range(n):
        if 0x10 + 8 * i + 8 > len(d):
            break
        local, glob = struct.unpack_from('<II', d, 0x10 + 8 * i)
        if local < 0x10000 and glob < len(part['parents']):
            out[local] = glob
    return out


def signature(part):
    """(parent index, rest distance to the parent) of every bone, from the rest pose (model space)."""
    names = [b for b, _ in part['parents']]
    out = []
    for b, par in part['parents']:
        t = part['bones'][b][3]
        if par < 0:
            out.append((-1, 0.0))
        else:
            q = part['bones'][names[par]][3]
            out.append((par, sum((t[j] - q[j]) ** 2 for j in range(3)) ** 0.5))
    return out


def script_handlers(sraw):
    """Moves that live in the script (e.g. Darilbalde's claw grab): a dispatcher compares the action id (record +0x4)
    with constants and stores the handler function: push id, load l0, eq, jz, push function, store l1.
    Returns ({action id: function}, {function: instructions})."""
    if not sraw:
        return {}, {}
    try:
        fns = H._script_functions(sraw)
    except Exception:
        return {}, {}
    out = {}
    for ins in fns.values():
        for i in range(len(ins) - 5):
            a, b, c, d_, e, f = ins[i:i + 6]
            if (a[1] == 10 and isinstance(a[2], int) and b[1] == 11 and b[2] == ('l', 0) and c[1] == 37 and d_[1] == 52
                    and e[1] == 10 and e[2] in fns and f[1] == 28 and f[2] == ('l', 1)):
                out.setdefault(a[2], e[2])
    return out, fns


def moves_of(praw, uid, raw, parts, sraw=None):
    tables, actions = H.read_params(praw, uid)
    shapes = next((t for t in tables.values() if H.C_BONE in t.cols and H.C_RADIUS in t.cols and H.C_HIT in t.cols), None)
    hits = next((t for t in tables.values() if H.C_HIT_DAMAGE in t.cols), None)
    projs = next((t for t in tables.values() if H.C_PROJ_HIT in t.cols), None)
    if shapes is None or not actions:
        return []
    by_hit = {}
    for rid in shapes.ids:
        by_hit.setdefault(shapes.get(rid, H.C_HIT), []).append(rid)

    def hit_ids(x, depth=0):
        if hits is not None and x in hits.row:
            return [x]
        if projs is None or x not in projs.row or depth > 3:
            return []
        out = []
        h = projs.get(x, H.C_PROJ_HIT)
        if hits is not None and h in hits.row:
            out.append(h)
        for c in H.C_PROJ_CHILDREN:
            if c in projs.cols:
                h = projs.get(x, c)
                if h != x:
                    out += hit_ids(h, depth + 1)
        return out

    me = H.self_hash(raw)
    bones = H.body_bone_names(raw, parts)
    order = [b for b, _ in model_blender.body_part(parts)['parents']]
    pidx = part_index(raw)
    by_k = dict((p['k'], p) for p in parts)
    maps = {}
    handlers, fns = script_handlers(sraw)

    def spheres_of(hids):
        sph = []
        for hid in hids:
            for rid in by_hit.get(hid, ()):
                bi, ph = shapes.get(rid, H.C_BONE), shapes.get(rid, H.C_PART)
                xyz_r = (shapes.get(rid, H.C_X, '<f'), shapes.get(rid, H.C_Y, '<f'), shapes.get(rid, H.C_Z, '<f'), shapes.get(rid, H.C_RADIUS, '<f'))
                if ph == me:
                    if bi in bones and bones[bi] in order:
                        sph.append((-1, order.index(bones[bi])) + xyz_r)
                    continue
                k = pidx.get(ph)                                       # a weapon part with its own skeleton
                part = by_k.get(k)
                if part is None or len(part['parents']) < 2 or len(part['parents']) > 64:
                    continue
                if k not in maps:
                    maps[k] = part_bone_map(raw, part)
                if bi in maps[k]:
                    sph.append((k, maps[k][bi]) + xyz_r)
        return sph

    out, seen = [], set()
    for r in actions:
        h, first, last = r[0xb4 // 4], r[0x80 // 4], r[0x84 // 4]
        if not h or h in seen or last < first or last > 10000:
            continue
        sph = spheres_of({x for v in r for x in hit_ids(v)})
        s0, s1 = r[0x6c // 4], r[0x70 // 4]
        fn = handlers.get(r[1]) if not sph else None
        if fn is not None:                                         # the move is scripted: hits in "if frame >= X" blocks
            ins = fns[fn]
            blocks = H._frame_blocks(ins)
            got = []
            for pc, op, a in ins:
                if op != 10 or not isinstance(a, int):
                    continue
                hs = [x for x in hit_ids(a) if x in by_hit]
                inside = [b for b in blocks if b[1] <= pc < b[2]] if hs else []
                if not inside:
                    continue
                f0 = int(inside[-1][0])
                later = sorted(fb for fb, _s, _e in blocks if fb > f0)
                got.append((f0, max(f0, int(later[0]) - 1 if later else f0 + 2), spheres_of(hs)))
            got = [g for g in got if g[2]]
            if got:
                first, last = min(g[0] for g in got), max(g[1] for g in got)
                sph = [x for g in got for x in g[2]]
                s0 = s1 = 0                                        # frames of the script count from the action start
        seen.add(h)                    # kept even without spheres: a stage of a chain (N1 -> N2 -> N3)
        out.append((h, s0, s1, first, last, r[0xbc // 4], sorted(set(sph))))
    return out


d = Fhm2d(read(0x036B9E67)).subfile(0)
n, st = struct.unpack_from('<II', d, 0x10)
index = dict((u, i) for i, u in enumerate(struct.unpack_from('<%dI' % n, d, 0x20)))
units, moves, spheres, fails = [], [], [], 0
part_ids, hparts, sigs = {}, [], []
for uid in ids_wanted:
    if uid not in index:
        continue
    model, _e, _s, params, script, _a = struct.unpack_from('<6I', d, 0x20 + 4 * n + st * index[uid])
    raw, praw = read(model), read(params)
    if not raw or not praw:
        continue
    try:
        parts = model_blender.pieces(raw)[0]
        ms = moves_of(praw, uid, raw, parts, read(script))
    except Exception:
        fails += 1
        continue
    if not any(m[-1] for m in ms):
        continue
    ms.sort()
    units.append((uid, len(moves), len(ms)))
    by_k = dict((p['k'], p) for p in parts)
    for h, s0, s1, a, b, nx, sph in ms:
        moves.append((h, s0, s1, a, b, nx, len(spheres), len(sph)))
        for k, bone, x, y, z, r in sph:
            pid = 0xffff
            if k >= 0:
                if (uid, k) not in part_ids:
                    sg = signature(by_k[k])
                    part_ids[(uid, k)] = len(hparts)
                    hparts.append((len(sg), len(sigs), by_k[k]['name']))
                    sigs += sg
                pid = part_ids[(uid, k)]
            spheres.append((pid, bone, x, y, z, r))
out = os.path.join(here, '..', 'src', 'unit_hitboxes.h')
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// Generated by tools/make_hitboxes.py from the game data. Do not edit.\n#pragma once\n')
    f.write('struct HitSphere { unsigned short part; unsigned char bone; float x, y, z, r; };   // part 0xffff = body\n')
    f.write('struct HitPart { unsigned short nbones; unsigned first_sig; };   // a weapon part with its own skeleton\n')
    f.write('struct HitSig { short parent; float dist; };   // rest pose: parent bone, distance to it\n')
    f.write('static const HitPart HIT_PARTS[] = {\n%s\n};\n' % '\n'.join('    { %d, %d },   // %s' % hp for hp in hparts))
    f.write('static const HitSig HIT_SIGS[] = {\n%s\n};\n' % '\n'.join('    { %d, %.4ff },' % sg for sg in sigs))
    f.write('struct HitMove { unsigned hash, start_anim, hit_anim; unsigned short first, last; unsigned next, first_sphere; unsigned short count; };\n')
    f.write('struct HitUnit { unsigned id; unsigned first, count; };\n')
    f.write('static const HitSphere HIT_SPHERES[] = {\n%s\n};\n' % '\n'.join(
        '    { 0x%x, %d, %.4ff, %.4ff, %.4ff, %.4ff },' % s for s in spheres))
    f.write('static const HitMove HIT_MOVES[] = {   // sorted by hash within each unit\n%s\n};\n' % '\n'.join(
        '    { 0x%08x, 0x%08x, 0x%08x, %d, %d, 0x%08x, %d, %d },' % m for m in moves))
    f.write('static const HitUnit HIT_UNITS[] = {   // sorted by id\n%s\n};\n' % '\n'.join(
        '    { %d, %d, %d },' % u for u in units))
print('%d units, %d moves, %d spheres (%d weapon parts), %d failed -> %s' % (len(units), len(moves), len(spheres), len(hparts), fails, os.path.normpath(out)))
