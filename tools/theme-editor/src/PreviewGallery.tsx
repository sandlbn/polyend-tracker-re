import { useEffect, useRef, type ReactNode } from 'react';
import { colorHex } from './theme';

type ScreenId = 'file' | 'loader' | 'pattern' | 'playback' | 'song' | 'instrument' | 'editor' | 'config' | 'recorder' | 'performance' | 'synth' | 'games' | 'master' | 'resample' | 'mixer';
export type SlotJump = { slot: number; screenId: string };
// Some firmware slots have no visible use in the representative screen states.
// Keep their color specimen outside the SVG so it cannot be mistaken for firmware UI.
export const slotSpecimens: Record<number, { screenId: string; label: string }> = {
  153: { screenId: 'config', label: 'Optional text box background' },
};
const screens: Array<{ id: ScreenId; title: string; short: string }> = [
  { id: 'file', title: 'File - Open Project', short: 'File' },
  { id: 'loader', title: 'Sample Loader', short: 'Loader' },
  { id: 'pattern', title: 'Pattern', short: 'Pattern' },
  { id: 'playback', title: 'Sample Playback', short: 'Playback' },
  { id: 'song', title: 'Song', short: 'Song' },
  { id: 'instrument', title: 'Instrument Parameters 1/2', short: 'Instrument' },
  { id: 'editor', title: 'Sample Editor 1/2', short: 'Editor' },
  { id: 'config', title: 'Config', short: 'Config' },
  { id: 'recorder', title: 'Sample Recorder', short: 'Recorder' },
  { id: 'performance', title: 'Performance FX', short: 'Perform' },
  { id: 'synth', title: 'Synth Parameters 1/2', short: 'Synth' },
  { id: 'games', title: 'Games', short: 'Games' },
  { id: 'master', title: 'Master 1/3', short: 'Master' },
  { id: 'resample', title: 'Resample', short: 'Resample' },
  { id: 'mixer', title: 'Track Mixer 2/3', short: 'Mixer' },
];
type ArtProps = { slots: number[]; selectedSlot: number; pick: (slot: number) => void; gradientId?: string };
const paint = (slots: number[], slot: number) => colorHex(slots[slot]);

function R({ x, y, w, h, slot, slots, pick, selectedSlot, rx = 0 }: ArtProps & { x: number; y: number; w: number; h: number; slot: number; rx?: number }) {
  return <rect className="paintable" data-slot={slot} x={x} y={y} width={w} height={h} rx={rx} fill={paint(slots, slot)}
    stroke={selectedSlot === slot && slot !== 0 ? '#FFFFFF' : 'none'} strokeWidth={selectedSlot === slot ? 2 : 0}
    onClick={(event) => { event.stopPropagation(); pick(slot); }} />;
}
function SelectionFrame({ x, y, w, h, slot, slots, pick }: ArtProps & { x: number; y: number; w: number; h: number; slot: number }) {
  return <rect className="paintable" data-slot={slot} x={x} y={y} width={w} height={h} fill="none" stroke={paint(slots, slot)} strokeWidth={2}
    onClick={(event) => { event.stopPropagation(); pick(slot); }} />;
}
function T({ x, y, text, slot, slots, pick, size = 17, weight = 400, anchor = 'start' }: ArtProps & { x: number; y: number; text: string; slot: number; size?: number; weight?: number; anchor?: 'start' | 'middle' | 'end' }) {
  return <text className="paintable" data-slot={slot} x={x} y={y} fill={paint(slots, slot)} fontSize={size} fontWeight={weight} textAnchor={anchor}
    onClick={(event) => { event.stopPropagation(); pick(slot); }}>{text}</text>;
}

function Chrome({ title, subtitle, buttons, values, ...p }: ArtProps & { title: string; subtitle?: string; buttons: string[]; values?: string[] }) {
  return <>
    <R {...p} x={0} y={0} w={800} h={480} slot={0} />
    <R {...p} x={0} y={0} w={800} h={28} slot={2} />
    <T {...p} x={12} y={20} text={title} slot={1} size={19} weight={700} />
    {subtitle && <T {...p} x={788} y={20} text={subtitle} slot={5} anchor="end" size={14} />}
    <R {...p} x={0} y={28} w={800} h={2} slot={266} />
    <R {...p} x={0} y={430} w={800} h={50} slot={58} />
    {buttons.map((label, index) => <g key={index}>
      <R {...p} x={index * 100 + 1} y={433} w={98} h={44} slot={10} />
      {Boolean(p.slots[276]) && <g data-slot={276}><R {...p} x={index * 100 + 4} y={433} w={92} h={2} slot={11} /></g>}
      <T {...p} x={index * 100 + 50} y={values ? 449 : 461} text={label} slot={9} size={13} anchor="middle" />
      {values?.[index] && <T {...p} x={index * 100 + 50} y={469} text={values[index]} slot={12} size={13} anchor="middle" />}
    </g>)}
  </>;
}

