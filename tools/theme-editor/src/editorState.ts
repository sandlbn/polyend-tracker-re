import { baseTheme, importTheme, themeData, type BaseName, type ThemeDocument } from './theme';

export type Draft = {
  id: string;
  name: string;
  theme: ThemeDocument;
  past: ThemeDocument[];
  future: ThemeDocument[];
};
export type EditorState = { activeId: string; drafts: Record<string, Draft> };
export type CatalogItem = { id: string; name: string; kind: 'stock' | 'example' };
export const STORAGE_KEY = 'opentracker-theme-editor:v1';

export const catalog: CatalogItem[] = [
  ...(['original', 'monochrome', 'anniversary'] as BaseName[]).map((name) => ({
    id: `stock:${name}`, name: name[0].toUpperCase() + name.slice(1), kind: 'stock' as const,
  })),
  ...themeData.presets.map(({ name }, index) => ({ id: `example:${index}`, name, kind: 'example' as const })),
];

export function freshDraft(id: string): Draft {
  const item = catalog.find((entry) => entry.id === id);
  if (!item) throw new Error(`Unknown preset: ${id}`);
  const theme = item.kind === 'stock'
    ? baseTheme(id.slice(6) as BaseName)
    : importTheme(themeData.presets[Number(id.slice(8))].text).theme;
  return { id, name: item.name, theme, past: [], future: [] };
}

export function freshEditorState(): EditorState {
  const draft = freshDraft('stock:original');
  return { activeId: draft.id, drafts: { [draft.id]: draft } };
}

function validTheme(value: unknown): value is ThemeDocument {
  if (!value || typeof value !== 'object') return false;
  const theme = value as ThemeDocument;
  return ['original', 'monochrome', 'anniversary'].includes(theme.base)
    && Array.isArray(theme.slots) && theme.slots.length === 277
    && theme.slots.every((slot, index) => Number.isInteger(slot) && slot >= 0 && slot <= (index < 273 ? 0xffffff : 1));
}

export function loadEditorState(storage: Pick<Storage, 'getItem'>): EditorState {
  try {
    const saved = JSON.parse(storage.getItem(STORAGE_KEY) ?? 'null') as {
      version?: number; activeId?: string; drafts?: Array<{ id: string; name: string; theme: ThemeDocument }>;
    } | null;
    if (saved?.version !== 1 || !Array.isArray(saved.drafts)) return freshEditorState();
    const drafts: Record<string, Draft> = {};
    for (const item of saved.drafts) {
      if (typeof item?.id !== 'string' || typeof item.name !== 'string' || !validTheme(item.theme)) continue;
      drafts[item.id] = { ...item, past: [], future: [] };
    }
    if (!saved.activeId || !drafts[saved.activeId]) return freshEditorState();
    return { activeId: saved.activeId, drafts };
  } catch {
    return freshEditorState();
  }
}

export function saveEditorState(storage: Pick<Storage, 'setItem'>, state: EditorState): void {
  const drafts = Object.values(state.drafts).map(({ id, name, theme }) => ({ id, name, theme }));
  storage.setItem(STORAGE_KEY, JSON.stringify({ version: 1, activeId: state.activeId, drafts }));
}

export function selectDraft(state: EditorState, id: string): EditorState {
  if (id === state.activeId) return state;
  const draft = state.drafts[id] ?? freshDraft(id);
  return { activeId: id, drafts: { ...state.drafts, [id]: draft } };
}

export function addImportedDraft(state: EditorState, id: string, name: string, theme: ThemeDocument): EditorState {
  const draft: Draft = { id, name, theme: { base: theme.base, slots: [...theme.slots] }, past: [], future: [] };
  return { activeId: id, drafts: { ...state.drafts, [id]: draft } };
}

export function renameActiveDraft(state: EditorState, name: string): EditorState {
  const draft = state.drafts[state.activeId];
  return { ...state, drafts: { ...state.drafts, [draft.id]: { ...draft, name } } };
}

export function editActiveSlots(state: EditorState, changes: ReadonlyArray<readonly [number, number]>): EditorState {
  const draft = state.drafts[state.activeId];
  const slots = [...draft.theme.slots];
  for (const [index, value] of changes) slots[index] = value;
  if (slots.every((value, index) => value === draft.theme.slots[index])) return state;
  const next: Draft = {
    ...draft,
    theme: { ...draft.theme, slots },
    past: [...draft.past.slice(-99), draft.theme],
    future: [],
  };
  return { ...state, drafts: { ...state.drafts, [draft.id]: next } };
}

export function stepHistory(state: EditorState, direction: 'undo' | 'redo'): EditorState {
  const draft = state.drafts[state.activeId];
  const source = direction === 'undo' ? draft.past : draft.future;
  if (!source.length) return state;
  const nextTheme = source[source.length - 1];
  const next: Draft = direction === 'undo'
    ? { ...draft, theme: nextTheme, past: source.slice(0, -1), future: [...draft.future, draft.theme] }
    : { ...draft, theme: nextTheme, past: [...draft.past, draft.theme], future: source.slice(0, -1) };
  return { ...state, drafts: { ...state.drafts, [draft.id]: next } };
}
