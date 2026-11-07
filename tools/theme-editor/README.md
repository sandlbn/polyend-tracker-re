# OpenTracker Theme Editor

Browser editor for the original Tracker's `/Themes/*.txt` files. Fifteen representative 800×480 screens show the current palette: the fourteen requested screen types plus Track Mixer 2/3. Preview drawings reproduce major firmware layouts; they do not run the C++ simulator.

## Run

Node 20+ and npm required.

```powershell
cd tools/theme-editor
npm install
npm run dev
```

Open **http://127.0.0.1:5173/**. This local dev server uses HTTP. If Firefox upgrades it to HTTPS, turn off HTTPS upgrading for this local address.

`npm test` runs parser and draft tests. `npm run build` checks TypeScript and creates a static build in `dist/`.

## GitHub Pages

In the GitHub repository, set **Settings → Pages → Build and deployment → Source** to **GitHub Actions**. The [Theme Editor Pages workflow](../../.github/workflows/theme-editor-pages.yml) tests and builds changes on pull requests and pushes. A push to the default branch publishes the editor at `https://<owner>.github.io/<repo>/`; the workflow sets Vite's asset base to the repository path. You can also run the workflow manually from the default branch.

## Edit

Choose stock theme or repo example; each keeps its own browser draft. Click a screen element or search slot list. Double-click a slot row to focus a screen that uses it and highlight its first matching element. Color input edits one slot; role input sets all slots in that role. Undo/redo buttons and Ctrl/Cmd+Z/Y cover color, flag, and role changes. Import a firmware `.txt` or download a canonical `.txt` to copy into `/Themes` on the SD card.

Theme data is generated from `src/core/ui/theme.cpp`, `src/core/ui/theme_file.cpp`, and `themes/*.txt` into ignored `.generated/` during dev, test, or build. Existing theme files are never written by the editor.
