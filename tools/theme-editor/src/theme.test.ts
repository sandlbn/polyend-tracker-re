import { describe, expect, it } from 'vitest';
import {
  baseTheme, COLOR_COUNT, colorHex, exportTheme, FLAG_NAMES, importTheme,
  MAX_FILE_BYTES, parseColor, SLOT_COUNT, themeData, validateThemeName,
} from './theme';

describe('generated firmware data', () => {
  it('contains every stock slot and a partition of the color slots', () => {
    expect(Object.keys(themeData.palettes)).toEqual(['original', 'monochrome', 'anniversary']);
    for (const slots of Object.values(themeData.palettes)) expect(slots).toHaveLength(SLOT_COUNT);
    expect(themeData.roles).toHaveLength(42);
    expect(themeData.roles.flatMap((role) => role.slots).sort((a, b) => a - b))
      .toEqual(Array.from({ length: COLOR_COUNT }, (_, index) => index));
  });

  it('loads every repo preset without bad lines', () => {
    expect(themeData.presets.length).toBeGreaterThan(0);
    for (const preset of themeData.presets) {
      const result = importTheme(preset.text);
      expect(result.badLines, preset.name).toEqual([]);
      expect(result.theme.slots, preset.name).toHaveLength(SLOT_COUNT);
    }
  });
});

describe('firmware theme text', () => {
  it('accepts supported color spellings and rejects malformed colors', () => {
    expect(parseColor('#ABC')).toBe(0xAABBCC);
    expect(parseColor('#A1B2C3')).toBe(0xA1B2C3);
    expect(parseColor('0xa1b2c3')).toBe(0xA1B2C3);
    expect(parseColor('A1B2C3')).toBe(0xA1B2C3);
    expect(parseColor('#ABCD')).toBeUndefined();
    expect(colorHex(0x12EF)).toBe('#0012EF');
  });

  it('uses late overrides, locks base after colors, and reports bad lines', () => {
    const result = importTheme('base = monochrome\naccent = #123\nslot.130 = #ABCDEF\nbase = original\nslot.277 = #000000\n');
    expect(result.theme.base).toBe('monochrome');
    expect(result.theme.slots[130]).toBe(0xABCDEF);
    expect(result.badLines).toEqual([4, 5]);
  });

  it('parses BOM, comments, case-insensitive keys, and four flags', () => {
    const result = importTheme('\uFEFF; note\r\nBASE = anniversary\r\nFLAG.Bottom_Fade = ON # comment\r\nslot.274 = false\r\nflag.cursor_row_gradient = 1\r\nflag.bar_lines = off\r\n');
    expect(result.badLines).toEqual([]);
    expect(result.theme.base).toBe('anniversary');
    expect(result.theme.slots.slice(COLOR_COUNT)).toEqual([1, 0, 1, 0]);
  });

  it('round trips all 277 independently edited slots', () => {
    const theme = baseTheme('original');
    for (let index = 0; index < COLOR_COUNT; index++) {
      theme.slots[index] = (index * 0x10203 + 0x34567) & 0xffffff;
    }
    for (let index = COLOR_COUNT; index < SLOT_COUNT; index++) theme.slots[index] = 1 - theme.slots[index];
    const text = exportTheme('All slots', theme);
    expect(new TextEncoder().encode(text).length).toBeLessThanOrEqual(MAX_FILE_BYTES);
    expect(importTheme(text).theme).toEqual(theme);
    for (const flag of FLAG_NAMES) expect(text).toContain(`flag.${flag} =`);
  });

  it('restores unreadable foreground colors like firmware', () => {
    const result = importTheme('text = #101010\nbackground = #101010\n');
    expect(result.guarded).toBe(true);
    expect(result.theme.slots[87]).toBe(themeData.palettes.original[87]);
  });

  it('rejects unsafe names, invalid slots, and oversized files', () => {
    expect(validateThemeName('Valid theme 1')).toBeUndefined();
    expect(validateThemeName('../bad')).toBeDefined();
    expect(validateThemeName('A'.repeat(21))).toBeDefined();
    const theme = baseTheme('original');
    theme.slots[3] = 0x1000000;
    expect(() => exportTheme('Test', theme)).toThrow('slot 3');
    expect(() => importTheme('x'.repeat(MAX_FILE_BYTES + 1))).toThrow('8192 bytes');
  });
});