function Fader({ x, y, width, height, level, fillSlot, ...p }: ArtProps & { x: number; y: number; width: number; height: number; level: number; fillSlot: number }) {
  const inset = width < 60 ? 11 : 14;
  const fillX = x + inset;
  const fillW = width - 2 * inset;
  const bottom = y + height - 15;
  const fillH = Math.max(1, Math.round((height - 25) * level));
  return <>
    <R {...p} x={x} y={y} w={width} h={height} slot={50} />
    <R {...p} x={fillX} y={bottom - fillH} w={fillW} h={fillH} slot={fillSlot} />
    {Boolean(p.slots[274]) && <rect className="paintable" data-slot={274} x={fillX} y={bottom - fillH} width={Math.min(9, fillW)} height={fillH} fill={`url(#${p.gradientId}-valuebar)`} onClick={() => p.pick(274)} />}
    {Boolean(p.slots[273]) && <rect data-slot={273} x={x} y={y + height - 11} width={width} height={11} fill={`url(#${p.gradientId}-fade)`} pointerEvents="none" />}
  </>;
}

function Pattern(p: ArtProps) {
  const rowNumbers = [56, 57, 58, 59, 60, 61, 62, 63, 64, 1, 2, 3, 4, 5, 6];
  const trackNames = ['Kick', 'Snare', 'Hats', 'Bass'];
  const rowTop = (row: number) => 29 + row * 26;
  const cursorY = rowTop(7);
  const wrapY = rowTop(9) - 1;
  return <>
    <Chrome {...p} title="Intro" subtitle="0m:00s  ▌▌  P1.063                  1. Drum Kit 8 Hits" buttons={['Kick', 'Snare', 'Hats', 'Bass', 'Lead', 'Pad', 'Perc', 'FX']} values={Array(8).fill('Mute')} />
    <R {...p} x={0} y={29} w={800} h={395} slot={175} />
    {rowNumbers.map((number, row) => {
      const y = rowTop(row);
      return <g key={row}>
        <R {...p} x={0} y={y} w={800} h={26} slot={row === 7 ? 187 : row % 4 === 0 ? 185 : 175} />
        <T {...p} x={4} y={y + 17} text={String(number)} slot={66} size={13} />
        {Array.from({ length: 4 }, (_, track) => {
          const x = 33 + track * 186;
          const active = row > 0 && ((track === 0 && row > 8) || (track === 2 && row % 2 === 0) || (track === 0 && row % 3 === 1));
          return <g key={track}>
            <T {...p} x={x} y={y + 17} text={active ? (track === 2 ? 'C5' : ['C2', 'C#2', 'C5', 'D2'][row % 4]) : '---'} slot={active ? 176 : 180} size={15} />
            <T {...p} x={x + 46} y={y + 17} text={active ? (track === 2 ? '03' : '01') : '--'} slot={active ? 177 : 181} size={15} />
            <T {...p} x={x + 84} y={y + 17} text={active && row < 9 ? 'V 36' : '---'} slot={active ? 178 : 182} size={15} />
            <T {...p} x={x + 130} y={y + 17} text={active && track === 2 ? 'm 12' : '---'} slot={active ? 179 : 183} size={13} />
          </g>;
        })}
      </g>;
    })}
    {trackNames.map((name, track) => <g key={name}>
      <R {...p} x={26 + track * 186} y={31} w={58} h={23} slot={174} />
      <T {...p} x={30 + track * 186} y={49} text={name} slot={87} size={17} weight={700} />
      <R {...p} x={26 + track * 186} y={55} w={2} h={368} slot={272} />
    </g>)}
    {Boolean(p.slots[275]) && <>
      <rect data-slot={275} x={1} y={cursorY - 11} width={798} height={10} fill={`url(#${p.gradientId}-cursor)`} opacity="0.32" pointerEvents="none" />
      <rect data-slot={275} x={1} y={cursorY + 26} width={798} height={10} fill={`url(#${p.gradientId}-cursor)`} opacity="0.32" pointerEvents="none" />
    </>}
    <R {...p} x={29} y={cursorY} w={41} h={26} slot={130} />
    <SelectionFrame {...p} x={29} y={cursorY} w={181} h={26} slot={130} />
    <T {...p} x={35} y={cursorY + 17} text="C2" slot={188} size={15} />
    <g className="paintable" data-slot={194} fill={paint(p.slots, 194)} onClick={(event) => { event.stopPropagation(); p.pick(194); }}>
      <rect x={0} y={wrapY - 1} width={800} height={1} opacity={0.055} />
      <rect x={0} y={wrapY} width={800} height={1} opacity={0.525} />
      <rect x={0} y={wrapY + 1} width={800} height={1} opacity={0.055} />
    </g>
  </>;
}

