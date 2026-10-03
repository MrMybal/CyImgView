# Vérification des fichiers de langue (src/lang/<groupe>.<code>.lang) :
#  - chaque langue d'un groupe a exactement les clés du français, avec les mêmes spécificateurs printf
#    (même ordre) et le même nombre de choix pour les listes (clés « .choices », séparateur « | ») ;
#  - @code correspond au nom du fichier, @name est présent ;
#  - les clés citées dans le code (src, plugins) existent en français.
#     python tools\check_lang.py          (code de sortie 1 s'il y a un problème)
import collections
import glob
import os
import re
import sys

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG = os.path.join(ROOT, 'src', 'lang')
SPEC = re.compile(r'%[-+ #0-9.]*(?:ls|l?[dusfxX]|lld|llu|zu|%)')

groups = collections.defaultdict(dict)  # groupe -> code -> {clé: valeur}
problems = []
for path in sorted(glob.glob(os.path.join(LANG, '*.lang'))):
    name = os.path.basename(path)
    parts = name.split('.')
    if len(parts) != 3:
        problems.append(f'{name} : nom inattendu (attendu <groupe>.<code>.lang)')
        continue
    group, code = parts[0], parts[1]
    table, header = {}, {}
    for n, line in enumerate(open(path, encoding='utf-8-sig'), 1):
        line = line.rstrip('\r\n')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        key, sep, value = line.partition('=')
        if not sep:
            problems.append(f'{name}:{n} : ligne sans « = »')
            continue
        key, value = key.strip(), value.strip()
        if key.startswith('@'):
            header[key] = value
        elif key in table:
            problems.append(f'{name}:{n} : clé en double {key}')
        else:
            table[key] = value
    if header.get('@code') != code:
        problems.append(f'{name} : @code = {header.get("@code")} (attendu {code})')
    if not header.get('@name'):
        problems.append(f'{name} : @name manquant')
    groups[group][code] = table

all_fr = {}
for group, langs in sorted(groups.items()):
    fr = langs.get('fr')
    if fr is None:
        problems.append(f'groupe {group} : pas de fichier français')
        continue
    all_fr.update(fr)
    for code, table in sorted(langs.items()):
        if code == 'fr':
            continue
        missing = sorted(set(fr) - set(table))
        extra = sorted(set(table) - set(fr))
        if missing:
            problems.append(f'{group}.{code} : clés manquantes {missing}')
        if extra:
            problems.append(f'{group}.{code} : clés en trop {extra}')
        for key in sorted(set(fr) & set(table)):
            if SPEC.findall(fr[key]) != SPEC.findall(table[key]):
                problems.append(f'{group}.{code} : spécificateurs différents pour {key}')
            if key.endswith('.choices') and fr[key].count('|') != table[key].count('|'):
                problems.append(f'{group}.{code} : nombre de choix différent pour {key}')

# Clés citées dans le code
LITERAL = re.compile(r'"((?:editapi|edit|develop|openwith|viewer|plugin|channels|filter|slide|save|menu|decoder)\.[a-z0-9_.]+)"')
sources = glob.glob(os.path.join(ROOT, 'src', '*.cpp')) + glob.glob(os.path.join(ROOT, 'src', '*.h'))
sources += glob.glob(os.path.join(ROOT, 'plugins', '*', '*.cpp'))
for path in sources:
    text = ''.join(l for l in open(path, encoding='utf-8', errors='replace') if not l.lstrip().startswith('#include'))
    for key in sorted(set(LITERAL.findall(text))):
        if key.endswith('.'):
            continue  # préfixe complété dans le code
        if key.startswith('plugin.') and key.split('.')[1] not in ('fx_extra', 'annotate', 'bw_mode', 'share_actions'):
            continue  # plugins de décodage : textes par défaut dans le plugin
        if key not in all_fr:
            problems.append(f'{os.path.relpath(path, ROOT)} : clé inconnue {key}')

for p in problems:
    print(p)
print(f'{len(groups)} groupes, {sum(len(l) for l in groups.values())} fichiers, {len(problems)} problème(s)')
sys.exit(1 if problems else 0)
