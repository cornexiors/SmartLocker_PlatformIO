import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Proxy dipakai hanya jika VITE_API_URL dikosongkan (lihat .env).
// Berguna kalau backend tidak mengaktifkan CORS untuk http://localhost:5173.
const target = 'http://127.0.0.1:8000'

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      '/auth': target,
      '/me': target,
      '/lockers': target,
      '/commands': target,
    },
  },
})
