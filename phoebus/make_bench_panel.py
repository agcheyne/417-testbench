#!/usr/bin/env python3
"""Write bench.bob, the Phoebus panel of the 417 test bench (docs/epics.md).

    phoebus/make_bench_panel.py            -> phoebus/bench.bob
    phoebus/bench_panel.sh                 opens it

The panel only reads and writes PVs of the IOCs (N1470 on CA port 5070, PiLas 5072, ThorLabs
motors 5064); it never talks to a serial port. Generated, so that the 8 HV rows and the repeated
widgets stay consistent: edit this file, not bench.bob.

The lock: loc://unlocked$(DID) is local to one window. Locked, every write widget is disabled,
except ALL OFF, laser emission OFF and the alarm sound mute. Unlocked: setpoints, channel
OFF, PDWN, laser frequency and tune, and the box-safe arms; an ON button also needs its IOC's
BOX_SAFE armed (the IOC refuses it otherwise). The window relocks itself after 5 min without a
write (the script on the lock box).
"""
import os
import sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
# "--unlocked-copy FILE": also write FILE with the lock open at start (for screenshots in a test)
UNLOCK = 'loc://unlocked$(DID)<VLong>(0)'
RELOCK_LEFT = 'loc://relock_left$(DID)<VLong>(0)'
RELOCK_S = 300
LAST_WRITE = 'loc://last_write$(DID)<VDouble>(0)'
FACE_VIEW = 'loc://face_view$(DID)<VString>("active")'   # the face shown: "active" or a detector (this window only)

UNITS = [('A', 236), ('B', 1593)]
STAT_LABELS = ['RUP', 'RDW', 'OVC', 'OVV', 'UNV', 'MAXV', 'TRIP', 'OVP', 'OVT', 'DIS', 'KILL', 'ILK']

C = dict(  # colours
    bg=(236, 236, 236), panel=(250, 250, 250), text=(25, 25, 25), dim=(130, 130, 130),
    unused=(214, 214, 214), head=(60, 70, 90), white=(255, 255, 255),
    green=(0, 170, 60), red=(220, 30, 30), orange=(255, 150, 0), yellow=(250, 210, 0),
    off=(90, 90, 90), blue=(40, 100, 200), minor_bg=(255, 228, 180), major_bg=(255, 200, 200),
    lock=(120, 120, 120), unlocked=(255, 190, 60), alloff=(200, 0, 0))

out = []
n_widgets = [0]


def color(tag, rgb):
    return '<%s><color red="%d" green="%d" blue="%d"/></%s>' % ((tag,) + tuple(rgb) + (tag,))


def font(size=12, bold=False):
    return ('<font><font family="Liberation Sans" style="%s" size="%.1f"/></font>'
            % ('BOLD' if bold else 'REGULAR', size))


def widget(wtype, x, y, w, h, version='2.0.0', name=None, **props):
    n_widgets[0] += 1
    s = ['<widget type="%s" version="%s">' % (wtype, version),
         '<name>%s</name>' % escape(name or '%s_%d' % (wtype, n_widgets[0])),
         '<x>%d</x><y>%d</y><width>%d</width><height>%d</height>' % (x, y, w, h)]
    for k, v in props.items():
        s.append(v if k.startswith('raw') else '<%s>%s</%s>' % (k, escape(str(v)), k))
    s.append('</widget>')
    out.append(''.join(s))


def rule(name, prop, pvs, cases, out_exp=False):
    """cases: [(bool_exp, value_xml)]; the first true one wins, else the widget's own value"""
    s = ['<rule name="%s" prop_id="%s" out_exp="%s">' % (name, prop, 'true' if out_exp else 'false')]
    for exp, val in cases:
        s.append('<exp bool_exp="%s"><value>%s</value></exp>' % (escape(exp, {'"': '&quot;'}), val))
    s += ['<pv_name>%s</pv_name>' % escape(p) for p in pvs]
    s.append('</rule>')
    return ''.join(s)


def rules(*r):
    return '<rules>' + ''.join(r) + '</rules>'


def colval(rgb):
    return '<color red="%d" green="%d" blue="%d"/>' % tuple(rgb)


def label(x, y, w, h, text, size=12, bold=False, fg='text', align=0, **kw):
    widget('label', x, y, w, h, text=text, raw_f=font(size, bold), raw_c=color('foreground_color', C[fg]),
           horizontal_alignment=align, vertical_alignment=1, **kw)


def update(x, y, w, h, pv, size=12, bold=False, fmt=None, prec=None, units=True, align=2, alarm=True, **kw):
    p = dict(pv_name=pv, raw_f=font(size, bold), horizontal_alignment=align, vertical_alignment=1,
             show_units='true' if units else 'false', border_alarm_sensitive='true' if alarm else 'false')
    if fmt is not None:
        p['format'] = fmt
    if prec is not None:
        p['precision'] = prec
    p.update(kw)
    widget('textupdate', x, y, w, h, **p)


def led(x, y, w, h, pv, on='green', off='off', label_=None, **kw):
    widget('led', x, y, w, h, pv_name=pv, raw_on=color('on_color', C[on]), raw_off=color('off_color', C[off]),
           square='false', **kw)
    if label_:
        label(x + w + 4, y, 170, h, label_, size=11)


def enabled_rule(extra_pvs=(), extra_exp=''):
    pvs = [UNLOCK] + list(extra_pvs)
    exp = 'pv0==1' + extra_exp
    return rule('lock', 'enabled', pvs, [(exp, 'true')])


def entry(x, y, w, h, pv, prec=None, extra_pvs=(), extra_exp=''):
    p = dict(pv_name=pv, raw_f=font(11), horizontal_alignment=2, show_units='false', enabled='false',
             raw_r=rules(enabled_rule(extra_pvs, extra_exp)), raw_bg=color('background_color', C['white']))
    if prec is not None:
        p['format'] = 1
        p['precision'] = prec
    widget('textentry', x, y, w, h, version='3.0.0', **p)


