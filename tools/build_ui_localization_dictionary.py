"""Compile authored native UI phrases into a bounded, exact-match C++ catalog."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
FORMAT = re.compile(r'%(?:[-+ #0]*\d*(?:\.\d+)?(?:I64|hh|ll|[hlztL])?[diuoxXfFeEgGaAcspn%])')


def load(paths):
    values, system = {}, {}
    for path in paths:
        document = json.loads(Path(path).read_text(encoding='utf-8'))
        if document.get('schema_version') != 1 or not isinstance(document.get('entries'), list):
            raise ValueError('Unsupported UI phrase catalog: '+str(path))
        seen = set()
        for entry in document['entries']:
            source, target = entry.get('source'), entry.get('target')
            status = entry.get('status')
            unique=(entry.get('id'),source) if document.get('language')=='system' else source
            if (not isinstance(source,str) or not source or len(source)>1024 or
                    not isinstance(target,str) or not target or len(target)>1024 or
                    '\0' in source or '\0' in target or unique in seen or
                    status not in {'ready','keep','review'}):
                raise ValueError('Invalid or duplicate UI phrase in '+str(path))
            seen.add(unique)
            if FORMAT.findall(source) != FORMAT.findall(target):
                raise ValueError('UI phrase format arguments changed: '+repr(source))
            if status != 'ready':
                continue
            if document.get('language') == 'system':
                identity=entry.get('id')
                if (type(identity) is not int or not 0<identity<2**32 or
                        len(target.encode('utf-16-le'))//2>99 or identity in system):
                    raise ValueError('Invalid native UI text identity or extent')
                system[identity]=(source,target)
                continue
            if source in values and values[source] != target:
                raise ValueError('Conflicting UI translation: '+repr(source))
            values[source] = target
    return sorted(values.items()), sorted(system.items())


def render(values):
    phrases,system=values
    rows=['// Generated from authored UI phrases; edit translations/*.json and regenerate.',
          '#pragma once', '#include <cstddef>', '', 'namespace sdgo::ui_localization {',
          'struct Phrase { const wchar_t* source; const wchar_t* target; };',
          'inline constexpr Phrase kPhrases[] = {']
    for source,target in phrases:
        rows.append('    {L'+json.dumps(source,ensure_ascii=True)+', L'+json.dumps(target,ensure_ascii=True)+'},')
    rows += ['};','inline constexpr std::size_t kPhraseCount = sizeof(kPhrases) / sizeof(kPhrases[0]);',
             'struct SystemPhrase { unsigned id; const wchar_t* source; const wchar_t* target; };',
             'inline constexpr SystemPhrase kSystemPhrases[] = {']
    for identity,(source,target) in system:
        rows.append('    {'+str(identity)+'u, L'+json.dumps(source,ensure_ascii=True)+', L'+json.dumps(target,ensure_ascii=True)+'},')
    rows += ['};','inline constexpr std::size_t kSystemPhraseCount = sizeof(kSystemPhrases) / sizeof(kSystemPhrases[0]);',
             '} // namespace sdgo::ui_localization','']
    return '\n'.join(rows).encode('utf-8')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check',action='store_true')
    args=parser.parse_args()
    root=ROOT/'mods/ui_localization'
    output=root/'phrases.generated.h'
    content=render(load(sorted((root/'translations').glob('*.json'))))
    if args.check:
        if not output.is_file() or output.read_bytes()!=content:
            raise SystemExit('Generated UI phrase catalog is stale')
    else:
        output.write_bytes(content)
    phrases,system=load(sorted((root/'translations').glob('*.json')))
    print('UI phrases:',len(phrases),'system UI text rows:',len(system))


if __name__=='__main__':main()
