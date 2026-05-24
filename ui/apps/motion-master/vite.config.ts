import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { VitePWA } from 'vite-plugin-pwa'

export default defineConfig({
  plugins: [
    react(),
    tailwindcss(),
    VitePWA({
      registerType: 'prompt',
      includeAssets: ['favicon.ico', 'favicon.svg', 'apple-touch-icon-180x180.png', 'fonts/**/*'],
      manifest: {
        name: 'Motion Master',
        short_name: 'Motion Master',
        description: 'Next-generation motion control software by Synapticon',
        theme_color: '#004f5d',
        background_color: '#004f5d',
        display: 'standalone',
        display_override: ['window-controls-overlay'],
        scope: '/app',
        start_url: '/app',
        orientation: 'landscape',
        icons: [
          { src: 'pwa-64x64.png', sizes: '64x64', type: 'image/png' },
          { src: 'pwa-192x192.png', sizes: '192x192', type: 'image/png' },
          { src: 'pwa-512x512.png', sizes: '512x512', type: 'image/png', purpose: 'any' },
          { src: 'maskable-icon-512x512.png', sizes: '512x512', type: 'image/png', purpose: 'maskable' },
        ],
      },
      workbox: {
        globPatterns: ['**/*.{js,css,html,ico,png,svg,woff,woff2}'],
        runtimeCaching: [
          {
            urlPattern: /^https:\/\/local\.motion-master\.synapticon\.com:8443\//,
            handler: 'NetworkFirst',
            options: {
              cacheName: 'mm-api-cache',
              networkTimeoutSeconds: 5,
              expiration: {
                maxEntries: 100,
                maxAgeSeconds: 60 * 60,
              },
              cacheableResponse: {
                statuses: [0, 200],
              },
            },
          },
        ],
      },
      devOptions: {
        enabled: false,
      },
    }),
  ],
  base: process.env.VITE_BASE ?? '/',
})
