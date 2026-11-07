import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { importApi } from 'cydoc/api-import';

// A small neutral-format producer for explicit C ABI declarations. This is not a C++ parser.
// Descriptions are reviewed in both languages; implementation bodies never enter the producer.
export async function generateApi({ root, workspace, content }) {
  const catalog = JSON.parse(await readFile(join(workspace, 'api/catalog.json'), 'utf8'));
  const descriptions = new Map(), owners = new Map();
  const perLocale = { en: [], fr: [] };
  const normalize = value => value.replace(/\s+/g, ' ').trim();
  for (const item of catalog.symbols) {
    const source = (await readFile(join(root, item.header), 'utf8')).replace(/\/\*[\s\S]*?\*\//g, '');
    const scope = item.owner ? source.match(new RegExp(`typedef\\s+struct\\s+${item.owner}\\s*\\{([\\s\\S]*?)\\}\\s*${item.owner}\\s*;`))?.[1] : source;
    if (!scope) throw new Error(`Public ABI structure no longer exists: ${item.owner}`);
    const pattern = item.entry
      ? new RegExp(`typedef\\s+([^;{}]+?)\\(\\s*\\*${item.field}\\s*\\)\\s*\\(([^;{}]*?)\\)\\s*;`)
      : new RegExp(`(?:^|;)\\s*([^;{}]+?)\\(\\s*\\*${item.field}\\s*\\)\\s*\\(([^;{}]*?)\\)\\s*;`);
    const declaration = scope.match(pattern);
    if (!declaration) throw new Error(`Public ABI declaration changed: ${item.owner || ''}/${item.field}`);
    if (item.entry && !source.includes(`"${item.entry}"`)) throw new Error(`Unknown exported entry: ${item.entry}`);
    const resultType = normalize(declaration[1]), argumentsText = normalize(declaration[2]);
    const name = item.entry || `${item.owner}::${item.field}`;
    const parameters = argumentsText === 'void' || !argumentsText ? [] : argumentsText.split(',').map(value => {
      const text = value.trim(), parameter = text.match(/([A-Za-z_]\w*)$/)?.[1];
      if (!parameter || !catalog.parameters[parameter]) throw new Error(`Translate the new ABI parameter: ${name}/${parameter}`);
      return { name: parameter, type: text.slice(0, -parameter.length).trim() };
    });
    descriptions.set(name, item.description);
    owners.set(name, item.header.endsWith('cyimg_plugin.h') ? 'decoder-api' : item.header.endsWith('cyimg_metadata_plugin.h') ? 'metadata-api' : 'editor-api');
    for (const locale of ['en', 'fr']) {
      if (!item.description[locale] || !item.returns[locale]) throw new Error(`Missing public API translation: ${item.key}/${locale}`);
      perLocale[locale].push({ key: item.key, name, kind: 'function', language: 'c',
        signature: item.entry ? `${resultType} ${item.entry}(${argumentsText});` : `${resultType} (*${item.field})(${argumentsText});`,
        description: `${item.description[locale]}\n\n\`\`\`c\n#include "${item.header.split('/').at(-1)}"\n\`\`\``,
        parameters: parameters.map(parameter => ({ ...parameter, description: catalog.parameters[parameter.name][locale] })),
        returnType: resultType, returns: item.returns[locale], references: item.references.map(key => {
          const referenced = catalog.symbols.find(value => value.key === key);
          if (!referenced) throw new Error(`Unknown public API relation: ${key}`);
          return { key, name: referenced.entry || `${referenced.owner}::${referenced.field}` };
        }) });
    }
  }
  for (const locale of ['en', 'fr']) {
    const input = `generated/api.${locale}.json`;
    await writeFile(join(workspace, input), JSON.stringify({ symbols: perLocale[locale] }, null, 2));
    const pages = await importApi([{ format: 'cydoc', path: input, prefix: 'api', locale }], {
      base: workspace, locales: [{ code: 'en' }, { code: 'fr' }]
    });
    for (const page of pages) {
      const metadata = { id: page.id, title: page.title, description: descriptions.get(page.title)[locale].split('. ')[0].replace(/[.!?]+$/, '') + '.',
        section: locale === 'fr' ? 'API / Fonctions publiques' : 'API / Public functions', parent: owners.get(page.title),
        order: 300, kind: 'api', symbol: page.symbol, [locale === 'en' ? 'revision' : 'sourceRevision']: 1 };
      await writeFile(join(content, locale, `${page.id}.md`), `---\n${Object.entries(metadata).map(([key, value]) => `${key}: ${value}`).join('\n')}\n---\n\n${page.markdown}\n`);
    }
  }
}
