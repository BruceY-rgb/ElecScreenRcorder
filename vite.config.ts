import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import electron from 'vite-plugin-electron';
import renderer from 'vite-plugin-electron-renderer';
import { resolve } from 'path';

const rootDir = __dirname;

export default defineConfig({
  plugins: [
    react(),
    electron([
      {
        entry: resolve(rootDir, 'src/main/index.ts'),
        onstart(options) {
          options.startup();
        },
        vite: {
          build: {
            outDir: resolve(rootDir, 'dist/main'),
            rollupOptions: {
              external: ['electron', 'electron-store'],
            },
          },
        },
      },
      {
        entry: resolve(rootDir, 'src/preload/index.ts'),
        onstart(options) {
          options.reload();
        },
        vite: {
          build: {
            outDir: resolve(rootDir, 'dist/preload'),
            rollupOptions: {
              external: ['electron'],
            },
          },
        },
      },
    ]),
    renderer(),
  ],
  root: resolve(rootDir, 'src/renderer'),
  base: './',
  build: {
    outDir: resolve(rootDir, 'dist/renderer'),
    emptyOutDir: true,
    rollupOptions: {
      input: {
        main: resolve(rootDir, 'src/renderer/index.html'),
        overlay: resolve(rootDir, 'src/renderer/overlay.html'),
      },
    },
  },
  resolve: {
    alias: {
      '@': resolve(rootDir, 'src/renderer/src'),
    },
  },
});