def button(x, y, w, h, text, writes, bg='head', fg='white', locked_ok=False, extra_pvs=(), extra_exp='',
           size=11, tooltip=None, vis=None):
    """vis = (pvs, bool_exp): shown only while bool_exp holds"""
    acts = ''.join('<action type="write_pv"><pv_name>%s</pv_name><value>%s</value><description>%s</description>'
                   '</action>' % (escape(pv), escape(str(v)), escape(text)) for pv, v in writes)
    p = dict(raw_a='<actions execute_as_one="true">%s</actions>' % acts, text=text, raw_f=font(size, True),
             raw_bg=color('background_color', C[bg]), raw_fg=color('foreground_color', C[fg]))
    if tooltip:
        p['tooltip'] = tooltip
    rl = []
    if not locked_ok:
        p['enabled'] = 'false'
        rl.append(enabled_rule(extra_pvs, extra_exp))
    if vis:
        p['visible'] = 'false'
        rl.append(rule('vis', 'visible', vis[0], [(vis[1], 'true')]))
    if rl:
        p['raw_r'] = rules(*rl)
    widget('action_button', x, y, w, h, version='3.0.0', **p)


def group(x, y, w, h, title):
    widget('rectangle', x, y, w, h, raw_bg=color('background_color', C['panel']),
           raw_l=color('line_color', (190, 190, 190)), line_width=1, corner_width=6, corner_height=6)
    label(x + 10, y + 4, w - 20, 22, title, size=13, bold=True, fg='head')


def relock_scripts():
    """relock.py on the lock and a ticker; one touch.py per IOC watching its setpoints (a script runs only
    while all its PVs are connected: an IOC that is down must not stop the relock)"""
    def script(name, pvs):
        return ('<script file="EmbeddedPy"><text><![CDATA[%s]]></text>%s</script>'
                % (open(os.path.join(HERE, name)).read(),
                   ''.join('<pv_name trigger="%s">%s</pv_name>' % (t, escape(pv)) for pv, t in pvs)))
    hv = ['N1470_%s:CH%d:%s_SP' % (u, ch, p) for u, sn in UNITS for ch in range(4)
          for p in ('VSET', 'ISET', 'RUP', 'RDW', 'TRIP', 'PDWN')] + ['N1470:BOX_SAFE']
    laser = ['PILAS:F_SP', 'PILAS:TUNE_SP', 'PILAS:BOX_SAFE']
    bench = ['STAGE:X_SP', 'STAGE:Y_SP', 'STAGE:STEP', 'DAQ:RUNTYPE_SP', 'DAQ:SETUP_DET', 'DAQ:SETUP_DAQ']
    out = script('relock.py', [(UNLOCK, 'true'), ('sim://ramp(0,1000000,1,1)', 'true'), (RELOCK_LEFT, 'false'),
                              (LAST_WRITE, 'false')])
    for group_pvs in (hv, laser, bench):
        out += script('touch.py', [(LAST_WRITE, 'false')] + [(pv, 'true') for pv in group_pvs])
    return out


# ---------------------------------------------------------------------------------------------
W, H = 1900, 900

