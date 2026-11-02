import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  build: {
    outDir: '../backend/web',
    emptyOutDir: true,
    sourcemap: false,
    // Old WebView kernels (WeChat XWeb < 104, iOS Safari < 16.4) silently
    // drop range-syntax media queries like `@media (width<=900px)`, which
    // breaks the entire mobile layout. Lower them to legacy max-width form.
    cssTarget: 'chrome61'
  },
  server: {
    proxy: {
      '/api': 'http://127.0.0.1:8080',
      '/.well-known': 'http://127.0.0.1:8080',
      '/uploads': 'http://127.0.0.1:8080'
    }
  }
})
