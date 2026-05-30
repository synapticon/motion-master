import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { defineConfig, type Plugin } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { VitePWA } from 'vite-plugin-pwa'
import { parse as parseYaml } from 'yaml'

// Bundle apps/motion_master/swagger.yml (the binary's shipped spec, outside this
// app's tree) as a virtual module so the API docs page can render it without a
// network fetch — no CORS exposure and it stays available offline in the PWA.
// Named exports give the download links a JSON rendering and the spec version
// (info.version, kept in sync by tools/bump-version.sh) without re-parsing at
// runtime: `default` = raw YAML, `json` = pretty JSON, `version` = info.version.
function swaggerSpec(): Plugin {
  const virtualId = 'virtual:swagger-spec'
  const resolvedId = '\0' + virtualId
  const specPath = fileURLToPath(
    new URL('../../../apps/motion_master/swagger.yml', import.meta.url),
  )
  return {
    name: 'swagger-spec',
    resolveId(id) {
      if (id === virtualId) return resolvedId
    },
    load(id) {
      if (id === resolvedId) {
        const yaml = readFileSync(specPath, 'utf-8')
        const doc = parseYaml(yaml)
        const json = JSON.stringify(doc, null, 2)
        const version = doc?.info?.version ?? '0.0.0'
        return [
          `export default ${JSON.stringify(yaml)}`,
          `export const json = ${JSON.stringify(json)}`,
          `export const version = ${JSON.stringify(version)}`,
        ].join('\n')
      }
    },
    configureServer(server) {
      server.watcher.add(specPath)
    },
  }
}

export default defineConfig({
  plugins: [
    react(),
    tailwindcss(),
    swaggerSpec(),
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
