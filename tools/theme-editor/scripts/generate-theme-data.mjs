import { readFileSync, readdirSync, mkdirSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const editor = path.join(root, 'tools/theme-editor');
const cpp = readFileSync(path.join(root, 'src/core/ui/theme.cpp'), 'utf8');
const fileCpp = readFileSync(path.join(root, 'src/core/ui/theme_file.cpp'), 'utf8');
const names = ['Original', 'Monochrome', 'Anniversary'];
const palettes = {};

for (const name of names) {
  const match = cpp.match(new RegExp(`const uint32_t k${name}Slots\\[kThemeSlotCount\\] = \\{([\\s\\S]*?)\\};`));
  if (!match) throw new Error(`Missing ${name} stock palette`);
  const slots = [...match[1].matchAll(/0x[0-9a-fA-F]+/g)].map(([hex]) => Number.parseInt(hex, 16));
  if (slots.length !== 277) throw new Error(`${name}: expected 277 slots, got ${slots.length}`);
  palettes[name.toLowerCase()] = slots;
}

const arrays = new Map();
for (const match of fileCpp.matchAll(/(?:const uint16_t\s+|,\s*)(k\w+)\[\]\s*=\s*\{([^}]*)\}/g)) {
  arrays.set(match[1], [...match[2].matchAll(/\d+/g)].map(([n]) => Number(n)));
}
const roles = [...fileCpp.matchAll(/R\("([a-z0-9_]+)",\s*(k\w+)\)/g)].map(([, name, arrayName]) => {
  const slots = arrays.get(arrayName);
  if (!slots) throw new Error(`Missing slot array ${arrayName} for ${name}`);
  return { name, slots };
});
if (roles.length !== 42) throw new Error(`Expected 42 roles, got ${roles.length}`);
const coverage = roles.flatMap(({ slots }) => slots).sort((a, b) => a - b);
if (coverage.length !== 273 || coverage.some((slot, index) => slot !== index)) {
  throw new Error('Role mapping must partition color slots 0..272');
}

const presets = readdirSync(path.join(root, 'themes'))
  .filter((name) => name.toLowerCase().endsWith('.txt'))
  .sort((a, b) => a.localeCompare(b, 'en'))
  .map((file) => ({ name: file.slice(0, -4), text: readFileSync(path.join(root, 'themes', file), 'utf8') }));

const output = { palettes, roles, presets };
const outputDir = path.join(editor, '.generated');
mkdirSync(outputDir, { recursive: true });
writeFileSync(path.join(outputDir, 'themes.json'), `${JSON.stringify(output, null, 2)}\n`);
console.log(`Generated 3 palettes, ${roles.length} roles, ${presets.length} presets`);
