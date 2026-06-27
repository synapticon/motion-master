import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { VitePWA } from 'vite-plugin-pwa'

// Minimal starter config for an app served under /apps/<name>. The base is injected
// at build time (VITE_BASE=/apps/example in deploy-pages.yml) and drives both the
// asset URLs and the router basename; it defaults to '/' for `pnpm dev`.
export default defineConfig({
  plugins: [
    react(),
    tailwindcss(),
    VitePWA({
      registerType: 'prompt',
      includeAssets: ['favicon.ico', 'apple-touch-icon-180x180.png'],
      manifest: {
        name: 'Motion Master Example',
        short_name: 'Example',
        description: 'Starter app demonstrating the shared Synapticon UI and client library',
        theme_color: '#004f5d',
        background_color: '#004f5d',
        display: 'standalone',
        scope: '/apps/example',
        start_url: '/apps/example',
        icons: [
          { src: 'pwa-64x64.png', sizes: '64x64', type: 'image/png' },
          { src: 'pwa-192x192.png', sizes: '192x192', type: 'image/png' },
          { src: 'pwa-512x512.png', sizes: '512x512', type: 'image/png', purpose: 'any' },
          {
            src: 'maskable-icon-512x512.png',
            sizes: '512x512',
            type: 'image/png',
            purpose: 'maskable',
          },
        ],
      },
      devOptions: { enabled: false },
    }),
  ],
  base: process.env.VITE_BASE ?? '/',
})
