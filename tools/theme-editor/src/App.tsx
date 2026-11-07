import { useEffect, useMemo, useState, type ChangeEvent } from 'react';
import {
  addImportedDraft, catalog, editActiveSlots, freshEditorState, loadEditorState,
  renameActiveDraft, saveEditorState, selectDraft, stepHistory, type EditorState,
} from './editorState';
import { COLOR_COUNT, colorHex, exportTheme, FLAG_NAMES, importTheme, parseColor, themeData, validateThemeName } from './theme';
import PreviewGallery, { slotSpecimens, type SlotJump } from './PreviewGallery';

function initialState(): EditorState {
  try { return loadEditorState(localStorage); } catch { return freshEditorState(); }
}

export default function App() {
  const [state, setState] = useState<EditorState>(initialState);
  const [focus, setFocus] = useState('pattern');
  const [selectedSlot, setSelectedSlot] = useState(130);
  const [jumpTarget, setJumpTarget] = useState<SlotJump | null>(null);
  const [search, setSearch] = useState('');
  const [hexInput, setHexInput] = useState('');
  const [message, setMessage] = useState('');
  const draft = state.drafts[state.activeId];
  const slots = draft.theme.slots;
  const role = themeData.roles.find((item) => item.slots.includes(selectedSlot));
  const selectedValue = slots[selectedSlot];

  useEffect(() => {
    try { saveEditorState(localStorage, state); }
    catch { setMessage('Browser storage unavailable. Export theme before closing.'); }
  }, [state]);

  useEffect(() => { setHexInput(selectedSlot < COLOR_COUNT ? colorHex(selectedValue) : ''); }, [selectedSlot, selectedValue]);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (!(event.ctrlKey || event.metaKey) || (event.target instanceof HTMLElement && event.target.closest('input, textarea'))) return;
      if (event.key.toLowerCase() === 'z') {
        event.preventDefault();
        setState((current) => stepHistory(current, event.shiftKey ? 'redo' : 'undo'));
      } else if (event.key.toLowerCase() === 'y') {
        event.preventDefault();
        setState((current) => stepHistory(current, 'redo'));
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  const visibleRoles = useMemo(() => {
    const query = search.trim().toLowerCase();
    if (!query) return themeData.roles;
    return themeData.roles.filter((item) => item.name.includes(query) || item.slots.some((slot) =>
      String(slot) === query || `slot.${slot}`.includes(query)));
  }, [search]);
  const visibleFlags = FLAG_NAMES.map((name, index) => ({ name, index: COLOR_COUNT + index }))
    .filter(({ name, index }) => !search.trim() || name.includes(search.toLowerCase()) || String(index) === search.trim());

  function changeSlot(index: number, value: number) {
    setState((current) => editActiveSlots(current, [[index, value]]));
  }

  function jumpToSlot(index: number) {
    setSelectedSlot(index);
    const cards = Array.from(document.querySelectorAll<HTMLElement>('.preview-gallery .screen-card[data-screen-id]'));
    const containsSlot = (card: HTMLElement) => Boolean(card.querySelector(`[data-slot="${index}"]`));
    const current = cards.find((card) => card.dataset.screenId === focus);
    const target = current && containsSlot(current) ? current : cards.find(containsSlot);
    if (!target?.dataset.screenId) {
      const specimen = slotSpecimens[index];
      if (specimen) {
        setFocus(specimen.screenId);
        setJumpTarget({ slot: index, screenId: specimen.screenId });
        setMessage('');
        return;
      }
      setJumpTarget(null);
      setMessage(`Slot ${index} is not shown in the current screen previews.`);
      return;
    }
    setFocus(target.dataset.screenId);
    setJumpTarget({ slot: index, screenId: target.dataset.screenId });
    setMessage('');
  }

  function commitHex() {
    const parsed = parseColor(hexInput.trim());
    if (parsed === undefined) {
      setMessage('Use #RRGGBB or #RGB for color.');
      setHexInput(colorHex(selectedValue));
      return;
    }
    changeSlot(selectedSlot, parsed);
    setMessage('');
  }

  async function onImport(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (!file) return;
    const name = file.name.replace(/\.txt$/i, '');
    const error = validateThemeName(name);
    if (error) { setMessage(`${file.name}: ${error}`); return; }
    try {
      const result = importTheme(await file.text());
      const id = `import:${crypto.randomUUID()}`;
      setState((current) => addImportedDraft(current, id, name, result.theme));
      setMessage(result.badLines.length || result.guarded
        ? `Imported with ${result.badLines.length} skipped line(s)${result.badLines.length ? `: ${result.badLines.join(', ')}` : ''}${result.guarded ? '; unreadable text colors restored by firmware rules' : ''}.`
        : `Imported ${file.name}.`);
    } catch (error) {
      setMessage(error instanceof Error ? error.message : 'Theme import failed.');
    }
  }

  function onExport() {
    try {
      const text = exportTheme(draft.name, draft.theme);
      const url = URL.createObjectURL(new Blob([text], { type: 'text/plain;charset=utf-8' }));
      const link = document.createElement('a');
      link.href = url;
      link.download = `${draft.name}.txt`;
      link.click();
      window.setTimeout(() => URL.revokeObjectURL(url), 1000);
      setMessage(`Downloaded ${link.download}. Copy into Tracker /Themes folder.`);
    } catch (error) {
      setMessage(error instanceof Error ? error.message : 'Theme export failed.');
    }
  }

  return <div className="app">
    <header className="topbar">
      <div className="brand"><span className="brand-mark">OT</span><div><strong>Theme Editor</strong><small>OpenTracker · 277 slots</small></div></div>
      <div className="top-actions">
        <label className="field compact">Theme
          <select value={state.activeId} onChange={(event) => setState((current) => selectDraft(current, event.target.value))}>
            <optgroup label="Built-in">{catalog.filter((item) => item.kind === 'stock').map((item) => <option key={item.id} value={item.id}>{item.name}</option>)}</optgroup>
            <optgroup label="Repo examples">{catalog.filter((item) => item.kind === 'example').map((item) => <option key={item.id} value={item.id}>{item.name}</option>)}</optgroup>
            {Object.values(state.drafts).some((item) => item.id.startsWith('import:')) && <optgroup label="Imported">{Object.values(state.drafts).filter((item) => item.id.startsWith('import:')).map((item) => <option key={item.id} value={item.id}>{item.name}</option>)}</optgroup>}
          </select>
        </label>
        <label className="field compact">Export name
          <input value={draft.name} maxLength={20} onChange={(event) => setState((current) => renameActiveDraft(current, event.target.value))} />
        </label>
        <label className="button file-button">Import .txt<input type="file" accept=".txt,text/plain" onChange={onImport} /></label>
        <button className="button primary" onClick={onExport}>Download .txt</button>
      </div>
    </header>

    <main className="workspace">
      <aside className="inspector">
        <div className="inspector-header"><div><h1>Colors & flags</h1><p>Click to select; double-click a slot to find it.</p></div><span className="count">277</span></div>
        <div className="selected-editor">
            <div className="selected-heading"><span>Selected</span><strong title="Double-click to find this slot" onDoubleClick={() => jumpToSlot(selectedSlot)}>slot.{selectedSlot}</strong></div>
          <div className="selected-name">{selectedSlot < COLOR_COUNT ? role?.name.replaceAll('_', ' ') : FLAG_NAMES[selectedSlot - COLOR_COUNT].replaceAll('_', ' ')}</div>
          {selectedSlot < COLOR_COUNT ? <div className="color-controls">
            <input aria-label={`Color for slot ${selectedSlot}`} type="color" value={colorHex(selectedValue)} onChange={(event) => changeSlot(selectedSlot, parseColor(event.target.value)!)} />
            <input aria-label="Hex color" value={hexInput} onChange={(event) => setHexInput(event.target.value)} onBlur={commitHex} onKeyDown={(event) => { if (event.key === 'Enter') event.currentTarget.blur(); }} />
          </div> : <label className="flag-control"><input type="checkbox" checked={Boolean(selectedValue)} onChange={(event) => changeSlot(selectedSlot, event.target.checked ? 1 : 0)} /> Enabled</label>}
          {role && <label className="bulk-control">Set all {role.slots.length} “{role.name}” slots
            <input aria-label={`Set all ${role.name} slots`} type="color" value={colorHex(slots[role.slots[0]])} onChange={(event) => {
              const value = parseColor(event.target.value)!;
              setState((current) => editActiveSlots(current, role.slots.map((index) => [index, value])));
            }} />
          </label>}
        </div>
        <div className="history-row"><button disabled={!draft.past.length} onClick={() => setState((current) => stepHistory(current, 'undo'))}>Undo</button><button disabled={!draft.future.length} onClick={() => setState((current) => stepHistory(current, 'redo'))}>Redo</button><span>{draft.theme.base} base</span></div>
        <input className="search" aria-label="Search roles or slots" placeholder="Search role or slot number…" value={search} onChange={(event) => setSearch(event.target.value)} />
        <div className="slot-list">
          {visibleRoles.map((item) => <section className="role-group" key={item.name}>
            <div className="role-title"><span>{item.name.replaceAll('_', ' ')}</span><span>{item.slots.length}</span></div>
            <div className="role-slots">{item.slots.filter((index) => !search.trim() || item.name.includes(search.toLowerCase()) || String(index) === search.trim() || `slot.${index}`.includes(search.toLowerCase())).map((index) => <button key={index} className={`slot-row ${selectedSlot === index ? 'active' : ''}`} onClick={() => { setSelectedSlot(index); setJumpTarget(null); }} onDoubleClick={() => jumpToSlot(index)}>
              <span className="swatch" style={{ backgroundColor: colorHex(slots[index]) }} /><span>slot.{index}</span><code>{colorHex(slots[index])}</code>
            </button>)}</div>
          </section>)}
          {visibleFlags.length > 0 && <section className="role-group"><div className="role-title"><span>flags</span><span>4</span></div>{visibleFlags.map(({ name, index }) => <button key={name} className={`slot-row ${selectedSlot === index ? 'active' : ''}`} onClick={() => { setSelectedSlot(index); setJumpTarget(null); }} onDoubleClick={() => jumpToSlot(index)}><span className={`flag-dot ${slots[index] ? 'on' : ''}`} /><span>slot.{index} · {name}</span><code>{slots[index] ? 'on' : 'off'}</code></button>)}</section>}
          {!visibleRoles.length && !visibleFlags.length && <p className="empty">No slots match search.</p>}
        </div>
      </aside>

      <section className="preview-area" aria-label="Screen previews">
        <div className="preview-intro"><div><h2>Screen previews</h2><p>Pick screen to focus. Click colored part to select its theme slot.</p></div><span>800 × 480 reference</span></div>
        <PreviewGallery slots={slots} focus={focus} jumpTarget={jumpTarget} onFocus={(id) => { setFocus(id); setJumpTarget(null); }} selectedSlot={selectedSlot} onPickSlot={(index) => { setSelectedSlot(index); setJumpTarget(null); }} />
      </section>
    </main>
    {message && <div role="status" className="toast" onClick={() => setMessage('')}>{message}</div>}
  </div>;
}
