import json, re, shutil, sys

# usage: splice_round.py <sections.json> <round> <commit> <backup-suffix>
sec_file, rnd, commit, bak = sys.argv[1:5]
shutil.copy('sr2-test-run.html', f'sr2-test-run.{bak}.bak.html')
html = open('sr2-test-run.html', encoding='utf8').read()
s = html.index('const SECTIONS = [')
e = html.index('\n];', s) + 3
old = html[s:e]
cases = {}
# A case is one line: ["id", "title", "how" | ["step", ...]],
for m in re.finditer(r'^    (\["([a-z0-9]+)", .*\]),?$', old, re.M):
    cases[m.group(2)] = m.group(1)
secs = json.load(open(sec_file, encoding='utf8'))
out = ['const SECTIONS = [']
missing = []
for name, items in secs:
    # "Group / Name" puts the section under a group fold (e.g. "Skipped / Sandbox").
    grp, _, sub = name.partition(' / ')
    if sub:
        out.append(f'  {{ name: {json.dumps(sub)}, group: {json.dumps(grp)}, cases: [')
    else:
        out.append(f'  {{ name: {json.dumps(name)}, cases: [')
    for it in items:
        if isinstance(it, str):
            if it not in cases:
                missing.append(it)
                continue
            out.append('    ' + cases[it] + ',')
        else:
            out.append('    ' + json.dumps(it, ensure_ascii=False) + ',')
    out.append('  ]},')
out.append('];')
html = html[:s] + '\n'.join(out) + html[e:]
html = re.sub(r'Round \d+: new and carried-over cases\. Use the build whose Insert menu footer shows commit <code>[0-9a-f]+</code>',
              f'Round {rnd}: new and carried-over cases. Use the build whose Insert menu footer shows commit <code>{commit}</code>', html)
open('sr2-test-run.html', 'w', encoding='utf8').write(html)
print('round', rnd in html, 'cases', sum(len(i) for _, i in secs), 'missing', missing)
