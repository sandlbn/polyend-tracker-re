import source from '../.generated/themes.json';

export const SLOT_COUNT = 277;
export const COLOR_COUNT = 273;
export const MAX_FILE_BYTES = 8192;
export const FLAG_NAMES = ['bottom_fade', 'valuebar_gradient', 'cursor_row_gradient', 'bar_lines'] as const;
export type BaseName = 'original' | 'monochrome' | 'anniversary';
export type ThemeRole = { name: string; slots: number[] };
export type ThemePreset = { name: string; text: string };
export type ThemeData = {
  palettes: Record<BaseName, number[]>;
  roles: ThemeRole[];
  presets: ThemePreset[];
};
export const themeData: ThemeData = source as ThemeData;

export type ThemeDocument = { base: BaseName; slots: number[] };
export type ImportResult = {
  theme: ThemeDocument;
  badLines: number[];
  guarded: boolean;
};

const BASES: BaseName[] = ['original', 'monochrome', 'anniversary'];
const roleByName = new Map(themeData.roles.map((role) => [role.name, role]));
const flagByName = new Map<string, number>(FLAG_NAMES.map((flag, index) => [flag, COLOR_COUNT + index]));

export function baseTheme(base: BaseName): ThemeDocument {
  return { base, slots: [...themeData.palettes[base]] };
}

export function colorHex(value: number): string {
  return `#${(value & 0xffffff).toString(16).toUpperCase().padStart(6, '0')}`;
}

export function parseColor(value: string): number | undefined {
  let hex = value;
  if (hex.startsWith('#')) {
    hex = hex.slice(1);
    if (hex.length === 3 && /^[0-9a-f]{3}$/i.test(hex)) {
      hex = [...hex].map((digit) => digit + digit).join('');
    }
  } else if (/^0x/i.test(hex)) {
    hex = hex.slice(2);
  }
  return /^[0-9a-f]{6}$/i.test(hex) ? Number.parseInt(hex, 16) : undefined;
}

function parseFlag(value: string): number | undefined {
  if (['1', 'on', 'true'].includes(value.toLowerCase())) return 1;
  if (['0', 'off', 'false'].includes(value.toLowerCase())) return 0;
  return undefined;
}

function restoreUnreadableColors(theme: ThemeDocument): boolean {
  let guarded = false;
  const base = themeData.palettes[theme.base];
  for (const [foreground, background] of [
    ['text', 'background'],
    ['label_text', 'label_bg'],
    ['title_text', 'title_bg'],
  ]) {
    const foregroundSlots = roleByName.get(foreground)!.slots;
    const backgroundSlots = roleByName.get(background)!.slots;
    if (theme.slots[foregroundSlots[0]] !== theme.slots[backgroundSlots[0]]) continue;
    guarded = true;
    for (const slot of foregroundSlots) theme.slots[slot] = base[slot];
    if (theme.slots[foregroundSlots[0]] === theme.slots[backgroundSlots[0]]) {
      for (const slot of backgroundSlots) theme.slots[slot] = base[slot];
    }
  }
  return guarded;
}

export function importTheme(text: string): ImportResult {
  if (new TextEncoder().encode(text).length > MAX_FILE_BYTES) {
    throw new Error(`Theme exceeds firmware limit of ${MAX_FILE_BYTES} bytes`);
  }
  const theme = baseTheme('original');
  const badLines: number[] = [];
  let locked = false;
  const lines = text.replace(/^\uFEFF/, '').split(/\n/);
  lines.forEach((rawLine, lineIndex) => {
    const line = rawLine.replace(/\r$/, '');
    if (new TextEncoder().encode(line).length > 127) {
      badLines.push(lineIndex + 1);
      return;
    }
    const trimmed = line.trimStart();
    if (!trimmed || trimmed.startsWith('#') || trimmed.startsWith(';')) return;
    const match = trimmed.match(/^([A-Za-z_][A-Za-z0-9_.]*)[ \t]*=[ \t]*([^\s;#]+|#[^\s;#]+)(?:[ \t]*(?:#|;).*)?[ \t]*$/);
    if (!match || match[1].length > 24) {
      badLines.push(lineIndex + 1);
      return;
    }
    const key = match[1].toLowerCase();
    const value = match[2];
    let applied = false;
    if (key === 'base') {
      const base = BASES.find((name, index) => name === value.toLowerCase() || String(index) === value);
      if (base && !locked) {
        theme.base = base;
        theme.slots = [...themeData.palettes[base]];
        applied = true;
      }
    } else if (key.startsWith('slot.')) {
      const indexText = key.slice(5);
      if (/^\d{1,4}$/.test(indexText)) {
        const index = Number(indexText);
        if (index < SLOT_COUNT) {
          const parsed = index < COLOR_COUNT ? parseColor(value) : parseFlag(value);
          if (parsed !== undefined) {
            theme.slots[index] = parsed;
            applied = true;
            locked = true;
          }
        }
      }
    } else if (key.startsWith('flag.')) {
      const index = flagByName.get(key.slice(5));
      const parsed = parseFlag(value);
      if (index !== undefined && parsed !== undefined) {
        theme.slots[index] = parsed;
        applied = true;
        locked = true;
      }
    } else {
      const role = roleByName.get(key);
      const parsed = parseColor(value);
      if (role && parsed !== undefined) {
        for (const slot of role.slots) theme.slots[slot] = parsed;
        applied = true;
        locked = true;
      }
    }
    if (!applied) badLines.push(lineIndex + 1);
  });
  return { theme, badLines, guarded: restoreUnreadableColors(theme) };
}

export function validateThemeName(name: string): string | undefined {
  if (!name || name.length > 20 || /[^\x20-\x7e]|[\\/:]/.test(name)) {
    return 'Name must contain 1–20 printable ASCII characters, excluding /, \\, and :';
  }
  return undefined;
}

export function exportTheme(name: string, theme: ThemeDocument): string {
  const nameError = validateThemeName(name);
  if (nameError) throw new Error(nameError);
  if (!BASES.includes(theme.base) || theme.slots.length !== SLOT_COUNT) throw new Error('Invalid theme data');
  const base = themeData.palettes[theme.base];
  const lines = [`# OpenTracker theme "${name}"`, `base = ${theme.base}`, ''];
  for (let index = 0; index < SLOT_COUNT; index++) {
    const value = theme.slots[index];
    if (!Number.isInteger(value) || value < 0 || value > (index < COLOR_COUNT ? 0xffffff : 1)) {
      throw new Error(`Invalid value for slot ${index}`);
    }
    if (value === base[index]) continue;
    lines.push(index < COLOR_COUNT
      ? `slot.${index} = ${colorHex(value)}`
      : `flag.${FLAG_NAMES[index - COLOR_COUNT]} = ${value}`);
  }
  const output = `${lines.join('\n')}\n`;
  if (new TextEncoder().encode(output).length > MAX_FILE_BYTES) {
    throw new Error(`Export exceeds firmware limit of ${MAX_FILE_BYTES} bytes`);
  }
  return output;
}
