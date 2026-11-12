import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import PreviewGallery, { slotSpecimens } from './PreviewGallery';
import { baseTheme, colorHex } from './theme';

describe('screen gallery', () => {
  it('keeps all 15 screens visible while one is focused', () => {
    const html = renderToStaticMarkup(<PreviewGallery slots={baseTheme('original').slots} focus="playback" selectedSlot={244} onFocus={() => {}} onPickSlot={() => {}} />);
    expect((html.match(/class="screen-card /g) ?? [])).toHaveLength(15);
    expect((html.match(/class="screen-card focused"/g) ?? [])).toHaveLength(1);
    expect((html.match(/aria-pressed="(?:true|false)"/g) ?? [])).toHaveLength(30);
    for (const title of ['File - Open Project', 'Sample Loader', 'Pattern', 'Sample Playback', 'Song', 'Instrument Parameters 1/2', 'Sample Editor 1/2', 'Config', 'Sample Recorder', 'Performance FX', 'Synth Parameters 1/2', 'Games', 'Master 1/3', 'Resample', 'Track Mixer 2/3']) {
      expect(html).toContain(`${title} themed preview`);
    }
  });

  it('applies edited colors and firmware style flags to previews', () => {
    const theme = baseTheme('original');
    theme.slots[244] = 0x123abc;
    theme.slots[207] = 0xabcdef;
    const render = (focus: string) => renderToStaticMarkup(<PreviewGallery slots={theme.slots} focus={focus} selectedSlot={244} onFocus={() => {}} onPickSlot={() => {}} />);
    expect(render('playback').includes('data-slot="244" fill="#123ABC"')).toBe(true);
    expect(render('song').includes('fill="#ABCDEF"')).toBe(true);
    expect(render('mixer').includes('fill="url(#mixer-valuebar)"')).toBe(true);
    theme.slots[274] = 0;
    expect(render('mixer').includes('fill="url(#mixer-valuebar)"')).toBe(false);
    theme.slots[275] = 0;
    expect(render('pattern').includes('fill="url(#pattern-cursor)"')).toBe(false);
  });

  it('draws Sample Loader selections as transparent theme-colored borders', () => {
    const slots = baseTheme('original').slots;
    const html = renderToStaticMarkup(<PreviewGallery slots={slots} focus="loader" selectedSlot={130} onFocus={() => {}} onPickSlot={() => {}} />);
    const loader = html.match(/<svg[^>]*aria-label="Sample Loader themed preview"[\s\S]*?<\/svg>/)?.[0] ?? '';
    expect(loader).toContain(`x="4" y="188" width="291" height="29" fill="none" stroke="${colorHex(slots[130])}"`);
    expect(loader).toContain(`x="304" y="64" width="291" height="29" fill="none" stroke="${colorHex(slots[83])}"`);
  });

  it('fills only the Pattern note cell and outlines its track row', () => {
    const slots = baseTheme('original').slots;
    const html = renderToStaticMarkup(<PreviewGallery slots={slots} focus="pattern" selectedSlot={130} onFocus={() => {}} onPickSlot={() => {}} />);
    const pattern = html.match(/<svg[^>]*aria-label="Pattern themed preview"[\s\S]*?<\/svg>/)?.[0] ?? '';
    expect(pattern).toContain(`x="29" y="211" width="41" height="26" rx="0" fill="${colorHex(slots[130])}"`);
    expect(pattern).toContain(`x="29" y="211" width="181" height="26" fill="none" stroke="${colorHex(slots[130])}"`);
    expect(pattern).not.toContain(`x="26" y="211" width="186" height="26" rx="0" fill="${colorHex(slots[130])}"`);
    expect(pattern).toContain(`fill="${colorHex(slots[194])}"`);
    expect(pattern).toContain('x="0" y="262" width="800" height="1" opacity="0.525"');
  });

  it('uses outline selections across list and parameter screens', () => {
    const slots = baseTheme('original').slots;
    slots[83] = 0x123456;
    slots[130] = 0xabcdef;
    const html = renderToStaticMarkup(<PreviewGallery slots={slots} focus="config" selectedSlot={87} onFocus={() => {}} onPickSlot={() => {}} />);
    const screen = (title: string) => html.match(new RegExp(`<svg[^>]*aria-label="${title} themed preview"[\\s\\S]*?<\\/svg>`))?.[0] ?? '';
    for (const title of ['File - Open Project', 'Sample Playback', 'Instrument Parameters 1/2', 'Sample Editor 1/2', 'Config', 'Sample Recorder', 'Synth Parameters 1/2', 'Games', 'Master 1/3', 'Resample']) {
      expect(screen(title), title).toContain('fill="none" stroke="#123456"');
    }
    for (const title of ['File - Open Project', 'Sample Loader', 'Instrument Parameters 1/2', 'Config', 'Sample Recorder', 'Synth Parameters 1/2', 'Master 1/3', 'Resample']) {
      expect(screen(title), title).toContain('fill="none" stroke="#ABCDEF"');
      expect(screen(title), title).not.toContain('fill="#ABCDEF"');
    }
    expect(screen('Song')).toContain(`fill="none" stroke="${colorHex(slots[208])}"`);
  });

  it('matches Sample Playback waveform, mode list, and footer geometry', () => {
    const slots = baseTheme('original').slots;
    const html = renderToStaticMarkup(<PreviewGallery slots={slots} focus="playback" selectedSlot={130} onFocus={() => {}} onPickSlot={() => {}} />);
    const playback = html.match(/<svg[^>]*aria-label="Sample Playback themed preview"[\s\S]*?<\/svg>/)?.[0] ?? '';
    expect(playback).toContain('x="44" y="116" width="443" height="219"');
    expect(playback).toContain('x="0" y="225" width="600" height="2"');
    expect(playback).toContain('x="604" y="33" width="191" height="27" fill="none"');
    expect(playback).toContain('>Play Mode</text>');
    expect(playback).not.toContain('ENVELOPE');
    expect(playback).not.toContain('ONE SHOT');
  });

  it('marks screen cards and painted elements for slot navigation', () => {
    const slots = baseTheme('original').slots;
    const html = renderToStaticMarkup(<PreviewGallery slots={slots} focus="pattern" selectedSlot={194} onFocus={() => {}} onPickSlot={() => {}} />);
    expect(html).toContain('data-screen-id="pattern"');
    expect(html).toContain('data-screen-id="playback"');
    expect(html).toContain('data-slot="194"');
    expect(html).toContain('data-slot="244"');
    expect(html).toContain('data-slot="83"');
  });

  it('offers a Config specimen for optional text box background slot 153', () => {
    const slots = baseTheme('original').slots;
    slots[153] = 0x123abc;
    expect(slotSpecimens[153].screenId).toBe('config');
    const html = renderToStaticMarkup(<PreviewGallery slots={slots} focus="config" selectedSlot={153}
      jumpTarget={{ slot: 153, screenId: 'config' }} onFocus={() => {}} onPickSlot={() => {}} />);
    expect(html).toContain('data-slot="153"');
    expect(html).toContain('background-color:#123ABC');
    expect(html).toContain('Optional text box background');
    expect(html).toContain('this screen does not draw the optional background');
  });
});
