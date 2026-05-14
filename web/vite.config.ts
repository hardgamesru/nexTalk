import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// https://vite.dev/config/
export default defineConfig({
  plugins: [vue()],
  server: {
    host: process.env.VITE_DEV_HOST ?? '127.0.0.1',
    port: 5173,
    strictPort: true,
  },
})
