// Copia só os ficheiros estáticos que o Kerf precisa para correr dentro do
// Tauri para src-tauri/dist/. Isto existe porque "frontendDist" não pode
// apontar para a raiz do repo (ia embutir recursivamente src-tauri/target,
// node_modules e .git dentro do próprio binário). Não é um build step real —
// é só uma cópia de ficheiros, sem bundler nem transpilação.
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const DIST = path.join(ROOT, 'src-tauri', 'dist');

const FILES = ['index.html', 'manifest.json'];
const DIRS = ['vendor'];

fs.rmSync(DIST, { recursive: true, force: true });
fs.mkdirSync(DIST, { recursive: true });

for (const file of FILES) {
    fs.copyFileSync(path.join(ROOT, file), path.join(DIST, file));
}

for (const dir of DIRS) {
    fs.cpSync(path.join(ROOT, dir), path.join(DIST, dir), { recursive: true });
}

console.log(`[copy-frontend-for-tauri] copiado para ${DIST}`);