function Mixer(p: ArtProps) {
  const levels = [0.72, 0.48, 0.85, 0.32, 0.61, 0.43, 0.77, 0.56];
  const names = ['Kick', 'Snare', 'Hats', 'Bass', 'Lead', 'Pad', 'Perc', 'FX'];
  return <>
    <Chrome {...p} title="Track Mixer 2/3" buttons={levels.map((level) => `${Math.round((level - 1) * 24)} dB`)} values={Array(8).fill('Mute')} />
    {levels.map((level, index) => {
      const x = index * 100 + 2;
      return <g key={index}>
        <R {...p} x={x} y={32} w={96} h={27} slot={2} />
        <T {...p} x={x + 48} y={51} text={names[index]} slot={1} size={15} anchor="middle" weight={700} />
        <Fader {...p} x={x} y={64} width={47} height={359} level={level} fillSlot={index === 3 ? 78 : 73} />
        <Fader {...p} x={x + 50} y={64} width={47} height={359} level={level * 0.82} fillSlot={270} />
        <R {...p} x={x + 61} y={408 - level * 334} w={26} h={1} slot={270} />
      </g>;
    })}
  </>;
}

function Loader(p: ArtProps) {
  const files = ['../', '/Drums', '/Bass', '/Keys', 'analog_kick.wav', 'dusty_snare.wav', 'closed_hat.wav', 'clap_02.wav', 'tom_low.wav', 'shaker.wav', 'rimshot.wav'];
  const instruments = ['01  Analog Kick', '02  Dusty Snare', '03  Closed Hat', '04  Clap 02', '05  (empty)', '06  (empty)', '07  (empty)'];
  return <>
    <Chrome {...p} title="Sample Loader" buttons={['Load', 'Preview', 'Folder', 'Rename', 'Copy', 'Delete', 'Exit', 'More']} />
    {[[1, 'SD Card'], [301, 'Instruments']].map(([x, label]) => <g key={label}>
      <R {...p} x={Number(x)} y={35} w={297} h={24} slot={149} />
      <T {...p} x={Number(x) + 12} y={56} text={String(label)} slot={148} size={16} weight={700} />
      <R {...p} x={Number(x)} y={61} w={297} h={362} slot={86} />
    </g>)}
    {files.map((file, index) => <g key={file}>
      {index === 4 && <SelectionFrame {...p} x={4} y={64 + index * 31} w={291} h={29} slot={130} />}
      <T {...p} x={12} y={85 + index * 31} text={file} slot={index === 4 ? 87 : 101} size={16} />
    </g>)}
    {instruments.map((name, index) => <g key={name}>
      {index === 0 && <SelectionFrame {...p} x={304} y={64 + index * 31} w={291} h={29} slot={83} />}
      <T {...p} x={313} y={85 + index * 31} text={name} slot={index < 4 ? 87 : 66} size={16} />
    </g>)}
    <Fader {...p} x={601} y={29} width={97} height={394} level={0.74} fillSlot={73} />
    <Fader {...p} x={701} y={29} width={97} height={394} level={0.42} fillSlot={73} />
    <R {...p} x={715} y={252} w={69} h={8} slot={51} />
    <SelectionFrame {...p} x={1} y={31} w={297} h={392} slot={130} />
  </>;
}

function Performance(p: ArtProps) {
  const fx = ['Volume/Velocity', 'Sample panning', 'Tune', 'Sample LP cutoff', 'Sample HP cutoff', 'Sample delay send', 'Sample reverb send', 'Step Repeater', 'Bit Depth', 'Overdrive', 'Volume LFO speed', 'Pattern Length'];
  return <>
    <Chrome {...p} title="Performance FX" subtitle="0m:00s  ▌▌  P1.001" buttons={['Kick', 'Snare', 'Hats', 'Bass', 'Lead', 'Pad', 'Perc', 'FX']} values={Array(8).fill('Mute')} />
    {fx.map((label, index) => {
      const x = 11 + index * 65;
      return <g key={index}>
        <R {...p} x={x} y={34} w={63} h={37} slot={235} />
        <T {...p} x={x + 32} y={59} text={index === 7 ? 'Off' : '---'} slot={234} size={15} anchor="middle" />
        <text className="paintable" data-slot={230} transform={`translate(${x + 40} 266) rotate(-90)`} fill={paint(p.slots, 230)} fontSize="16" onClick={() => p.pick(230)}>{label}</text>
        <R {...p} x={x} y={279} w={63} h={146} slot={235} />
        {[0, 1, 2, 3].map((row) => <T key={row} {...p} x={x + 32} y={307 + 30 * row} text={index === 7 && row === 0 ? 'Off' : row === 0 ? '---' : `${row % 2 ? '+' : '-'}${(index + row) * 8}`} slot={row === 0 ? 236 : 234} size={15} anchor="middle" />)}
      </g>;
    })}
  </>;
}

