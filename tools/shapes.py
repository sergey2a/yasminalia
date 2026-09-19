#!/usr/bin/env python3
"""Контуры лица Ясмины: экспорт в SVG для правки в Figma/Inkscape и импорт обратно.

    tools/shapes.py export [каталог]     # -> shapes_<состояние>.svg
    tools/shapes.py import face.svg happy

Рисуется и правится ПРАВАЯ половина лица — левая зеркалится прошивкой.
В SVG редактируемые контуры помечены id eye / brow / mouth (и mouth_open в
состоянии speak); всё остальное — подсказка и при импорте игнорируется.

Требование к контуру: замкнутый путь ровно из 6 кубических сегментов.
Число и порядок точек одинаковы во всех состояниях — на этом держится морфинг.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src', 'shapes.cpp')
STATES = ['idle', 'listen', 'speak', 'happy',
          'love', 'wink', 'surprise', 'sleep', 'sad']
FEATURES = ['eye', 'brow', 'mouth', 'decor']       # у чего есть свой якорь
PATHS = ['eye', 'brow', 'mouth', 'mouth_talk', 'decor']
ANCHORED = ['eye', 'brow', 'mouth', 'decor']
ANCHOR_OF = {'eye': 'eye', 'brow': 'brow', 'mouth': 'mouth',
             'mouth_talk': 'mouth', 'decor': 'decor'}
SEGS, PTS = 6, 18

TATAR = {'idle': 'тынычлык', 'listen': 'тыңлыйм', 'speak': 'сөйлим',
         'happy': 'шат', 'love': 'гашыйк', 'wink': 'шаян',
         'surprise': 'гаҗәп', 'sleep': 'йокы', 'sad': 'моң'}
NOTE = {
    'idle':     'Спокойный открытый глаз: мягкий овал, чуть шире снизу.',
    'listen':   'Глаз распахнут и выше, бровь поднята, рот маленький.',
    'speak':    'Лёгкий прищур: тот же контур, вершина ниже.',
    'happy':    'Прищур-полумесяц: n4 поднят выше углов n3 и n5.',
    'love':     'Сердечки вместо глаз, розовая палитра, сердечки по краям.',
    'wink':     'Один глаз прикрыт (wink = 1), рядом искорки.',
    'surprise': 'Круглые глаза, брови высоко, рот овалом.',
    'sleep':    'Глаза сомкнуты в полоски, месяц над ними, холодный синий.',
    'sad':      'Брови домиком, рот углами вниз, слезинка под глазом.',
}


# --------------------------------------------------------------------------
def parse(path=SRC):
    text = open(path, encoding='utf-8').read()
    body = re.sub(r'//[^\n]*', '', text)
    nums = [float(x) for x in re.findall(r'(-?\d+\.?\d*)f', body[body.index('LOOK['):])]

    per = 2 + PTS*2 + 2 + PTS*2 + 2 + PTS*2 + PTS*2 + 2 + PTS*2 + 3 + 9 + 3
    if len(nums) != per * len(STATES):
        sys.exit(f'ожидалось {per*len(STATES)} чисел в LOOK, найдено {len(nums)}')

    data = {'states': {}}
    for i, st in enumerate(STATES):
        n, k = nums[i*per:(i+1)*per], 0
        d = {'anchor': {}, 'path': {}}
        for f in PATHS:
            if f in ANCHORED:
                d['anchor'][f] = (n[k], n[k+1]); k += 2
            d['path'][f] = [(n[k+2*j], n[k+2*j+1]) for j in range(PTS)]
            k += PTS*2
        d['decorOn'], d['decorMirror'], d['wink'] = n[k], n[k+1], n[k+2]; k += 3
        d['ink'] = tuple(n[k:k+3]); k += 3
        d['decorInk'] = tuple(n[k:k+3]); k += 3
        d['glowInk'] = tuple(n[k:k+3]); k += 3
        d['params'] = tuple(n[k:k+3])
        data['states'][st] = d
    return data


def fmt(v):
    if abs(v) < 0.005:        # убираем «-0.00» от вычитания якоря
        v = 0.0
    return f'{v:7.2f}f'


def emit_path(pts, indent, tail=','):
    rows = []
    labels = ['n5 -> n0', 'n0 -> n1  вершина', 'n1 -> n2',
              'n2 -> n3', 'n3 -> n4  низ', 'n4 -> n5']
    for s in range(SEGS):
        trio = ', '.join('{' + fmt(x) + ',' + fmt(y) + '}' for x, y in pts[s*3:s*3+3])
        end = ',' if s < SEGS - 1 else ' }}' + tail
        head = '{{ ' if s == 0 else '   '
        rows.append(f'{indent}{head}{trio}{end}   // {labels[s]}')
    return '\n'.join(rows)


def write(data, path=SRC):
    out = ['#include "shapes.h"', '',
           '// Контуры лица. Координаты — пиксели панели относительно якоря черты,',
           '// цвета — 0..255. Правишь здесь или через tools/shapes.py; результат',
           '// видно сразу в tools/preview/build.sh.', '',
           'namespace face {', '',
           'const Look LOOK[FACE_STATE_COUNT] = {', '']
    for st in STATES:
        s = data['states'][st]
        bar = '=' * max(4, 56 - len(st) - len(TATAR[st]))
        out.append(f'  // ============== {st.upper()} — {TATAR[st]} {bar}')
        out.append(f'  // {NOTE[st]}')
        first = True
        for f in PATHS:
            if f in ANCHORED:
                ax, ay = s['anchor'][f]
                out.append(f'{"  { " if first else "    "}{{ {ax:.2f}f, {ay:.2f}f }},')
                first = False
            out.append(f'    // >>> {f}_{st}')
            out.append(emit_path(s['path'][f], '    '))
            out.append(f'    // <<< {f}_{st}')
        out.append(f'    {s["decorOn"]:.2f}f, {s["decorMirror"]:.2f}f, {s["wink"]:.2f}f,'
                   '             // украшение: видно, зеркалить; подмигивание')
        for key, note in (('ink', 'цвет черт лица'), ('decorInk', 'цвет украшения'),
                          ('glowInk', 'цвет ореола')):
            r, g, b = s[key]
            out.append(f'    {{ {r:6.1f}f, {g:6.1f}f, {b:6.1f}f }},           // {note}')
        g, gz, bl = s['params']
        out.append(f'    {g:.2f}f, {gz:.2f}f, {bl:.2f}f }},'
                   '                    // ореол, взгляд, моргание')
        out.append('')
    out += ['};', '', '} // namespace face', '']
    open(path, 'w', encoding='utf-8').write('\n'.join(out))


# --------------------------------------------------------------------------
def to_abs(pts, anchor, mirror=False):
    ax, ay = anchor
    sx = -1.0 if mirror else 1.0
    cx = 120.0
    return [(cx + sx * (ax + x), ay + y) for x, y in pts]


def d_attr(abs_pts):
    d = f'M {abs_pts[-1][0]:.2f} {abs_pts[-1][1]:.2f}'
    for s in range(SEGS):
        (x1, y1), (x2, y2), (x3, y3) = abs_pts[s*3:s*3+3]
        d += f' C {x1:.2f} {y1:.2f} {x2:.2f} {y2:.2f} {x3:.2f} {y3:.2f}'
    return d + ' Z'


def export(outdir):
    data = parse()
    os.makedirs(outdir, exist_ok=True)
    for st in STATES:
        s = data['states'][st]
        parts = [
            '<svg xmlns="http://www.w3.org/2000/svg" width="240" height="240" '
            'viewBox="0 0 240 240">',
            f'  <title>Ясмина — {st}</title>',
            '  <rect width="240" height="240" fill="#000"/>',
            '  <circle cx="120" cy="120" r="120" fill="#0a0806"/>',
            '  <!-- Подсказка: левая половина лица. Прошивка зеркалит её сама,',
            '       правь только контуры с id eye / brow / mouth. -->',
            '  <g id="guide" opacity="0.30">',
        ]
        for f in ('eye', 'brow', 'decor'):
            parts.append(f'    <path d="{d_attr(to_abs(s["path"][f], s["anchor"][f], True))}"'
                         ' fill="#fee1a4"/>')
        parts.append('  </g>')
        for f in FEATURES:
            op = '' if (f != 'decor' or s['decorOn'] > 0.5) else ' opacity="0.25"'
            parts.append(f'  <path id="{f}"{op} '
                         f'd="{d_attr(to_abs(s["path"][f], s["anchor"][f]))}"'
                         ' fill="#fee1a4"/>')
        parts.append('  <!-- раскрытый рот этой эмоции: показан полупрозрачным -->')
        parts.append(f'  <path id="mouth_talk" opacity="0.45" '
                     f'd="{d_attr(to_abs(s["path"]["mouth_talk"], s["anchor"]["mouth"]))}"'
                     ' fill="#fee1a4"/>')
        parts.append('</svg>')
        name = os.path.join(outdir, f'shapes_{st}.svg')
        open(name, 'w', encoding='utf-8').write('\n'.join(parts) + '\n')
        print('записан', name)


# --------------------------------------------------------------------------
TOKEN = re.compile(r'[MmCcSsLlZz]|-?\d*\.?\d+(?:[eE][-+]?\d+)?')


def parse_d(d):
    """Путь -> список из 18 абсолютных точек (по 3 на сегмент)."""
    toks = TOKEN.findall(d)
    i, cmd = 0, None
    cur = start = (0.0, 0.0)
    prev_c2 = None
    out = []

    def num():
        nonlocal i
        v = float(toks[i]); i += 1
        return v

    while i < len(toks):
        if toks[i].isalpha():
            cmd = toks[i]; i += 1
            if cmd in 'Zz':
                continue
        rel = cmd.islower()
        bx, by = cur if rel else (0.0, 0.0)
        if cmd in 'Mm':
            cur = start = (num() + bx, num() + by)
            cmd = 'L' if cmd == 'M' else 'l'
            prev_c2 = None
        elif cmd in 'Ll':
            x, y = num() + bx, num() + by
            out += [(cur[0] + (x-cur[0])/3, cur[1] + (y-cur[1])/3),
                    (cur[0] + 2*(x-cur[0])/3, cur[1] + 2*(y-cur[1])/3), (x, y)]
            cur, prev_c2 = (x, y), None
        elif cmd in 'Cc':
            c1 = (num() + bx, num() + by)
            c2 = (num() + bx, num() + by)
            p = (num() + bx, num() + by)
            out += [c1, c2, p]
            cur, prev_c2 = p, c2
        elif cmd in 'Ss':
            c2 = (num() + bx, num() + by)
            p = (num() + bx, num() + by)
            c1 = (2*cur[0] - prev_c2[0], 2*cur[1] - prev_c2[1]) if prev_c2 else cur
            out += [c1, c2, p]
            cur, prev_c2 = p, c2
        else:
            sys.exit(f'команда "{cmd}" в пути не поддерживается — сохрани кривые как C')
    # замыкающий сегмент до начальной точки, если редактор его не записал
    if len(out) == PTS - 3 and out[-1] != start:
        out += [out[-1], start, start]
    return out


def import_svg(svg_path, state):
    if state not in STATES:
        sys.exit(f'состояние должно быть одним из {STATES}')
    text = open(svg_path, encoding='utf-8').read()
    data = parse()
    s = data['states'][state]
    found = []
    for pid, d in re.findall(r'<path[^>]*\bid="([^"]+)"[^>]*\bd="([^"]+)"', text):
        if pid not in PATHS:
            continue
        pts = parse_d(d)
        if len(pts) != PTS:
            sys.exit(f'{pid}: нужно ровно {SEGS} кубических сегментов, '
                     f'найдено {len(pts)//3}')
        ax, ay = s['anchor'][ANCHOR_OF[pid]]
        s['path'][pid] = [(x - 120.0 - ax, y - ay) for x, y in pts]
        found.append(pid)
    if not found:
        sys.exit('в файле нет путей с id eye / brow / mouth')
    write(data)
    print(f'{state}: обновлены {", ".join(found)} -> src/shapes.cpp')


if __name__ == '__main__':
    if len(sys.argv) >= 2 and sys.argv[1] == 'export':
        export(sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'preview-out'))
    elif len(sys.argv) == 4 and sys.argv[1] == 'import':
        import_svg(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 2 and sys.argv[1] == 'normalize':
        write(parse()); print('src/shapes.cpp приведён к каноническому виду')
    else:
        print(__doc__)
