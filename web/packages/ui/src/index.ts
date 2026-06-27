// Public entry point for @synapticon/ui — the shared design system and UI components.
//
// The Tailwind theme (design tokens, fonts, base layer) is a CSS side-export: import it once per
// app with `import '@synapticon/ui/theme.css'`. The components below are the starter set; apps are
// free to use them, wrap them, or bail out and build their own.

export { Callout, type CalloutVariant } from './components/Callout'
export { PageHeader } from './components/PageHeader'
export { Button, btnOutline } from './components/Button'
