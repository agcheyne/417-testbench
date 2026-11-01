#!/usr/bin/env python3
"""Write the detector face at its stage coordinates with the laser on it, live, like the online channel
map (agcheyne.github.io photonis-ui): every pad with its readout and V1742 input, the readout that is
cabled now highlighted, the laser (THOR_X/Y:read_pos) as a red marker, and the pad under the laser in
yellow. Two sizes:

    phoebus/make_laser_face.py          every detector in db/detectors
      -> phoebus/laser_face_DET.bob         full window (laser_face.sh opens the active detector's)
      -> phoebus/laser_face_small_DET.bob   418 x 392 px, embedded in bench.bob, which shows the active
                                            detector's (DAQ:ACTIVE_DET)
      -> phoebus/laser_face_pads_DET.json   the small face's pads, for bench.bob's click-to-move buttons
      -> laser_face.bob / laser_face_small.bob   copies of the active detector's (as before)
    A detector without stage geometry in its detector.conf (PITCH_X_MM, STAGE_X0_MM ...) gets a face that
    says so.

Generated from the db (detector.conf: NROWS, NCOLS, PITCH_X/Y_MM, STAGE_X0/Y0_MM, STAGE_X/Y_DIR;
mapping.txt; cabling.txt; MASKED_CHANNELS in testbench.conf): run it again after a db change (a new
offset, recabling). Stage x to the right, stage y DOWN: (0, 0) at the top left, as in the gain maps and all
the plots so far (user, 1 Nov 2026; it was drawn y up before).
"""
import os
import re
import sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# (det, slot) whose inputs 0-15 sit one pin over. Photonis readout 0 had that on 27 Oct 2026; the user,
# 1 Nov: "remove the cable slip -- that wont be like that again". Empty: every pad shows the db's input.
SLIP_SLOTS = set()
MARGIN = 4.0    # mm of stage drawn around the detector
LX, LY = 'THOR_X:read_pos', 'THOR_Y:read_pos'
CAB, OTHER, HL = (205, 225, 250), (232, 232, 232), (255, 225, 60)


def conf(path):
    d = {}
    for line in open(path):
        m = re.match(r'\s*([A-Za-z0-9_]+)\s*=\s*"?([^"#]*?)"?\s*(#.*)?$', line)
        if m:
            d[m.group(1)] = m.group(2).strip()
    return d


def col(rgb): return '<color red="%d" green="%d" blue="%d"/>' % rgb


def font(size, bold=False):
    return '<font><font family="Liberation Sans" style="%s" size="%.1f"/></font>' % ('BOLD' if bold else 'REGULAR', size)


W_SMALL, H_SMALL = 418, 392


def placeholder(det, why):
    """a face for a detector that cannot be drawn at stage coordinates"""
    for fname, (W, H) in [('laser_face_%s.bob' % det, (480, 200)), ('laser_face_small_%s.bob' % det, (W_SMALL, H_SMALL))]:
        txt = '%s: no face to draw (%s). Fill in its detector.conf, then run phoebus/make_laser_face.py.' % (det, why)
        open(os.path.join(HERE, fname), 'w').write(
            '<?xml version="1.0" encoding="UTF-8"?>\n<display version="2.0.0"><name>%s laser face</name>'
            '<width>%d</width><height>%d</height><background_color>%s</background_color>'
            '<widget type="label" version="2.0.0"><name>why</name><x>10</x><y>10</y><width>%d</width><height>80</height>'
            '<text>%s</text>%s<wrap_words>true</wrap_words></widget></display>\n'
            % (det, W, H, col((250, 250, 250)), W - 20, escape(txt), font(12, True)))
        print('wrote %s (%s: placeholder, %s)' % (fname, det, why))