function Instrument(p: ArtProps) {
  const labels = ['Volume', 'Panning', 'Tune', 'Finetune', 'Filter Type', 'Cutoff', 'Resonance', 'More'];
  const textValues = ['-6.00 dB', '0', '+2', '0', 'Low-pass', '65', '28', ''];
  const levels = [0.83, 0.5, 0.56, 0.5, 0, 0.65, 0.28];
  return <>
    <Chrome {...p} title="Instrument Parameters 1/2" subtitle="1. Analog Kick" buttons={labels} values={textValues} />
    {labels.slice(0, 7).map((label, index) => {
      const x = index * 100 + 2;
      return <g key={label}>
        {index === 4 ? <>
          <R {...p} x={x} y={29} w={96} h={394} slot={50} />
          {['Disabled', 'Low-pass', 'High-pass', 'Band-pass'].map((name, item) => <g key={name}>
            {item === 1 && <SelectionFrame {...p} x={x + 5} y={71 + item * 36} w={86} h={33} slot={83} />}
            <T {...p} x={x + 8} y={95 + item * 36} text={name} slot={item === 1 ? 87 : 101} size={12} />
          </g>)}
        </> : index >= 1 && index <= 3 ? <>
          <R {...p} x={x} y={29} w={96} h={394} slot={50} />
          <R {...p} x={x + 12} y={226} w={72} h={1} slot={66} />
          <R {...p} x={x + 12} y={levels[index] > 0.5 ? 226 - (levels[index] - 0.5) * 340 : 226} w={72} h={Math.max(2, Math.abs(levels[index] - 0.5) * 340)} slot={73} />
        </> : <Fader {...p} x={x} y={29} width={96} height={394} level={levels[index]} fillSlot={73} />}
      </g>;
    })}
    <R {...p} x={702} y={29} w={96} h={394} slot={50} />
    <SelectionFrame {...p} x={1} y={31} w={98} h={392} slot={130} />
  </>;
}

function Playback(p: ArtProps) {
  const modes = ['1-Shot', 'Forward Loop', 'Backward Loop', 'Pingpong Loop', 'Slice', 'Beat Slice', 'Wavetable', 'Granular'];
  return <>
    <Chrome {...p} title="Sample Playback" subtitle="1. Analog Kick" buttons={['Preview', 'Start', '', '', 'End', 'Zoom', '', '']} values={['-', '0.094s', '', '', '1.043s', '1.00', '', '']} />
    <R {...p} x={44} y={116} w={443} h={219} slot={247} />
    <R {...p} x={0} y={225} w={600} h={2} slot={245} />
    <g className="paintable" data-slot={244} fill={paint(p.slots, 244)} onClick={(event) => { event.stopPropagation(); p.pick(244); }}>
    {Array.from({ length: 300 }, (_, i) => {
      const x = i * 2;
      const decay = Math.exp(-Math.max(0, x - 52) / 178);
      const pulse = Math.abs(Math.sin(i * 0.73) * Math.cos(i * 0.19));
      const amp = Math.max(1, Math.round((0.17 + 0.83 * decay) * pulse * 98));
      return <rect key={i} x={x} y={225 - amp} width={2} height={amp * 2 + 1} />;
    })}
    </g>
    <R {...p} x={44} y={108} w={2} h={227} slot={244} />
    <path className="paintable" data-slot={244} d="M44 108 L52 112 L44 116 Z" fill="none" stroke={paint(p.slots, 244)} strokeWidth={1.5} onClick={() => p.pick(244)} />
    <R {...p} x={486} y={108} w={2} h={227} slot={244} />
    <path className="paintable" data-slot={244} d="M486 108 L478 112 L486 116 Z" fill="none" stroke={paint(p.slots, 244)} strokeWidth={1.5} onClick={() => p.pick(244)} />
    <R {...p} x={601} y={29} w={197} h={394} slot={86} />
    {modes.map((mode, index) => <T key={mode} {...p} x={614} y={52 + index * 27} text={mode} slot={87} size={14} />)}
    <SelectionFrame {...p} x={604} y={33} w={191} h={27} slot={83} />
    <SelectionFrame {...p} x={601} y={31} w={197} h={392} slot={130} />
    <T {...p} x={700} y={459} text="Play Mode" slot={9} size={15} anchor="middle" />
    <path className="paintable" data-slot={9} d="M620 457 L633 457 L626.5 450 Z M767 450 L780 450 L773.5 457 Z" fill={paint(p.slots, 9)} onClick={() => p.pick(9)} />
  </>;
}