# ---- status bar ----
widget('rectangle', 0, 0, W, 82, raw_bg=color('background_color', C['head']), line_width=0)
label(12, 6, 260, 30, '417 test bench', size=20, bold=True, fg='white')
label(12, 38, 260, 20, 'IOCs: N1470 :5070, PiLas :5072, motors :5064', size=10, fg='white')
lamps = [
    ('N1470:INUSE', 'red', 'HV in use'),
    ('PILAS:LD', 'red', 'laser emission'),
    ('N1470_A:BDILK', 'red', 'interlock A'),
    ('N1470_B:BDILK', 'red', 'interlock B'),
    ('N1470_A:A_BOARD', 'red', 'board alarm A'),
    ('N1470_B:A_BOARD', 'red', 'board alarm B'),
    ('N1470:BOX_SAFE', 'orange', 'HV box-safe armed'),
    ('PILAS:BOX_SAFE', 'orange', 'laser box-safe armed'),
    ('N1470_A:SN_OK', 'green', 'unit A (s/n 236)'),
    ('N1470_B:SN_OK', 'green', 'unit B (s/n 1593)'),
    ('N1470:ANN_ALIVE', 'green', 'annunciator'),
    ('BENCH:PUB_ALIVE', 'green', 'DAQ publisher'),
]
for i, (pv, on, txt) in enumerate(lamps):
    x = 280 + (i // 2) * 168
    y = 8 + (i % 2) * 36
    widget('led', x, y, 26, 26, pv_name=pv, raw_on=color('on_color', C[on]), raw_off=color('off_color', C['off']),
           square='false')
    label(x + 30, y, 136, 26, txt, size=11, fg='white')
update(1294, 46, 200, 20, 'N1470:ARM_LEFT', size=10, units=True, align=0, alarm=False,
       raw_fg=color('foreground_color', C['white']), transparent='true',
       raw_r=rules(rule('vis', 'visible', ['N1470:BOX_SAFE'], [('pv0==1', 'true')])), visible='false')
# alarm banner: text of the active alarms, coloured by the bench severity
widget('textupdate', 1300, 6, W - 1306, 70, pv_name='N1470:ALARM_TXT', format=6, raw_f=font(13, True),
       horizontal_alignment=0, vertical_alignment=0, wrap_words='true', border_alarm_sensitive='false',
       raw_bg=color('background_color', C['green']), raw_fg=color('foreground_color', C['white']),
       raw_r=rules(rule('severity', 'background_color', ['N1470:ALARM.SEVR', 'N1470:ANN_ALIVE'],
                        [('pv1!=1', colval(C['off'])), ('pv0>=2', colval(C['red'])),
                         ('pv0>=1', colval(C['orange']))])),
       tooltip='Active alarms (written by the annunciator, bin/hv_annunciator.sh). Grey: the annunciator is down; '
               'the red / orange outlines below still come from the IOC.')

# ---- HV table ----
TX, TY = 6, 88
group(TX, TY, W - 12, 300, 'CAEN N1470 HV   (V, uA, V/s, s)')
cols = [('sel', 22), ('ch', 44), ('electrode', 110), ('VMON', 78), ('IMON', 78), ('VSET', 66), ('set', 66),
        ('ISET', 70), ('set', 70), ('RUP', 36), ('set', 44), ('RDW', 36), ('set', 44), ('TRIP', 42),
        ('set', 48), ('MAXV', 46), ('PDWN', 44), ('set', 70), ('ON', 28), ('STAT', 12 * 28),
        ('', 50), ('', 50), ('alarm', 44), ('', 40), ('', 40)]
xs = []
x = TX + 8
for name, w in cols:
    xs.append((x, w))
    x += w + 4
hy = TY + 26
for (name, w), (cx, cw) in zip(cols, xs):
    if name == 'STAT':
        for b, lab in enumerate(STAT_LABELS):
            label(cx + b * 28, hy, 28, 18, lab, size=8, bold=True, fg='dim', align=1)
    else:
        label(cx, hy, cw, 18, name, size=10, bold=True, fg='dim', align=1 if name != 'electrode' else 0)
ROW_H, ROW0 = 31, hy + 20
row = 0
for u, sn in UNITS:
    P = 'N1470_' + u
    for ch in range(4):
        y = ROW0 + row * ROW_H
        Q = '%s:CH%d:' % (P, ch)
        # row background: grey when not used, outline red / orange by the channel's alarm severity
        widget('rectangle', TX + 4, y - 2, W - 20, ROW_H - 2, raw_bg=color('background_color', C['white']),
               raw_l=color('line_color', C['white']), line_width=3,
               raw_r=rules(rule('used', 'background_color', [Q + 'USED', Q + 'ALARM.SEVR'],
                                [('pv1>=2', colval(C['major_bg'])), ('pv1>=1', colval(C['minor_bg'])),
                                 ('pv0!=1', colval(C['unused']))]),
                           rule('outline', 'line_color', [Q + 'ALARM.SEVR'],
                                [('pv0>=2', colval(C['red'])), ('pv0>=1', colval(C['orange']))])))
        it = iter(xs)
        cx, cw = next(it)
        widget('checkbox', cx + 2, y + 4, 20, 20, pv_name=P + ':SEL', bit=ch, label='', enabled='false',
               raw_r=rules(rule('used', 'enabled', [Q + 'USED'], [('pv0==1', 'true')])),
               tooltip='pick this channel for "ON selected" / "OFF selected" (shared by all windows); only '
                       "channels in the detector's hv.conf")
        cx, cw = next(it); label(cx, y, cw, 28, '%s ch%d' % (u, ch), size=12, bold=True)
        cx, cw = next(it); update(cx, y, cw, 28, Q + 'NAME', size=12, bold=True, align=0, alarm=False,
                                  raw_r=rules(rule('unused', 'foreground_color', [Q + 'USED'],
                                                   [('pv0!=1', colval(C['dim']))])))
        cx, cw = next(it); update(cx, y, cw, 28, Q + 'VMON', size=13, bold=True, fmt=1, prec=1, units=False)
        cx, cw = next(it); update(cx, y, cw, 28, Q + 'IMON', size=13, bold=True, fmt=1, prec=2, units=False)
        cx, cw = next(it); update(cx, y, cw, 28, Q + 'VSET', fmt=1, prec=1, units=False)
        cx, cw = next(it); entry(cx, y + 2, cw, 24, Q + 'VSET_SP', prec=1)
        cx, cw = next(it); update(cx, y, cw, 28, Q + 'ISET', fmt=1, prec=2, units=False)
        cx, cw = next(it); entry(cx, y + 2, cw, 24, Q + 'ISET_SP', prec=2)
        for par in ('RUP', 'RDW', 'TRIP'):
            cx, cw = next(it); update(cx, y, cw, 28, Q + par, fmt=1, prec=1 if par == 'TRIP' else 0, units=False)
            cx, cw = next(it); entry(cx, y + 2, cw, 24, Q + par + '_SP', prec=1 if par == 'TRIP' else 0)
        cx, cw = next(it); update(cx, y, cw, 28, Q + 'MAXV', fmt=1, prec=0, units=False)
        cx, cw = next(it); update(cx, y, cw, 28, Q + 'PDWN', units=False, align=1)
        cx, cw = next(it)
        widget('combo', cx, y + 2, cw, 24, pv_name=Q + 'PDWN_SP', items_from_pv='true', enabled='false',
               raw_f=font(10), raw_r=rules(enabled_rule()))
        cx, cw = next(it)
        widget('led', cx + 3, y + 3, 22, 22, pv_name=Q + 'STAT', bit=0, raw_on=color('on_color', C['red']),
               raw_off=color('off_color', C['off']), square='false', tooltip='channel ON')
        cx, cw = next(it)
        widget('byte_monitor', cx, y + 3, 12 * 28, 22, pv_name=Q + 'STAT', startBit=1, numBits=12,
               horizontal='true', square='true', bitReverse='true', raw_on=color('on_color', C['yellow']),
               raw_off=color('off_color', (225, 225, 225)))
        cx, cw = next(it)
        button(cx, y + 2, cw, 24, 'OFF', [(Q + 'OFF', 1)], bg='blue',
               tooltip='switch this channel off (unlocked)')
        cx, cw = next(it)
        button(cx, y + 2, cw, 24, 'ON', [(Q + 'ON', 1)], bg='red', extra_pvs=['N1470:BOX_SAFE', Q + 'USED'],
               extra_exp=' && pv1==1 && pv2==1',
               tooltip="switch on: unlock, then arm HV box-safe. Only channels in the detector's hv.conf "
                       "(the IOC refuses the others)")
        cx, cw = next(it)
        widget('multi_state_led', cx + 11, y + 3, 22, 22, pv_name=Q + 'ALARM.SEVR', square='false',
               raw_states='<states>'
               '<state><value>0</value><label/>' + color('color', C['green']) + '</state>'
               '<state><value>1</value><label/>' + color('color', C['orange']) + '</state>'
               '<state><value>2</value><label/>' + color('color', C['red']) + '</state>'
               '<state><value>3</value><label/>' + color('color', (200, 0, 200)) + '</state></states>',
               tooltip='channel alarm: green none, orange minor, red major')
        cx, cw = next(it)
        button(cx, y + 2, cw, 24, 'test', [(Q + 'TEST', 1)], bg='orange', fg='text', size=9,
               tooltip='a test alarm on this channel for 30 s: banner, log, sound, pop-ups (unlocked)')
        cx, cw = next(it)
        button(cx, y + 2, cw, 24, 'ack', [(Q + 'ACK', 1)], bg='head', size=9, locked_ok=True,
               tooltip="acknowledge: this channel's alarm stops sounding until it clears (locked too)")
        row += 1

# picked channels (the check boxes): ON needs the unlock and the HV box-safe arm, as each ON does
button(W - 476, TY + 2, 118, 22, 'ON selected', [('N1470_A:SEL:ON', 1), ('N1470_B:SEL:ON', 1)], bg='red', size=10,
       extra_pvs=['N1470:BOX_SAFE'], extra_exp=' && pv1==1',
       tooltip='switch the checked channels on, both units (unlock, then arm HV box-safe)')
button(W - 352, TY + 2, 118, 22, 'OFF selected', [('N1470_A:SEL:OFF', 1), ('N1470_B:SEL:OFF', 1)], bg='blue', size=10,
       tooltip='switch the checked channels off (unlocked; ALL HV OFF works locked)')
button(W - 228, TY + 2, 100, 22, 'clear', [('N1470_A:SEL', 0), ('N1470_B:SEL', 0)], bg='head', size=10, locked_ok=True,
       tooltip='un-check all channels')

# ---- strip charts ----
SY = 394
LOW_H = H - SY - 6
TRACE_COL = [(220, 30, 30), (240, 120, 0), (200, 170, 0), (120, 60, 160), (0, 120, 220), (0, 160, 160),
             (0, 150, 60), (120, 120, 120)]


def strip(x, y, w, h, title, par, unit):
    traces = []
    i = 0
    for u, sn in UNITS:
        for ch in range(4):
            traces.append('<trace><name>%s ch%d</name><y_pv>N1470_%s:CH%d:%s</y_pv><axis>0</axis>'
                          '<trace_type>2</trace_type>%s<line_width>2</line_width><point_type>0</point_type>'
                          '<point_size>10</point_size><visible>true</visible></trace>'
                          % (u, ch, u, ch, par, color('color', TRACE_COL[i])))
            i += 1
    widget('stripchart', x, y, w, h, version='2.1.0', title=title, start='1 hour',
           show_toolbar='false', show_legend='true',
           raw_y='<y_axes><y_axis><title>%s</title><autoscale>true</autoscale><log_scale>false</log_scale>'
                 '<minimum>0.0</minimum><maximum>100.0</maximum><show_grid>true</show_grid><visible>true</visible>'
                 '</y_axis></y_axes>' % unit,
           raw_t='<traces>%s</traces>' % ''.join(traces))


group(6, SY, 600, LOW_H, 'Last hour (what this window has seen)')
strip(12, SY + 26, 588, (LOW_H - 32) // 2, 'VMON', 'VMON', 'V')
strip(12, SY + 26 + (LOW_H - 32) // 2, 588, (LOW_H - 32) // 2, 'IMON', 'IMON', 'uA')

# ---- laser ----
LX, LY, LW = 612, SY, 420
group(LX, LY, LW, 334, 'PiLas laser')
rows = [('emission', 'PILAS:LD', None), ('frequency', 'PILAS:F', 'PILAS:F_SP'),
        ('tune', 'PILAS:TUNE', 'PILAS:TUNE_SP'), ('tune mode', 'PILAS:TM', None),
        ('trigger', 'PILAS:TS', None), ('edge', 'PILAS:TE', None), ('level', 'PILAS:TL', None),
        ('head temp.', 'PILAS:LHT', None), ('LD temp.', 'PILAS:LDTEMP', None), ('LD TEC', 'PILAS:LDTEC', None),
        ('last error', 'PILAS:LAST_ERR', None)]
for i, (txt, pv, sp) in enumerate(rows):
    y = LY + 28 + i * 24
    label(LX + 12, y, 100, 22, txt, size=11, fg='dim')
    update(LX + 112, y, 170 if pv != 'PILAS:LAST_ERR' else 296, 22, pv, size=12, bold=(i == 0), align=0)
    if sp:
        entry(LX + 290, y, 110, 22, sp, prec=1 if 'TUNE' in sp else 0)
button(LX + 12, LY + 296, 126, 30, 'emission OFF', [('PILAS:LD_OFF', 1)], bg='blue', locked_ok=True,
       tooltip='always works, locked too')
button(LX + 144, LY + 296, 126, 30, 'arm laser 10 s', [('PILAS:BOX_SAFE', 1)], bg='orange', fg='text',
       tooltip='the dark box is closed and the detector is safe to light (unlocked)')
update(LX + 300, LY + 28, 108, 22, 'PILAS:ARM_LEFT', size=11, bold=True, align=0, alarm=False, visible='false',
       raw_r=rules(rule('vis', 'visible', ['PILAS:BOX_SAFE'], [('pv0==1', 'true')])))
button(LX + 276, LY + 296, 132, 30, 'emission ON', [('PILAS:LD_ON', 1)], bg='red', extra_pvs=['PILAS:BOX_SAFE'],
       extra_exp=' && pv1==1', tooltip='unlock, arm, then on')

# ---- stage: no box of its own; x, y, the moving lamps and the pad under the laser are on the detector
# face (laser_face_small.bob, right column) ----
GX, GW = 1038, 420

# ---- DAQ and scan, with the rcdaq controls (the panel writes DAQ:CMD / DAQ:RUNTYPE_SP / DAQ:SETUP_*;
# bin/bench_publisher.sh executes them and refuses what is not allowed; DAQ:CMD_MSG says what happened).
# Locked: only End. Run type, logging and setup also only while stopped, and nothing during a scan. ----
DX, DY, DW = GX, SY, GW
DAQ_H = 334                    # the whole column (the stage box is under the face now)
group(DX, DY, DW, DAQ_H, 'DAQ (rcdaq) and scan')
led(DX + 12, DY + 28, 22, 22, 'DAQ:RUNNING', on='green', label_=None)
update(DX + 40, DY + 28, 80, 22, 'DAQ:RUNNING', size=12, bold=True, align=0, alarm=False)
label(DX + 124, DY + 28, 30, 22, 'run', size=10, fg='dim')
update(DX + 154, DY + 28, 80, 22, 'DAQ:RUN', size=11, bold=True, align=0)
NOSCAN = ['SCAN:STATE']            # extra pv of the enabled rules: pv1
button(DX + 242, DY + 26, 82, 26, 'Begin', [('DAQ:CMD', 'begin')], bg='green', extra_pvs=NOSCAN,
       extra_exp=' && pvStr1!="running"', vis=(['DAQ:RUNNING'], 'pv0==0'),
       tooltip='begin a run (unlocked; not during a scan: scan_xy.sh / scan_hv.sh drive the DAQ)')
button(DX + 242, DY + 26, 82, 26, 'End', [('DAQ:CMD', 'end')], bg='red', locked_ok=True,
       vis=(['DAQ:RUNNING'], 'pv0==1'), tooltip='end the run (locked too)')
button(DX + 330, DY + 26, 80, 26, 'Open', [('DAQ:CMD', 'open')], bg='head', extra_pvs=NOSCAN + ['DAQ:RUNNING'],
       extra_exp=' && pvStr1!="running" && pv2==0', vis=(['DAQ:LOGGING'], 'pv0==0'),
       tooltip='logging is OFF: runs are not written. Open = daq_open (unlocked, stopped)')
button(DX + 330, DY + 26, 80, 26, 'Close', [('DAQ:CMD', 'close')], bg='head', extra_pvs=NOSCAN + ['DAQ:RUNNING'],
       extra_exp=' && pvStr1!="running" && pv2==0', vis=(['DAQ:LOGGING'], 'pv0==1'),
       tooltip='logging is on: runs are written. Close = daq_close (unlocked, stopped)')
for txt, pv, x, lw, vw in [('events', 'DAQ:EVENTS', 12, 46, 76), ('rate', 'DAQ:RATE', 138, 30, 74),
                           ('avg', 'DAQ:RATE_AVG', 246, 26, 70), ('time', 'DAQ:DURATION', 346, 30, 52)]:
    label(DX + x, DY + 54, lw, 22, txt, size=10, fg='dim')
    update(DX + x + lw, DY + 54, vw, 22, pv, size=11, bold=True, align=0)
update(DX + 12, DY + 78, DW - 24, 20, 'DAQ:FILE', size=9, fmt=6, align=0, alarm=False)
STOPPED = NOSCAN + ['DAQ:RUNNING']
STOPPED_EXP = ' && pvStr1!="running" && pv2==0'
label(DX + 12, DY + 100, 36, 24, 'type', size=10, fg='dim')
widget('combo', DX + 48, DY + 100, 130, 24, pv_name='DAQ:RUNTYPE_SP', items_from_pv='true', enabled='false',
       raw_f=font(11, True), raw_r=rules(enabled_rule(STOPPED, STOPPED_EXP)),
       tooltip='run type (unlocked, stopped): daq_set_runtype')
update(DX + 186, DY + 100, 110, 24, 'DAQ:LOGGING', size=11, align=0, alarm=False)
led(DX + 380, DY + 102, 20, 20, 'DAQ:BUSY', on='orange')
label(DX + 300, DY + 100, 78, 24, 'busy', size=10, fg='dim', align=2)
label(DX + 12, DY + 126, 36, 24, 'setup', size=10, fg='dim')
widget('combo', DX + 48, DY + 126, 130, 24, pv_name='DAQ:SETUP_DET', items_from_pv='true', enabled='false',
       raw_f=font(10), raw_r=rules(enabled_rule(STOPPED, STOPPED_EXP)), tooltip='detector (db/detectors)')
widget('combo', DX + 182, DY + 126, 130, 24, pv_name='DAQ:SETUP_DAQ', items_from_pv='true', enabled='false',
       raw_f=font(10), raw_r=rules(enabled_rule(STOPPED, STOPPED_EXP)), tooltip='DAQ profile (db/daq, V1742)')
button(DX + 318, DY + 126, 92, 24, 'Apply', [('DAQ:CMD', 'apply')], bg='orange', fg='text', size=10,
       extra_pvs=STOPPED, extra_exp=STOPPED_EXP,
       tooltip='testbench.sh use detector / use daq, then daq/setup_rcdaq.sh: reconfigures rcdaq and the '
               'V1742 (unlocked, stopped, ~ some s)')
label(DX + 12, DY + 150, 46, 20, 'active', size=9, fg='dim')
update(DX + 58, DY + 150, 120, 20, 'DAQ:ACTIVE_DET', size=10, bold=True, align=0, alarm=False)
update(DX + 182, DY + 150, 130, 20, 'DAQ:ACTIVE_DAQ', size=10, bold=True, align=0, alarm=False)
update(DX + 12, DY + 172, DW - 24, 20, 'DAQ:CMD_MSG', size=9, fmt=6, align=0, alarm=False,
       tooltip='the last DAQ command from a panel and what came of it (all: logs/daq_commands.log)')
label(DX + 12, DY + 198, 34, 22, 'scan', size=10, fg='dim')
update(DX + 46, DY + 198, 70, 22, 'SCAN:STATE', size=11, bold=True, align=0, alarm=False)
update(DX + 120, DY + 198, DW - 132, 22, 'SCAN:NAME', size=10, fmt=6, align=0, alarm=False)
for txt, pv, x, lw, vw in [('pt', 'SCAN:POINT', 12, 18, 40), ('/', 'SCAN:NPOINTS', 72, 10, 40),
                           ('x', 'SCAN:X', 126, 12, 58), ('y', 'SCAN:Y', 198, 12, 58), ('HV', 'SCAN:HV', 270, 20, 46),
                           ('ETA', 'SCAN:ETA', 338, 0, 0)]:
    if vw:
        label(DX + x, DY + 222, lw, 22, txt, size=9, fg='dim')
        update(DX + x + lw, DY + 222, vw, 22, pv, size=10, bold=True, align=0, units=False)
update(DX + 336, DY + 222, 76, 22, 'SCAN:ETA', size=9, bold=True, align=0, units=False,
       tooltip='estimated end of the scan')

# which readout each V1742 reads (the active detector's cabling.txt, a slot per board's packet): buttons for
# detectors with 2-4 slots (named by SLOT_NAMES in detector.conf); bench_publisher.sh rewrites cabling.txt
# (unlocked, stopped) and the faces. DAQ:CABLED / DAQ:CABLED1: the slot board 0 / board 1 reads now
def det_slots(d):
    dd = os.path.join(os.path.dirname(HERE), 'db/detectors', d)
    slots = sorted(set(int(l.split('#')[0].split()[0]) for l in open(os.path.join(dd, 'mapping.txt'))
                       if len(l.split('#')[0].split()) >= 4))
    names = {}
    for l in open(os.path.join(dd, 'detector.conf')):
        if l.strip().startswith('SLOT_NAMES='):
            for kv in l.split('=', 1)[1].split('#')[0].strip().strip('"').split(';'):
                if '=' in kv:
                    names[int(kv.split('=')[0])] = kv.split('=', 1)[1].strip()
    return [(sl, names.get(sl, 'slot %d' % sl)) for sl in slots]


# one row per V1742: board 0 (packet 2000, s/n 49), board 1 (packet 2001, s/n 51240); "none" = not read (the
# other one alone; the publisher refuses both). The first board read gives the trigger. Adding or dropping a
# board also reruns daq/setup_rcdaq.sh
BOARDS = [(2000, 'DAQ:CABLED', 'board 0', 's/n 49, packet 2000; none = not read, board 1 alone'),
          (2001, 'DAQ:CABLED1', 'board 1', 's/n 51240, packet 2001; none = not read, board 0 alone')]
NONE_W = 36
for b, (pk, pv, lab, about) in enumerate(BOARDS):
    y = DY + 252 + 28 * b
    label(DX + 12, y, 46, 24, lab, size=9, fg='dim', tooltip='V1742 %s' % about)
    update(DX + 58, y, 90, 24, pv, size=10, bold=True, align=0, alarm=False,
           tooltip='the readout (mapping.txt slot) V1742 %s (%s) reads now: the active detector\'s cabling.txt'
                   % (lab, about))
    for d in sorted(x for x in os.listdir(os.path.join(os.path.dirname(HERE), 'db/detectors')) if not x.startswith('_')):
        sl = det_slots(d)
        if not 2 <= len(sl) <= 4:
            continue
        x0, bw = DX + 152, (DW - 164 - NONE_W) // len(sl)
        sl = [('-', 'none')] + sl
        for slot, name in sl:
            w = NONE_W if slot == '-' else bw
            what = ('%s is not read (the other board alone)' % lab if slot == '-' else
                    '%s reads %s (mapping.txt slot %d)' % (lab, name, slot))
            p = dict(raw_a='<actions execute_as_one="true"><action type="write_pv"><pv_name>DAQ:CMD</pv_name>'
                           '<value>cabling %d %s</value><description>%s cabled: %s</description></action></actions>'
                           % (pk, slot, lab, escape(name)),
                     text=name, raw_f=font(9, True), raw_bg=color('background_color', C['unused']),
                     raw_fg=color('foreground_color', C['text']), enabled='false', visible='false',
                     tooltip='%s: V1742 %s from the next run on: rewrites db/detectors/%s/cabling.txt and redraws '
                             'the faces%s (unlocked, rcdaq stopped)'
                             % (d, what, d, '; adding or dropping a board also sets rcdaq up again'),
                     raw_r=rules(enabled_rule(STOPPED, STOPPED_EXP),
                                 rule('vis', 'visible', ['DAQ:ACTIVE_DET'], [('pvStr0=="%s"' % d, 'true')]),
                                 rule('now', 'background_color', [pv],
                                      [('pvStr0=="%s"' % name, colval(C['blue']))]),
                                 rule('nowfg', 'foreground_color', [pv],
                                      [('pvStr0=="%s"' % name, colval(C['white']))])))
            widget('action_button', x0, y, w - 4, 24, version='3.0.0', **p)
            x0 += w

# ---- detector face with the laser (laser_face_small_DET.bob, from make_laser_face.py): the active detector's
# (DAQ:ACTIVE_DET, so it follows the setup picker), or another one picked to look at (FACE_VIEW, this window
# only, changes nothing); a click on a pad of the active detector's face moves the laser there (unlocked); the
# stage box under it ----
AX = 1464
FACE_H = 420
DETS = sorted(d for d in os.listdir(os.path.join(os.path.dirname(HERE), 'db/detectors')) if not d.startswith('_'))
group(AX, SY, W - AX - 6, FACE_H, 'Detector face and laser')
SHOWN = '(pvStr1=="%s" || (pvStr1=="active" && pvStr0=="%s"))'      # pv0 DAQ:ACTIVE_DET, pv1 FACE_VIEW
NOT_ACTIVE = 'pvStr1!="active" && pvStr1!=pvStr0'
label(AX + 178, SY + 3, 92, 22, 'view only', size=10, bold=True, fg='red', align=2, visible='false',
      tooltip='this is not the active detector: its pads cannot be clicked (the active one: DAQ box, setup)',
      raw_r=rules(rule('vis', 'visible', ['DAQ:ACTIVE_DET', FACE_VIEW], [(NOT_ACTIVE, 'true')])))
widget('combo', AX + 274, SY + 3, 150, 22, pv_name=FACE_VIEW, items_from_pv='false', raw_f=font(10),
       raw_i='<items>%s</items>' % ''.join('<item>%s</item>' % escape(i) for i in ['active'] + DETS),
       border_alarm_sensitive='false',
       tooltip='which face to show (this window only; changes nothing). "active" follows the setup picker; '
               'pad clicks work on the active detector\'s face only')
widget('embedded', AX + 4, SY + 24, 422, 392, file='laser_face_small.bob', resize=0,
       raw_n='<transparent>true</transparent>',
       raw_r=rules(rule('det', 'file', ['DAQ:ACTIVE_DET', FACE_VIEW],
                        [(SHOWN % (d, d), 'laser_face_small_%s.bob' % d) for d in DETS])))
# click on a pad: move the laser to its centre (STAGE:* through bench_publisher.sh and motor.sh). Transparent
# buttons over the embedded face, the active detector's only and only while its face is shown, enabled while
# unlocked and no scan, run or move
ACTIVE_DET = [l.split('=', 1)[1].strip() for l in open(os.path.join(os.path.dirname(HERE), 'state/active.conf'))
              if l.startswith('DETECTOR=')][0]
for d in DETS:
    pj = os.path.join(HERE, 'laser_face_pads_%s.json' % d)
    if not os.path.exists(pj):
        continue
    import json
    for pd in json.load(open(pj)):
        acts = ''.join('<action type="write_pv"><pv_name>%s</pv_name><value>%s</value><description>move here</description>'
                       '</action>' % (pv, v) for pv, v in [('STAGE:X_SP', pd['cx']), ('STAGE:Y_SP', pd['cy']),
                                                           ('STAGE:CMD', 'move')])
        widget('action_button', AX + 4 + pd['x'], SY + 24 + pd['y'], pd['w'], pd['h'], version='3.0.0', text='',
               transparent='true', enabled='false', visible='true' if d == ACTIVE_DET else 'false',
               tooltip='%s pad row %d col %d, centre %.2f, %.2f mm: click to move the laser here (unlocked; not '
                       'during a scan, a run or a move)' % (d, pd['row'], pd['col'], pd['cx'], pd['cy']),
               raw_a='<actions execute_as_one="true">%s</actions>' % acts,
               raw_r=rules(enabled_rule(['SCAN:STATE', 'DAQ:RUNNING', 'STAGE:BUSY'], ' && pvStr1!="running" && pv2==0 && pv3==0'),
                           rule('det', 'visible', ['DAQ:ACTIVE_DET', FACE_VIEW],
                                [('pvStr0=="%s" && %s' % (d, SHOWN % (d, d)), 'true'), ('true', 'false')])))
# ---- moving the laser (the XY stage), under the face. The panel writes STAGE:CMD / STAGE:X_SP / Y_SP /
# STEP; bin/bench_publisher.sh runs bin/motor.sh (limits, arrival) and refuses during a scan or a run.
# STOP writes the controller's stop PVs directly and works locked too. The position is on the face. ----
SGX, SGW = AX, W - AX - 6
SGY = SY + FACE_H + 4
SG_H = SY + LOW_H - SGY
group(SGX, SGY, SGW, SG_H, '')
label(SGX + 10, SGY + 2, 200, 18, 'Move the laser (XY stage)', size=11, bold=True, fg='head')
MOVE_OK = ['SCAN:STATE', 'DAQ:RUNNING', 'STAGE:BUSY']
MOVE_EXP = ' && pvStr1!="running" && pv2==0 && pv3==0'
label(SGX + 10, SGY + 22, 12, 24, 'x', size=10, fg='dim')
entry(SGX + 22, SGY + 22, 56, 24, 'STAGE:X_SP', prec=2)
label(SGX + 82, SGY + 22, 12, 24, 'y', size=10, fg='dim')
entry(SGX + 94, SGY + 22, 56, 24, 'STAGE:Y_SP', prec=2)
button(SGX + 154, SGY + 22, 56, 24, 'Move', [('STAGE:CMD', 'move')], bg='blue', size=10, extra_pvs=MOVE_OK,
       extra_exp=MOVE_EXP, tooltip='move to x, y [mm] (unlocked; not during a scan or a run)')
button(SGX + 214, SGY + 22, 50, 24, 'Home', [('STAGE:CMD', 'home')], bg='orange', fg='text', size=10,
       extra_pvs=MOVE_OK, extra_exp=MOVE_EXP, tooltip='drive both axes to their home switches at 0 mm (unlocked)')
update(SGX + 268, SGY + 20, SGW - 276, 28, 'STAGE:MSG', size=8, fmt=6, align=0, alarm=False, wrap_words='true',
       tooltip='the last stage command and what came of it (all: logs/daq_commands.log)')
label(SGX + 10, SGY + 50, 30, 24, 'step', size=10, fg='dim')
entry(SGX + 40, SGY + 50, 44, 24, 'STAGE:STEP', prec=2)
for k, (txt, cmd) in enumerate([('X\u2212', 'x-'), ('X+', 'x+'), ('Y\u2212', 'y-'), ('Y+', 'y+')]):
    button(SGX + 90 + k * 40, SGY + 50, 38, 24, txt, [('STAGE:CMD', cmd)],
           bg='head', size=10, extra_pvs=MOVE_OK, extra_exp=MOVE_EXP, tooltip='jog by the step (unlocked)')
led(SGX + 256, SGY + 52, 20, 20, 'STAGE:BUSY', on='orange')
button(SGX + 284, SGY + 50, SGW - 292, 24, 'STOP', [('THOR_X:stop', 1), ('THOR_Y:stop', 1), ('STAGE:STOP', 1)],
       bg='alloff', locked_ok=True, size=11, tooltip='stop both axes now (locked too)')


# ---- lock and controls (compact, left) and the HV alarm log (right) ----
KX, KY, KW = 612, SY + 340, 846
KH = H - KY - 6
group(KX, KY, KW, KH, 'Controls')
CW = 330                       # width of the controls column
widget('rectangle', KX + 10, KY + 26, CW, 36, raw_bg=color('background_color', C['lock']), line_width=0,
       corner_width=6, corner_height=6, name='lock_box',
       raw_r=rules(rule('unl', 'background_color', [UNLOCK], [('pv0==1', colval(C['unlocked']))])),
       raw_s='<scripts>' + relock_scripts() + '</scripts>')
label(KX + 18, KY + 30, 130, 28, 'LOCKED', size=14, bold=True, fg='white', name='locked_label',
      raw_r=rules(rule('vis', 'visible', [UNLOCK], [('pv0==1', 'false')])),
      tooltip='Locked: only ALL HV OFF, laser emission OFF, stage STOP, DAQ End and mute work. Unlock for '
              'setpoints, channel OFF/ON, PDWN, laser frequency/tune, the arms, the DAQ and the stage. ON also '
              'needs its box-safe arm (10 s). Relocks after 5 min without a write.')
label(KX + 18, KY + 30, 130, 28, 'UNLOCKED', size=14, bold=True, fg='text', name='unlocked_label', visible='false',
      raw_r=rules(rule('vis', 'visible', [UNLOCK], [('pv0==1', 'true')])))
label(KX + 130, KY + 30, 70, 28, 'relock in', size=9, fg='text', visible='false', align=2,
      raw_r=rules(rule('vis', 'visible', [UNLOCK], [('pv0==1', 'true')])))
update(KX + 204, KY + 30, 40, 28, RELOCK_LEFT, size=10, bold=True, fmt=1, prec=0, align=0, alarm=False, units=False,
       transparent='true', visible='false', raw_r=rules(rule('vis', 'visible', [UNLOCK], [('pv0==1', 'true')])))
widget('action_button', KX + CW - 86, KY + 31, 90, 26, version='3.0.0', text='unlock', raw_f=font(11, True),
       raw_a='<actions execute_as_one="true"><action type="write_pv"><pv_name>%s</pv_name><value>1</value>'
             '<description>unlock</description></action></actions>' % escape(UNLOCK),
       raw_r=rules(rule('vis', 'visible', [UNLOCK], [('pv0==1', 'false')])))
widget('action_button', KX + CW - 86, KY + 31, 90, 26, version='3.0.0', text='lock', raw_f=font(11, True),
       raw_a='<actions execute_as_one="true"><action type="write_pv"><pv_name>%s</pv_name><value>0</value>'
             '<description>lock</description></action></actions>' % escape(UNLOCK),
       visible='false', raw_r=rules(rule('vis', 'visible', [UNLOCK], [('pv0==1', 'true')])))
button(KX + 10, KY + 66, CW, 42, 'ALL HV OFF', [('N1470_A:ALL:OFF', 1), ('N1470_B:ALL:OFF', 1)], bg='alloff',
       locked_ok=True, size=18, tooltip='both units, all channels off: one click, locked too')
button(KX + 10, KY + 112, 160, 24, 'arm HV box-safe 10 s', [('N1470:BOX_SAFE', 1)], bg='orange', fg='text', size=10,
       tooltip='the dark box is closed and the detector is safe to power (unlocked)')
update(KX + 10, KY + 138, 160, 18, 'N1470:ARM_LEFT', size=10, bold=True, align=1, alarm=False,
       visible='false', raw_r=rules(rule('vis', 'visible', ['N1470:BOX_SAFE'], [('pv0==1', 'true')])))
button(KX + 176, KY + 112, CW - 166, 24, 'mute alarm sound', [('N1470:MUTE', 1)], bg='head', locked_ok=True, size=10,
       tooltip='acknowledge: the sound stops until the alarm clears (pop-ups and this panel stay)')
update(KX + 176, KY + 138, CW - 166, 18, 'N1470:MUTE', size=9, align=1, alarm=False)
# the HV alarm log (newest first: the annunciator writes the lines in that order)
LX0 = KX + CW + 20
label(LX0, KY + 4, 220, 20, 'HV alarm log (newest first)', size=12, bold=True, fg='head')
# the alarm sound (any time, locked too: it switches nothing): a pick plays once, "test" plays it again
label(KX + KW - 274, KY + 4, 44, 22, 'sound', size=10, fg='dim', align=2)
widget('combo', KX + KW - 226, KY + 3, 150, 22, pv_name='N1470:SOUND', items_from_pv='true', raw_f=font(10),
       tooltip='the alarm sound (bin/alarm_sounds.list); a pick plays once and is kept (state/alarm_sound)')
button(KX + KW - 70, KY + 3, 62, 22, 'test', [('N1470:SOUND_TEST', 1)], bg='head', locked_ok=True, size=10,
       tooltip='play the alarm sound once (muted or not)')
widget('textupdate', LX0, KY + 28, KX + KW - LX0 - 8, KH - 34, pv_name='N1470:ALARM_HIST', format=6, raw_f=font(9),
       horizontal_alignment=0, vertical_alignment=0, wrap_words='true', border_alarm_sensitive='false',
       raw_bg=color('background_color', C['white']))

xml = ('<?xml version="1.0" encoding="UTF-8"?>\n<display version="2.0.0"><name>417 bench</name>'
       '<width>%d</width><height>%d</height>%s%s</display>\n'
       % (W, H, color('background_color', C['bg']), '\n'.join(out)))
with open(os.path.join(HERE, 'bench.bob'), 'w') as f:
    f.write(xml)
print('bench.bob: %d widgets' % n_widgets[0])
if len(sys.argv) == 3 and sys.argv[1] == '--unlocked-copy':
    with open(sys.argv[2], 'w') as f:
        f.write(xml.replace(escape(UNLOCK), escape(UNLOCK.replace('(0)', '(1)'))).replace(UNLOCK, UNLOCK.replace('(0)', '(1)')))
