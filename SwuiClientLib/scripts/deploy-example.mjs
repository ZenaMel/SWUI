// deploy-example.mjs — copies built artifacts into the ADA example project's Content/UI/
// Run via: pnpm build:example
// This is developer-convenience only; it is not part of the plugin's public API.
import { copyFileSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const root      = join(__dirname, '..');
// SwuiClientLib → SimpleWebUI → Plugins → <GameRoot>
const contentUI = join(root, '..', '..', '..', 'Content', 'UI');

const copies = [
  ['dist/swui.js',        'swui.js'],
  ['dist/swui.js.map',    'swui.js.map'],
  ['src/layout.css',      'swui-layout.css'],
];

for (const [src, dst] of copies) {
  try {
    copyFileSync(join(root, src), join(contentUI, dst));
    console.log(`  copied  ${src} → Content/UI/${dst}`);
  } catch (e) {
    console.warn(`  SKIP    ${src} (${e.message})`);
  }
}
