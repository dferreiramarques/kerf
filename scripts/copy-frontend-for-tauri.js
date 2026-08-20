// Copia só os ficheiros estáticos que o Kerf precisa para correr dentro do
// Tauri para src-tauri/dist/. Isto existe porque "frontendDist" não pode
// apontar para a raiz do repo (ia embutir recursivamente src-tauri/target,
// node_modules e .git dentro do próprio binário). Não é um build step real —
// é só uma cópia de ficheiros, sem bundler nem transpilação.
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const DIST = path.join(ROOT, 'src-tauri', 'dist');

// Tauri espera um index.html dentro de frontendDist; o PWA vive em
// kerf.html na raiz do repo (index.html aí é a landing page de marketing).
const RENAMES = { 'kerf.html': 'index.html' };
const FILES = ['kerf.html', 'manifest.json'];
const DIRS = ['vendor'];

fs.rmSync(DIST, { recursive: true, force: true });
fs.mkdirSync(DIST, { recursive: true });

for (const file of FILES) {
    const destName = RENAMES[file] || file;
    fs.copyFileSync(path.join(ROOT, file), path.join(DIST, destName));
}

for (const dir of DIRS) {
    fs.cpSync(path.join(ROOT, dir), path.join(DIST, dir), { recursive: true });
}

console.log(`[copy-frontend-for-tauri] copiado para ${DIST}`);