function ListPanel({ x, y = 30, w, h = 393, items, selected = 0, selectionSlot = 83, title, ...p }: ArtProps & { x: number; y?: number; w: number; h?: number; items: string[]; selected?: number; selectionSlot?: number; title?: string }) {
  const top = title ? y + 31 : y;
  const clipId = `${p.gradientId}-list-${x}-${y}-${w}`;
  return <>
    <R {...p} x={x} y={y} w={w} h={h} slot={86} />
    {title && <><R {...p} x={x + 2} y={y + 2} w={w - 4} h={27} slot={149} /><T {...p} x={x + w / 2} y={y + 22} text={title} slot={87} size={16} anchor="middle" /></>}
    <defs><clipPath id={clipId}><rect x={x + 3} y={top} width={w - 6} height={y + h - top} /></clipPath></defs>
    <g clipPath={`url(#${clipId})`}>
    {items.map((item, i) => <g key={`${i}-${item}`}>
      {i === selected && <SelectionFrame {...p} x={x + 4} y={top + 5 + i * 27} w={w - 8} h={24} slot={selectionSlot} />}
      <T {...p} x={x + 14} y={top + 23 + i * 27} text={item} slot={item.startsWith('/') && i !== selected ? 102 : 87} size={14} />
    </g>)}
    </g>
    {items.length > 9 && <><R {...p} x={x + w - 9} y={top + 5} w={4} h={h - (top - y) - 12} slot={89} /><R {...p} x={x + w - 9} y={top + 5} w={4} h={80} slot={85} /></>}
  </>;
}

function WaveShape({ x, y, w, h, seed = 0, ...p }: ArtProps & { x: number; y: number; w: number; h: number; seed?: number }) {
  const mid = y + h / 2;
  const points = Array.from({ length: 160 }, (_, i) => {
    const phase = i / 160;
    const envelope = 0.15 + 0.8 * Math.abs(Math.sin(phase * 12 + seed) * Math.cos(phase * 4.5 + seed));
    const amp = Math.abs(Math.sin(i * 1.73 + seed) * Math.cos(i * 0.41)) * envelope * h * 0.46;
    return `${x + phase * w},${mid - amp}`;
  });
  const upper = points.join(' ');
  const lower = [...points].reverse().map((point) => { const [px, py] = point.split(',').map(Number); return `${px},${2 * mid - py}`; }).join(' ');
  return <>
    <R {...p} x={x} y={y} w={w} h={h} slot={247} />
    <polygon className="paintable" data-slot={244} points={`${upper} ${lower}`} fill={paint(p.slots, 244)} onClick={(event) => { event.stopPropagation(); p.pick(244); }} />
  </>;
}

function FileScreen(p: ArtProps) {
  return <>
    <Chrome {...p} title="File - Open Project" subtitle="Demo Project" buttons={['Enter', '', 'Delete', 'New Folder', '', '', 'Cancel', 'Open']} />
    <ListPanel {...p} x={1} w={297} items={['/..', '/Demo Project', '/Breakbeats', '/Ambient', '/Sketchbook', '/Drum Studies']} selected={1} selectionSlot={130} title="SD Card" />
    <ListPanel {...p} x={301} w={297} items={['Demo Project', 'Breakbeats', 'Ambient']} selected={0} title="Recent" />
    <R {...p} x={601} y={30} w={198} h={393} slot={50} />
    <SelectionFrame {...p} x={1} y={31} w={297} h={392} slot={130} />
  </>;
}

