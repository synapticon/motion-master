/// <reference types="vite/client" />
/// <reference types="vite-plugin-pwa/client" />

declare module 'virtual:swagger-spec' {
  const spec: string
  export default spec
  export const json: string
  export const version: string
}