def gen(det):
    D = os.path.join(ROOT, 'db/detectors', det)
    dc = conf(os.path.join(D, 'detector.conf'))
    nrows, ncols = int(dc['NROWS']), int(dc['NCOLS'])
    px, py = float(dc['PITCH_X_MM']), float(dc['PITCH_Y_MM'])
    x0, y0 = float(dc['STAGE_X0_MM']), float(dc['STAGE_Y0_MM'])
    xdir, ydir = int(dc.get('STAGE_X_DIR', 1)), int(dc.get('STAGE_Y_DIR', 1))
    masked = set(int(c) for c in conf(os.path.join(ROOT, 'db/testbench.conf')).get('MASKED_CHANNELS', '').split())

    slots = {}                                  # slot -> {(row, col): channel}
    for line in open(os.path.join(D, 'mapping.txt')):
        f = line.split('#')[0].split()
        if len(f) >= 4:
            s, ch, r, c = map(int, f[:4])
            slots.setdefault(s, {})[(r, c)] = ch
    cabled = []                                 # slots read now (cabling.txt: packet slot)
    for line in open(os.path.join(D, 'cabling.txt')):
        f = line.split('#')[0].split()
        if len(f) >= 2:
            cabled.append(int(f[1]))


    def readout_of(slot):
        """the site's readout number of a slot (slot 2 = readout 1 turned by 180 deg)"""
        return {2: 1}.get(slot, slot)


    def input_on(slot, ch):
        if (det, slot) in SLIP_SLOTS and ch < 16:
            return ch + 1 if ch < 15 else None
        return ch


    # each pad: the cabled slot that covers it, else the first slot that does (turned slots only if cabled)
    pads = {}
    for s in sorted(slots, key=lambda s: (s not in cabled, s)):
        if s not in cabled and readout_of(s) != s:
            continue
        for rc, ch in slots[s].items():
            if rc not in pads:
                pads[rc] = (s, input_on(s, ch))


    def cx(c): return x0 + xdir * c * px
    def cy(r): return y0 + ydir * r * py


    xs, ys = [cx(c) for c in range(ncols)], [cy(r) for r in range(nrows)]
    xmin, xmax = min(xs) - px / 2 - MARGIN, max(xs) + px / 2 + MARGIN
    ymin, ymax = min(ys) - py / 2 - MARGIN, max(ys) + py / 2 + MARGIN
    on_det = 'pv0>=%.2f && pv0<=%.2f && pv1>=%.2f && pv1<=%.2f' % (min(xs) - px / 2, max(xs) + px / 2,
                                                                  min(ys) - py / 2, max(ys) + py / 2)
    in_area = 'pv0>=%.2f && pv0<=%.2f && pv1>=%.2f && pv1<=%.2f' % (xmin, xmax, ymin, ymax)


    def rule(name, prop, pvs, exps, out_exp=False):
        s = ['<rule name="%s" prop_id="%s" out_exp="%s">' % (name, prop, 'true' if out_exp else 'false')]
        for b, v in exps:
            tag = 'expression' if out_exp else 'value'
            s.append('<exp bool_exp="%s"><%s>%s</%s></exp>' % (escape(b, {'"': '&quot;'}), tag, escape(v) if out_exp else v, tag))
        s += ['<pv_name>%s</pv_name>' % p for p in pvs]
        s.append('</rule>')
        return ''.join(s)


    def build(small):
        LEFT, TOP = (30, 6) if small else (60, 96)
        PX = (min((W_SMALL - LEFT - 14) / (xmax - xmin), (H_SMALL - TOP - 84) / (ymax - ymin)) if small
              else 8)                                                      # screen px per mm
        WA, HA = int(round((xmax - xmin) * PX)), int(round((ymax - ymin) * PX))
        W, H = (W_SMALL, H_SMALL) if small else (LEFT + WA + 30, TOP + HA + 120)
        out, n = [], [0]

        def sx(x): return LEFT + (x - xmin) * PX
        def sy(y): return TOP + (y - ymin) * PX          # y down: the smallest y at the top

        def widget(wtype, x, y, w, h, version='2.0.0', **props):
            n[0] += 1
            s = ['<widget type="%s" version="%s"><name>%s_%d</name>' % (wtype, version, wtype, n[0]),
                 '<x>%d</x><y>%d</y><width>%d</width><height>%d</height>' % (round(x), round(y), round(w), round(h))]
            for k, v in props.items():
                s.append(v if k.startswith('raw') else '<%s>%s</%s>' % (k, escape(str(v)), k))
            s.append('</widget>')
            out.append(''.join(s))

        def led(x, y, pv, text, size):
            widget('led', x, y, 18, 18, pv_name=pv, raw_on='<on_color>%s</on_color>' % col((255, 150, 0)),
                   raw_off='<off_color>%s</off_color>' % col((90, 90, 90)), square='false')
            label(x + 22, y - 1, 64, 20, text, size=size, color=(70, 70, 70), align=0)

        def label(x, y, w, h, text, size=10, bold=False, color=(25, 25, 25), align=1):
            widget('label', x, y, w, h, text=text, raw_f=font(size, bold),
                   raw_c='<foreground_color>%s</foreground_color>' % col(color), horizontal_alignment=align, vertical_alignment=1)

        # laser position line (full: under the title; small: under the face), with the stage's moving lamps
        if small:
            BY = TOP + HA + 26
            label(4, BY, 92, 22, 'laser x, y [mm]', size=10, color=(110, 110, 110), align=0)
            for i, pv in enumerate([LX, LY]):
                widget('textupdate', 98 + i * 68, BY, 64, 22, pv_name=pv, raw_f=font(12, True), format=1,
                       precision=2, show_units='false', horizontal_alignment=2, vertical_alignment=1)
            led(240, BY + 2, 'THOR_X:ismoving', 'x moving', 9)
            led(324, BY + 2, 'THOR_Y:ismoving', 'y moving', 9)
            warn = (LEFT + 6, TOP + 4, 200, 18, 10)
        else:
            label(10, 6, W - 20, 26, '%s face, laser position (live)' % det, size=15, bold=True, align=0)
            label(10, 32, W - 20, 18, 'stage x to the right, stage y down; pad (0,0) at %.1f, %.1f mm, pitch %.1f mm (db)'
                  % (x0, y0, px), size=10, color=(110, 110, 110), align=0)
            label(10, 56, 70, 22, 'laser x, y', size=11, color=(110, 110, 110), align=0)
            for i, pv in enumerate([LX, LY]):
                widget('textupdate', 82 + i * 96, 56, 90, 22, pv_name=pv, raw_f=font(12, True), format=1, precision=2,
                       show_units='false', horizontal_alignment=2, vertical_alignment=1)
            label(274, 56, 30, 22, 'mm', size=11, color=(110, 110, 110), align=0)
            led(312, 58, 'THOR_X:ismoving', 'x moving', 10)
            led(400, 58, 'THOR_Y:ismoving', 'y moving', 10)
            warn = (10, 76, W - 20, 18, 11)
        widget('label', warn[0], warn[1], warn[2], warn[3], text='laser off the detector', raw_f=font(warn[4], True),
               raw_c='<foreground_color>%s</foreground_color>' % col((200, 0, 0)), horizontal_alignment=0,
               vertical_alignment=0, wrap_words='true', visible='false',
               raw_r='<rules>' + rule('vis', 'visible', [LX, LY], [('!(%s)' % on_det, 'true')]) + '</rules>')

        # drawing area and mm ticks
        widget('rectangle', LEFT, TOP, WA, HA, raw_bg='<background_color>%s</background_color>' % col((250, 250, 250)),
               raw_lc='<line_color>%s</line_color>' % col((160, 160, 160)), line_width=1)
        tick = (9 if small else 9)
        for t in range(int(xmin // 5 + 1) * 5, int(xmax) + 1, 5):
            ln = 6 if t % 10 else 10
            widget('polyline', sx(t), TOP + HA, 1, ln, raw_p='<points><point x="0" y="0"/><point x="0" y="%d"/></points>' % ln,
                   line_width=1, raw_lc='<line_color>%s</line_color>' % col((120, 120, 120)))
            if t % 10 == 0:
                label(sx(t) - 20, TOP + HA + 9, 40, 14, str(t), size=8 if small else tick, color=(90, 90, 90))
        for t in range(int(ymin // 5 + 1) * 5, int(ymax) + 1, 5):
            ln = 6 if t % 10 else 10
            widget('polyline', LEFT - ln, sy(t), ln, 1, raw_p='<points><point x="0" y="0"/><point x="%d" y="0"/></points>' % ln,
                   line_width=1, raw_lc='<line_color>%s</line_color>' % col((120, 120, 120)))
            if t % 10 == 0:
                label(LEFT - (30 if small else 48), sy(t) - 7, 20 if small else 36, 14, str(t), size=8 if small else tick,
                      color=(90, 90, 90), align=2)
        if not small:
            label(LEFT, TOP + HA + 26, WA, 18, 'stage x [mm]', size=10, color=(90, 90, 90))
            widget('label', 4, TOP, 18, HA, text='stage y [mm]', raw_f=font(10), rotation_step=3, horizontal_alignment=1,
                   vertical_alignment=1, raw_c='<foreground_color>%s</foreground_color>' % col((90, 90, 90)))

        # pads
        GAP = 0.6   # mm between pads
        for r in range(nrows):
            for c in range(ncols):
                x, y = cx(c), cy(r)
                slot, inp = pads.get((r, c), (None, None))
                lo_x, hi_x, lo_y, hi_y = x - px / 2, x + px / 2, y - py / 2, y + py / 2
                tip = 'pad row %d col %d, centre %.1f, %.1f mm' % (r, c, x, y)
                if slot is not None:
                    tip += ': readout %d, V1742 input %s%s%s' % (readout_of(slot), inp if inp is not None else '?',
                                                               ' (masked)' if inp in masked and slot in cabled else '',
                                                               ' (cabled now)' if slot in cabled else '')
                under = 'pv0>=%.2f && pv0<%.2f && pv1>=%.2f && pv1<%.2f' % (lo_x, hi_x, lo_y, hi_y)
                widget('rectangle', sx(lo_x + GAP / 2), sy(lo_y + GAP / 2), (px - GAP) * PX, (py - GAP) * PX,
                       raw_bg='<background_color>%s</background_color>' % col(CAB if slot in cabled else OTHER),
                       raw_lc='<line_color>%s</line_color>' % col((150, 160, 175) if slot in cabled else (190, 190, 190)),
                       line_width=1, corner_width=3, corner_height=3, tooltip=tip,
                       raw_r='<rules>' + rule('laser', 'background_color', [LX, LY], [(under, col(HL))]) + '</rules>')
                if slot is None:
                    continue
                dim = slot not in cabled or inp in masked
                txt = 'ch%s' % (inp if inp is not None else '?')
                if small and px * PX < 26:      # too small for labels (Photek's 16 x 16): the tooltip has them
                    continue
                if small:
                    label(sx(lo_x), sy(y) - 15, px * PX, 14, 'R%d' % readout_of(slot), size=8,
                          color=(130, 130, 130) if dim else (60, 70, 90))
                    label(sx(lo_x), sy(y) - 2, px * PX, 16, txt, size=10, bold=not dim,
                          color=(150, 150, 150) if dim else (20, 20, 20))
                else:
                    label(sx(lo_x), sy(y) - 17, px * PX, 16, 'R%d' % readout_of(slot), size=9,
                          color=(120, 120, 120) if dim else (60, 70, 90))
                    label(sx(lo_x), sy(y) - 1, px * PX, 18, txt, size=11, bold=not dim,
                          color=(150, 150, 150) if dim else (20, 20, 20))

        # small face: the pads' rectangles (and centres) for bench.bob, which puts its click-to-move buttons on top
        # (they need the panel's lock, loc://unlocked$(DID), which an embedded display cannot be given)
        if small:
            import json
            json.dump([dict(row=r_, col=c_, x=round(sx(cx(c_) - px / 2)), y=round(sy(cy(r_) - py / 2)),
                            w=round(px * PX), h=round(py * PX), cx=round(cx(c_), 2), cy=round(cy(r_), 2))
                       for r_ in range(nrows) for c_ in range(ncols)],
                      open(os.path.join(HERE, 'laser_face_pads_%s.json' % det), 'w'))

        # the laser: a red ring with a white edge and a dot, centred on the stage position, hidden off the area
        R = 7 if small else 9
        for rad, rgb, lw, fill in [(R + 2, (255, 255, 255), 2, False), (R, (220, 0, 0), 3, False), (2, (220, 0, 0), 1, True)]:
            widget('ellipse', sx(xmin) - rad, sy(ymin) - rad, 2 * rad, 2 * rad, line_width=lw,
                   raw_lc='<line_color>%s</line_color>' % col(rgb), transparent='false' if fill else 'true',
                   raw_bg='<background_color>%s</background_color>' % col(rgb), visible='false',
                   raw_r='<rules>' + rule('x', 'x', [LX], [('true', '%.2f + (pv0 - %.3f) * %.2f' % (LEFT - rad, xmin, PX))], True)
                   + rule('y', 'y', [LY], [('true', '%.2f + (pv0 - %.3f) * %.2f' % (TOP - rad, ymin, PX))], True)
                   + rule('vis', 'visible', [LX, LY], [(in_area, 'true')]) + '</rules>')

        # legend
        items = [(CAB, 'cabled now: readout %s' % ', '.join(str(readout_of(s)) for s in cabled)),
                 (OTHER, 'other readout'), (HL, 'under the laser')]
        if small:
            LY2 = TOP + HA + 52
            for i, (rgb, txt) in enumerate(items):
                x = 6 + i * 138
                widget('rectangle', x, LY2 + 3, 14, 12, raw_bg='<background_color>%s</background_color>' % col(rgb),
                       raw_lc='<line_color>%s</line_color>' % col((150, 150, 150)), line_width=1)
                label(x + 18, LY2, 118, 18, txt, size=9, color=(70, 70, 70), align=0)
            if any(d == det for d, _ in SLIP_SLOTS):
                label(6, LY2 + 18, W - 12, 14, 'R0 ch0-15 shown one pad over (cable slip, 27 Oct; db unchanged)',
                      size=8, color=(120, 120, 120), align=0)
        else:
            ly = TOP + HA + 52
            items[0] = (CAB, items[0][1] + ' (cabling.txt)')
            items[1] = (OTHER, 'other readout (labels: its inputs when cabled)')
            for i, (rgb, txt) in enumerate(items):
                widget('rectangle', 12, ly + i * 20, 16, 14, raw_bg='<background_color>%s</background_color>' % col(rgb),
                       raw_lc='<line_color>%s</line_color>' % col((150, 150, 150)), line_width=1)
                label(34, ly + i * 20 - 2, W - 40, 18, txt, size=10, color=(70, 70, 70), align=0)
            if any(d == det for d, _ in SLIP_SLOTS):
                label(12, ly + 62, W - 20, 16, 'R0 ch0-15 shown one pad over (cable slip, 27 Oct; db unchanged); grey ch: masked input',
                      size=9, color=(120, 120, 120), align=0)
        return W, H, out


    for fname, small in [('laser_face_%s.bob' % det, False), ('laser_face_small_%s.bob' % det, True)]:
        W, H, out = build(small)
        xml = ['<?xml version="1.0" encoding="UTF-8"?>', '<display version="2.0.0">', '<name>%s laser face</name>' % det,
               '<width>%d</width><height>%d</height>' % (W, H),
               '<background_color>%s</background_color>' % col((250, 250, 250) if small else (236, 236, 236))] + out + ['</display>']
        path = os.path.join(HERE, fname)
        open(path, 'w').write('\n'.join(xml) + '\n')
        print('wrote %s (%s, %d x %d px, cabled slot(s) %s, %d widgets)' % (path, det, W, H, cabled, len(out)))


ACTIVE = conf(os.path.join(ROOT, 'state/active.conf'))['DETECTOR']
for det in sorted(d for d in os.listdir(os.path.join(ROOT, 'db/detectors')) if not d.startswith('_')):
    dc = conf(os.path.join(ROOT, 'db/detectors', det, 'detector.conf'))
    need = [k for k in ('NROWS', 'NCOLS', 'PITCH_X_MM', 'PITCH_Y_MM', 'STAGE_X0_MM', 'STAGE_Y0_MM') if not dc.get(k)]
    if need:
        placeholder(det, 'no ' + ', '.join(need) + ' in detector.conf')
    else:
        gen(det)
for fname in ('laser_face.bob', 'laser_face_small.bob'):        # the active detector's, under the old names
    src = fname.replace('.bob', '_%s.bob' % ACTIVE)
    open(os.path.join(HERE, fname), 'w').write(open(os.path.join(HERE, src)).read())
    print('%s = %s' % (fname, src))