function Song(p: ArtProps) {
  return <>
    <Chrome {...p} title="Song" subtitle="0m:51s  >  P13.029" buttons={['Stop', 'Stop', 'Undo', 'Add', 'Delete', 'Pattern', '', 'Tempo']} values={['', '', '', 'Empty Slot', 'Slot', '', '', 'BPM']} />
    <R {...p} x={1} y={30} w={697} h={393} slot={202} />
    {Array.from({ length: 12 }, (_, c) => <g key={c}>
      <text className="paintable" data-slot={199} transform={`translate(${29 + c * 40} 88) rotate(-90)`} fill={paint(p.slots, 199)} fontSize="14" onClick={() => p.pick(199)}>{c < 8 ? `Track ${c + 1}` : `Midi ${c + 1}`}</text>
      {Array.from({ length: 12 }, (_, r) => <R key={r} {...p} x={5 + c * 40} y={94 + r * 27} w={38} h={24} slot={(c < Math.min(2 + Math.floor(r / 2), 7) && (c !== 6 || r > 7)) ? 199 : 207} rx={2} />)}
    </g>)}
    <R {...p} x={484} y={91} w={207} h={325} slot={202} />
    <T {...p} x={587} y={65} text="Pattern" slot={199} size={15} anchor="middle" />
    {Array.from({ length: 12 }, (_, r) => <T key={r} {...p} x={522} y={111 + r * 27} text={String(r + 1)} slot={199} size={14} />)}
    <SelectionFrame {...p} x={490} y={91} w={200} h={27} slot={208} />
    <Fader {...p} x={701} y={29} width={97} height={394} level={0.28} fillSlot={199} />
  </>;
}

function SampleEditor(p: ArtProps) {
  const effects = ['Normalizer', 'Crop', 'Reverse', 'Amplifier', 'Overdrive', 'Delay', 'Bitcrusher', 'Chorus', 'Flanger', 'Equalizer', 'Limiter', 'Compressor', 'WT Smoother'];
  return <>
    <Chrome {...p} title="Sample Editor 1/2" subtitle="1. ST Drum Loop" buttons={['Start', 'End', 'Zoom', 'Undo', '', 'Select', 'Effect', '']} values={['0.000s', '1.936s', '1.14', '', '', 'effect', '', '']} />
    <R {...p} x={0} y={30} w={600} h={393} slot={162} />
    <WaveShape {...p} x={2} y={116} w={598} h={108} seed={1.3} />
    <WaveShape {...p} x={2} y={226} w={598} h={110} seed={2.1} />
    <R {...p} x={601} y={30} w={197} h={393} slot={86} />
    {effects.map((effect, i) => <g key={effect}>
      {i === 12 && <SelectionFrame {...p} x={604} y={355} w={187} h={30} slot={83} />}
      <T {...p} x={614} y={52 + i * 27} text={effect} slot={i === 12 ? 87 : 101} size={14} />
    </g>)}
    <R {...p} x={789} y={37} w={4} h={381} slot={89} /><R {...p} x={789} y={37} w={4} h={73} slot={85} />
    <SelectionFrame {...p} x={201} y={432} w={99} h={46} slot={130} />
  </>;
}

function Config(p: ArtProps) {
  const categories = ['Project Settings', 'General', 'MIDI', 'Metronome', 'Firmware', 'Info'];
  const options = [['Firmware update', ''], ['Current version', 'OpenTracker 1.9.3'], ['Build', 'Oct 16 2026'], ['Reset config', '']];
  return <>
    <Chrome {...p} title="Config" subtitle="0m:16s  >  P13.058" buttons={['Firmware', '', 'Current version', '', '', '', 'Back', 'Enter']} />
    <R {...p} x={1} y={30} w={197} h={393} slot={86} />
    {categories.map((category, i) => <g key={category}>
      {i === 4 && <SelectionFrame {...p} x={3} y={140} w={193} h={29} slot={83} />}
      <T {...p} x={15} y={52 + i * 27} text={category} slot={87} size={14} />
    </g>)}
    <R {...p} x={201} y={31} w={398} h={392} slot={86} />
    {options.map(([label, value], i) => <g key={label}>
      {i === 1 && <SelectionFrame {...p} x={207} y={60} w={385} h={29} slot={83} />}
      <T {...p} x={215} y={52 + i * 27} text={label} slot={87} size={14} />
      {value && <T {...p} x={586} y={52 + i * 27} text={value} slot={87} size={14} anchor="end" />}
    </g>)}
    <R {...p} x={601} y={30} w={197} h={393} slot={50} />
    <SelectionFrame {...p} x={201} y={31} w={398} h={392} slot={130} />
  </>;
}

function SampleRecorder(p: ArtProps) {
  return <>
    <Chrome {...p} title="Sample Recorder" subtitle="0m:00s  ||  P1.001" buttons={['Source', '', '', 'Metronome', 'Level', 'Gain', 'Monitor', 'Record']} values={['Line in', '', '', '', '', '50', 'On', '']} />
    <ListPanel {...p} x={2} y={30} w={96} items={['Line in', 'Radio', 'Mic LG', 'Mic HG']} />
    <R {...p} x={102} y={30} w={297} h={393} slot={50} />
    <Fader {...p} x={402} y={30} width={96} height={393} level={0.04} fillSlot={270} />
    <Fader {...p} x={502} y={30} width={96} height={393} level={0.5} fillSlot={73} />
    <ListPanel {...p} x={602} y={30} w={96} items={['On', 'Off']} />
    <R {...p} x={702} y={30} w={96} h={393} slot={50} />
    <R {...p} x={702} y={430} w={96} h={48} slot={211} />
    <SelectionFrame {...p} x={1} y={31} w={98} h={392} slot={130} />
  </>;
}

