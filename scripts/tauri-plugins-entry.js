// Entry point para gerar vendor/tauri-plugins.js (ver scripts/build-tauri-plugins.js).
// @tauri-apps/plugin-dialog e @tauri-apps/plugin-fs só existem como ES module — sem
// build UMD/global — por isso este ficheiro é bundlado com esbuild uma única vez e o
// resultado é vendorizado, tal como o vendor/jszip.min.js.
import { open, save } from '@tauri-apps/plugin-dialog';
import { readFile, writeFile } from '@tauri-apps/plugin-fs';

window.__TAURI_PLUGINS__ = {
    dialog: { open, save },
    fs: { readFile, writeFile }
};
