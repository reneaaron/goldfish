#!/usr/bin/env python3
"""Build the Goldfish weekly review page from data.json.

    python3 build.py data.json weekly-review.html

template.html carries the chrome (site tokens, nav, the goldfish and dove
lockups). This fills its slots. Structured fields are escaped; every *_html
field is passed through, with bare #123 turned into a GitHub link. The build
refuses an em dash, an image from an external host, or a tile with no prior.
"""
import base64, html, json, os, re, subprocess, sys, tempfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
GH = "https://github.com/reneaaron/goldfish"
esc = html.escape


def linkify(s):
    """#123 -> link, except inside an existing <a>...</a>. /issues/ redirects to a PR."""
    parts = re.split(r'(<a\b.*?</a>)', s, flags=re.S)
    return ''.join(p if p.startswith('<a') else
                   re.sub(r'(?<![\w&/])#(\d+)\b', rf'<a href="{GH}/issues/\1">#\1</a>', p)
                   for p in parts)


def refs(ns):
    return ' '.join(f'<a href="{GH}/pull/{n}">#{n}</a>' for n in ns)


def data_uri(raw, mime):
    return f"data:{mime};base64," + base64.b64encode(raw).decode()


def to_webp(path, vf=None):
    """Re-encode an image as WebP through ffmpeg (no ImageMagick on this machine)."""
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, 'o.webp')
        cmd = ['ffmpeg', '-loglevel', 'error', '-y', '-i', path]
        if vf:
            cmd += ['-vf', vf]
        subprocess.run(cmd + ['-q:v', '75', out], check=True)
        return open(out, 'rb').read()


_avatars = {}
def avatar(login, url):
    if login not in _avatars:
        if not url:
            _avatars[login] = ''
        else:
            with tempfile.TemporaryDirectory() as d:
                src = os.path.join(d, 'a')
                with urllib.request.urlopen(url, timeout=20) as r, open(src, 'wb') as f:
                    f.write(r.read())
                _avatars[login] = data_uri(to_webp(src, 'scale=72:72'), 'image/webp')
    return _avatars[login]