function Synth(p: ArtProps) {
  const labels = ['Model', 'Polyphony', 'Gain', 'Panning', 'Reverb Send', 'Delay Send', 'Load Patch', 'Edit Patch'];
  const values = ['ACD', '1', '-7.00 dB', '0', '-38.00 dB', '-32.00 dB', '', ''];
  return <>
    <Chrome {...p} title="Synth Parameters 1/2" subtitle="Synth 2 (ACD)" buttons={labels} values={values} />
    <ListPanel {...p} x={2} y={30} w={96} items={['ACD', 'FAT', 'VAP', 'WTFM', 'PERC']} />
    {[0.14, 0.84, 0.5, 0.05, 0.25].map((level, i) => <Fader key={i} {...p} x={102 + i * 100} y={30} width={96} height={393} level={level} fillSlot={73} />)}
    <R {...p} x={602} y={30} w={196} h={393} slot={50} />
    <T {...p} x={615} y={53} text="Patch:" slot={9} size={15} />
    <T {...p} x={615} y={80} text="OT IDM Acid" slot={87} size={17} />
    <SelectionFrame {...p} x={1} y={31} w={98} h={392} slot={130} />
  </>;
}

function Games(p: ArtProps) {
  return <>
    <Chrome {...p} title="Games" buttons={['', '', '', 'Up', 'Down', '', 'Start', 'Back']} values={['', '', '', '', '', '', 'Game', '']} />
    <R {...p} x={0} y={30} w={247} h={393} slot={50} />
    <ListPanel {...p} x={250} y={30} w={297} items={['Zooming Secretary', 'Demo Game', 'Puzzle Mix', 'Tracker Snake']} selected={0} />
    <R {...p} x={550} y={30} w={249} h={393} slot={50} />
    <T {...p} x={655} y={74} text="In game:" slot={87} size={15} />
    <T {...p} x={655} y={96} text="Play = Start" slot={87} size={15} />
    <T {...p} x={655} y={118} text="Rec = Select" slot={87} size={15} />
  </>;
}

function Master(p: ArtProps) {
  const labels = ['Volume', 'Reverb', 'Delay', 'Limiter', 'EQ', 'Saturation', 'Bass Boost', 'Space'];
  const values = ['75', '', '', '', '', 'Hard Clip', '30', '2'];
  return <>
    <Chrome {...p} title="Master 1/3" buttons={labels} values={values} />
    <Fader {...p} x={2} y={30} width={96} height={393} level={0.75} fillSlot={73} />
    <R {...p} x={102} y={30} w={397} h={393} slot={50} />
    <ListPanel {...p} x={502} y={30} w={96} items={['Hard Clip', 'Soft Clip', 'Saturate', 'Overdrive', 'Drive+']} />
    <Fader {...p} x={602} y={30} width={96} height={393} level={0.3} fillSlot={73} />
    <Fader {...p} x={702} y={30} width={96} height={393} level={0.25} fillSlot={73} />
    <SelectionFrame {...p} x={1} y={31} w={98} h={392} slot={130} />
  </>;
}

function Resample(p: ArtProps) {
  const labels = ['From', 'Length', 'Slices', 'Into', 'Place on', 'Gain', 'Monitor', 'Record'];
  const values = ['Master', '4 bars', 'Per beat', 'New', 'Track 1', '', '', ''];
  const lists = [
    ['Master', 'Track 1', 'Track 2', 'Track 3', 'Track 4', 'Line in'],
    ['Manual', '1 bar', '2 bars', '4 bars', '8 bars', '16 bars'],
    ['Off', 'Per beat', 'Per step', 'Beat hits', 'Step hits'],
    ['New', '1 Kick', '2 Snare', '3 Hats', '4 Bass'],
    ['Off', 'Track 1', 'Track 2', 'Track 3', 'Track 4'],
  ];
  return <>
    <Chrome {...p} title="Resample" buttons={labels} values={values} />
    {lists.map((items, i) => <ListPanel key={i} {...p} x={2 + i * 100} y={30} w={96} items={items} selected={[0, 3, 1, 0, 1][i]} />)}
    <R {...p} x={502} y={30} w={196} h={393} slot={50} />
    <R {...p} x={702} y={30} w={96} h={393} slot={50} />
    <T {...p} x={750} y={61} text="Ready" slot={87} size={15} anchor="middle" />
    <T {...p} x={750} y={87} text="-> Instr 5" slot={66} size={13} anchor="middle" />
    <SelectionFrame {...p} x={1} y={31} w={98} h={392} slot={130} />
  </>;
}

