import { defineConfig } from 'vite';
import dts from 'vite-plugin-dts';
import { resolve } from 'path';

export default defineConfig({
  build: {
    lib: {
      entry:   resolve(__dirname, 'src/swui.ts'),
      name:    'Swui',
      formats: ['iife', 'es'],
      // iife  → dist/swui.js        — <script src="swui.js"> usage
      // es    → dist/swui.esm.js    — import { Swui } from './swui.esm.js'
      fileName: (fmt) => fmt === 'iife' ? 'swui.js' : 'swui.esm.js',
    },
    outDir:    'dist',
    sourcemap: true,
  },
  plugins: [
    dts({
      rollupTypes:     true,
      insertTypesEntry: true,
    }),
  ],
});