def main(data_path, out_path):
    d = json.load(open(data_path))
    base = os.path.dirname(os.path.abspath(data_path))
    av = {c['login']: c.get('avatar_url', '') for c in d['contributors']}
    img = lambda login: (f'<img src="{avatar(login, av.get(login, ""))}" alt="">'
                         if av.get(login) else '<span></span>')

    def row(r, draft=False):
        tag = '<span class="tag draft">Draft</span>' if draft else ''
        return (f'<div class="row">{img(r["login"])}<div>{tag}{linkify(r["text_html"])}</div>'
                f'<div class="ref">{refs(r.get("refs", []))}</div></div>')

    tiles = ''
    for t in d['tiles']:
        if 'prior' not in t or str(t['prior']).strip() == '':
            sys.exit(f'tile "{t["label"]}" has no prior value; say why it has none instead')
        if t['tone'] not in ('good', 'bad', 'flat'):
            sys.exit(f'tile "{t["label"]}": tone must be good, bad or flat')
        tiles += (f'<div class="tile"><span class="lab">{esc(t["label"])}</span><span class="fig">{esc(str(t["figure"]))}</span>'
                  f'<span class="d {t["tone"]}">{esc(t["delta"])}</span><span class="pr">{esc(str(t["prior"]))}</span></div>')

    wins = ''.join(f'<div class="winc"><span class="fig">{esc(w["figure"])}<small>{esc(w.get("unit", ""))}</small></span>'
                   f'<h4>{esc(w["head"])}</h4><p>{linkify(w["body_html"])}</p></div>' for w in d['wins'])
    lede = ''.join(f'<p>{linkify(p)}</p>' for p in d['lede_html'])
    for p in d['lede_html']:
        if len(re.sub('<[^>]+>', '', p).split()) > 70:
            sys.exit('a lede paragraph is over 70 words; split it')

    areas = ''.join(
        f'<div class="area"><div class="ah"><h4>{esc(a["name"])}</h4><span class="cnt">{a["count"]} PRs</span></div>'
        f'<p>{linkify(a["body_html"])}</p><p class="next"><span class="tag {"att" if a.get("needs_review") else "draft"}">'
        f'{"Needs review" if a.get("needs_review") else "Next"}</span>{linkify(a["next_html"])}</p></div>' for a in d['areas'])

    contrib = ''.join(f'<div>{img(c["login"])}{esc(c["login"])} <b>{c["count"]}</b></div>' for c in d['contributors'])
    shipped = ''.join(f'<h3>{esc(g["group"])}</h3>' + ''.join(row(r) for r in g['rows']) for g in d['shipped'])
    inflight = ''.join(row(r, r.get('draft', False)) for r in d['inflight'])

    findings = ''
    for i, f in enumerate(d['findings'], 1):
        att = '<span class="tag att">Attention</span>' if f.get('attention') else ''
        findings += (f'<div class="finding{" hot" if f.get("attention") else ""}"><div class="num">{i}</div><div>'
                     f'<h4>{att}{esc(f["head"])}</h4><div class="meas">{esc(f["measure"])}</div>'
                     f'<span class="lbl">Cause</span><p>{linkify(f["cause_html"])}</p>'
                     f'<span class="lbl">So what</span><p>{linkify(f["so_what_html"])}</p></div></div>')
    if len(d['findings']) > 6:
        sys.exit('more than six findings; merge related ones')

    tables = ''
    for t in d['tables']:
        n = [c.endswith('#') for c in t['columns']]   # a column named "Prior#" is numeric: right-aligned
        cls = lambda num: ' class="n"' if num else ''
        head = ''.join(f'<th{cls(num)}>{esc(c.rstrip("#"))}</th>' for c, num in zip(t['columns'], n))
        body = ''.join('<tr>' + ''.join(f'<td{cls(num)}>{linkify(str(v))}</td>'
                                         for v, num in zip(r, n)) + '</tr>' for r in t['rows'])
        note = f'<p class="tnote">{linkify(t["note_html"])}</p>' if t.get('note_html') else ''
        tables += f'<h3>{esc(t["title"])}</h3><div class="tw"><table><tr>{head}</tr>{body}</table></div>{note}'

    do = ''.join(f'<li><b>{esc(x["head"])}</b>{linkify(x["body_html"])}</li>' for x in d['do_first'])
    wi = ''.join(f'<div><h4>{esc(x["head"])}</h4><p>{linkify(x["body_html"])}</p><p class="test">{linkify(x["test_html"])}</p></div>'
                 for x in d['what_if'])

    photo = ''
    if d.get('header_image'):
        p = os.path.join(base, d['header_image']) if not os.path.isabs(d['header_image']) else d['header_image']
        raw = open(p, 'rb').read() if p.endswith('.webp') else to_webp(p, d.get('header_crop'))
        note = f'<span class="note" aria-hidden="true">{esc(d["header_note"])}</span>' if d.get('header_note') else ''
        photo = (f'<div class="photo"><img src="{data_uri(raw, "image/webp")}" alt="{esc(d["header_image_alt"])}">{note}</div>')

    s = open(os.path.join(HERE, 'template.html')).read()
    slots = {
        'END_DATE': esc(d['end_date']), 'WINDOW_HTML': d['window_html'], 'SUB_HTML': linkify(d['sub_html']),
        'PHOTO': photo, 'TILES': tiles, 'WINS': wins, 'LEDE': lede, 'AREAS_INTRO': linkify(d['areas_intro_html']),
        'AREAS': areas, 'CONTRIBUTORS': contrib, 'SHIPPED': shipped, 'INFLIGHT_INTRO': linkify(d['inflight_intro_html']),
        'INFLIGHT': inflight, 'FINDINGS': findings, 'TABLES': tables, 'DO_FIRST_INTRO': linkify(d['do_first_intro_html']),
        'DO_FIRST': do, 'WHAT_IF': wi, 'FOOTER': linkify(d['footer_html']),
    }
    for k, v in slots.items():
        s = s.replace('{{' + k + '}}', v)

    problems = []
    if '{{' in s:
        problems.append('unfilled slot: ' + re.search(r'\{\{\w+\}\}', s).group(0))
    if '—' in s:
        problems.append('em dash on the page')
    if re.search(r'<img[^>]+src="https?:', s):
        problems.append('an image loads from an external host; embed it')
    if problems:
        sys.exit('build refused:\n  ' + '\n  '.join(problems))
    open(out_path, 'w').write(s)
    print(f'wrote {out_path} ({len(s) // 1024} KB)')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