const art: Record<ScreenId, (props: ArtProps) => ReactNode> = {
  file: FileScreen, loader: Loader, pattern: Pattern, playback: Playback, song: Song,
  instrument: Instrument, editor: SampleEditor, config: Config, recorder: SampleRecorder,
  performance: Performance, synth: Synth, games: Games, master: Master, resample: Resample, mixer: Mixer,
};

function ScreenCard({ id, title, ariaTitle = title, slots, selectedSlot, focused, jumpTarget, onFocus, onPickSlot }: ArtProps & { id: ScreenId; title: string; ariaTitle?: string; focused: boolean; jumpTarget?: SlotJump | null; onFocus: () => void; onPickSlot: (slot: number) => void }) {
  const Content = art[id];
  const specimen = focused && jumpTarget?.screenId === id && slotSpecimens[jumpTarget.slot];
  return <article className={`screen-card ${focused ? 'focused' : 'mini'}`} data-screen-id={id}>
    <button className="screen-card-head" onClick={onFocus} aria-pressed={focused}><span className="screen-led" /><span>{title}</span><span className="focus-text">{focused ? 'Focused' : 'Focus ↗'}</span></button>
    <div className="screen-frame" onClick={onFocus}>
      <svg viewBox="0 0 800 480" role="img" aria-label={`${ariaTitle} themed preview`}>
        <defs>
          <linearGradient id={`${id}-valuebar`}><stop stopColor="#000" stopOpacity="0.35" /><stop offset="1" stopColor="#000" stopOpacity="0" /></linearGradient>
          <linearGradient id={`${id}-fade`} x1="0" y1="0" x2="0" y2="1"><stop stopColor={paint(slots, 50)} /><stop offset="1" stopColor={paint(slots, 0)} /></linearGradient>
          <linearGradient id={`${id}-cursor`}><stop stopColor={paint(slots, 175)} /><stop offset="0.35" stopColor={paint(slots, 187)} /><stop offset="0.65" stopColor={paint(slots, 187)} /><stop offset="1" stopColor={paint(slots, 175)} /></linearGradient>
        </defs>
        <Content slots={slots} selectedSlot={selectedSlot} gradientId={id} pick={(slot) => { onPickSlot(slot); onFocus(); }} />
      </svg>
    </div>
    {specimen && <button className="slot-specimen" data-slot={jumpTarget.slot} onClick={() => onPickSlot(jumpTarget.slot)}>
      <span className="slot-specimen-swatch" style={{ backgroundColor: paint(slots, jumpTarget.slot) }} />
      <span><strong>slot.{jumpTarget.slot} · {specimen.label}</strong><small>Color specimen; this screen does not draw the optional background.</small></span>
    </button>}
  </article>;
}

export default function PreviewGallery({ slots, focus, selectedSlot, jumpTarget, onFocus, onPickSlot }: { slots: number[]; focus: string; selectedSlot: number; jumpTarget?: SlotJump | null; onFocus: (id: string) => void; onPickSlot: (slot: number) => void }) {
  const galleryRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!jumpTarget || jumpTarget.screenId !== focus) return;
    const card = galleryRef.current?.querySelector<HTMLElement>('.screen-card.focused');
    const element = card?.querySelector<Element>(`[data-slot="${jumpTarget.slot}"]`);
    if (!element) return;
    element.classList.add('slot-jump-target');
    card?.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    return () => element.classList.remove('slot-jump-target');
  }, [jumpTarget, focus]);
  const active = screens.find((screen) => screen.id === focus) ?? screens[1];
  return <div className="preview-gallery" ref={galleryRef}>
    <nav className="screen-picker" aria-label="Choose screen preview">{screens.map((screen) =>
      <button key={screen.id} type="button" aria-pressed={screen.id === active.id} onClick={() => onFocus(screen.id)}>{screen.short}</button>)}</nav>
    <ScreenCard key={active.id} id={active.id} title={active.title} slots={slots} selectedSlot={selectedSlot} focused jumpTarget={jumpTarget} onFocus={() => onFocus(active.id)} onPickSlot={onPickSlot} pick={onPickSlot} />
    <div className="thumbnail-grid">{screens.filter((screen) => screen.id !== active.id).map((screen) => <ScreenCard key={screen.id} id={screen.id} title={screen.short} ariaTitle={screen.title} slots={slots} selectedSlot={selectedSlot} focused={false} onFocus={() => onFocus(screen.id)} onPickSlot={onPickSlot} pick={onPickSlot} />)}</div>
  </div>;
}
