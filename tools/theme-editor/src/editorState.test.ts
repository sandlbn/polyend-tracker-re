import { describe, expect, it } from 'vitest';
import {
  addImportedDraft, editActiveSlots, freshEditorState, loadEditorState, saveEditorState,
  selectDraft, stepHistory, STORAGE_KEY,
} from './editorState';
import { baseTheme } from './theme';

describe('drafts and history', () => {
  it('keeps a separate draft for each preset', () => {
    let state = freshEditorState();
    state = editActiveSlots(state, [[130, 0x123456]]);
    state = selectDraft(state, 'stock:monochrome');
    state = editActiveSlots(state, [[130, 0x654321]]);
    state = selectDraft(state, 'stock:original');
    expect(state.drafts[state.activeId].theme.slots[130]).toBe(0x123456);
    state = selectDraft(state, 'stock:monochrome');
    expect(state.drafts[state.activeId].theme.slots[130]).toBe(0x654321);
  });

  it('treats a role-wide edit as one undo step', () => {
    let state = freshEditorState();
    const before = state.drafts[state.activeId].theme.slots.slice(176, 180);
    state = editActiveSlots(state, [[176, 0x111111], [177, 0x111111], [178, 0x111111]]);
    expect(state.drafts[state.activeId].past).toHaveLength(1);
    state = stepHistory(state, 'undo');
    expect(state.drafts[state.activeId].theme.slots.slice(176, 180)).toEqual(before);
    state = stepHistory(state, 'redo');
    expect(state.drafts[state.activeId].theme.slots.slice(176, 179)).toEqual([0x111111, 0x111111, 0x111111]);
  });

  it('undoes flags and clears redo after new edit', () => {
    let state = freshEditorState();
    state = editActiveSlots(state, [[273, 0]]);
    state = stepHistory(state, 'undo');
    expect(state.drafts[state.activeId].theme.slots[273]).toBe(1);
    state = editActiveSlots(state, [[274, 0]]);
    expect(stepHistory(state, 'redo')).toBe(state);
  });

  it('recovers every draft and selected theme from browser storage', () => {
    const values = new Map<string, string>();
    const storage = {
      getItem: (key: string) => values.get(key) ?? null,
      setItem: (key: string, value: string) => { values.set(key, value); },
    };
    let state = freshEditorState();
    state = editActiveSlots(state, [[130, 0x123456]]);
    state = addImportedDraft(state, 'import:fixture', 'Fixture', baseTheme('anniversary'));
    state = editActiveSlots(state, [[273, 1]]);
    saveEditorState(storage, state);
    expect(values.has(STORAGE_KEY)).toBe(true);
    const restored = loadEditorState(storage);
    expect(restored.activeId).toBe('import:fixture');
    expect(restored.drafts['stock:original'].theme.slots[130]).toBe(0x123456);
    expect(restored.drafts['import:fixture'].theme.slots[273]).toBe(1);
    expect(restored.drafts['import:fixture'].past).toEqual([]);
  });

  it('rejects corrupt saved state', () => {
    const restored = loadEditorState({ getItem: () => '{broken' });
    expect(restored.activeId).toBe('stock:original');
  });
});
