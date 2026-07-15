import { useEffect, useRef } from 'react'
import { ApiReferenceReact } from '@scalar/api-reference-react'
import '@scalar/api-reference-react/style.css'
import spec, { json as specJson, version } from 'virtual:swagger-spec'
import { downloadBytes } from '../utils/download'

const fileBase = `motion-master-api-v${version}`
const encoder = new TextEncoder()

function downloadSpec(text: string, ext: 'yml' | 'json') {
  downloadBytes(encoder.encode(text), `${fileBase}.${ext}`)
}

// Build the download bar as plain DOM so it can be inserted into Scalar's own
// introduction section (which we don't own as React). Styled via .mm-spec-download
// in the custom CSS below.
function buildDownloadBar(): HTMLElement {
  const bar = document.createElement('div')
  bar.className = 'mm-spec-download'

  const label = document.createElement('span')
  label.className = 'mm-spec-download__label'
  label.textContent = 'OpenAPI Specification'

  const ymlBtn = document.createElement('button')
  ymlBtn.type = 'button'
  ymlBtn.textContent = 'Download YAML'
  ymlBtn.addEventListener('click', () => downloadSpec(spec, 'yml'))

  const jsonBtn = document.createElement('button')
  jsonBtn.type = 'button'
  jsonBtn.textContent = 'Download JSON'
  jsonBtn.addEventListener('click', () => downloadSpec(specJson, 'json'))

  const file = document.createElement('span')
  file.className = 'mm-spec-download__file'
  file.textContent = `${fileBase}.yml / .json`

  bar.append(label, ymlBtn, jsonBtn, file)
  return bar
}

// Synapticon-matched theme for the Scalar docs. `theme: 'none'` switches off
// Scalar's built-in palette so these `--scalar-*` variables aren't overridden;
// the values mirror src/index.css design tokens. Fonts are the app's own
// @font-face families (already loaded globally), so no extra font fetch here.
const SCALAR_CSS = `
.light-mode {
  --scalar-font: "Inter", ui-sans-serif, system-ui, sans-serif;
  --scalar-font-code: ui-monospace, "SFMono-Regular", "Menlo", monospace;

  --scalar-color-1: #191817;
  --scalar-color-2: #434343;
  --scalar-color-3: #848484;
  --scalar-color-accent: #e0004d;

  --scalar-background-1: #ffffff;
  --scalar-background-2: #fafafa;
  --scalar-background-3: #f5f5f5;
  --scalar-background-accent: #e0004d1a;

  --scalar-border-color: #eeeeee;
  --scalar-radius: 0px;
  --scalar-radius-lg: 0px;

  --scalar-button-1: #e0004d;
  --scalar-button-1-color: #ffffff;
  --scalar-button-1-hover: #b80040;

  /* Sidebar — light surface, dark text; red marks the active item. Its own
     variable namespace, declared here so it cascades down. */
  --scalar-sidebar-width: 24rem;
  --scalar-sidebar-background-1: #fafafa;
  --scalar-sidebar-color-1: #191817;
  --scalar-sidebar-color-2: #656565;
  --scalar-sidebar-border-color: #eeeeee;
  --scalar-sidebar-color-active: #e0004d;
  --scalar-sidebar-item-hover-color: #191817;
  --scalar-sidebar-item-hover-background: #f5f5f5;
  --scalar-sidebar-item-active-background: #e0004d1a;
  --scalar-sidebar-search-background: #ffffff;
  --scalar-sidebar-search-border-color: #d9d9d9;
  --scalar-sidebar-search-color: #848484;
}

/* Compact the docs sidebar so its labels sit closer to the app's own
   tiny sidebar type. The list items carry Vue-scoped utility classes, so
   the reliable lever is a forced size on every text node inside it. */
.scalar-app .t-doc__sidebar,
.scalar-app .references-sidebar {
  font-size: 12px;
}
.scalar-app .t-doc__sidebar * {
  font-size: 12px !important;
  line-height: 1.4 !important;
}

/* Drop the sidebar footer chrome: "Powered by Scalar" and the "Generate MCP" entry.
   The footer has no stable class, so match the link by href and collapse its
   wrapper row via :has(). */
.scalar-app a[href*="scalar.com"],
.scalar-app div:has(> div > a[href*="scalar.com"]),
.scalar-app .mcp-nav,
.scalar-app .scalar-mcp-layer,
.scalar-app .scalar-mcp-layer-link {
  display: none !important;
}

/* "Ask AI" / "Ask AI Agent" buttons. There's no config flag, so hide by class:
   the sidebar one shares the search button's class and sits second in the row;
   the request-card one is .agent-button-container (with its popover overlay). */
.scalar-app button.bg-sidebar-b-search ~ button.bg-sidebar-b-search,
.scalar-app .agent-button-container,
.scalar-app .agent-scalar-overlay {
  display: none !important;
}

/* Our download bar, injected just below the API title in the introduction. */
.scalar-app .mm-spec-download {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 12px;
  margin: 16px 0 24px;
}
.scalar-app .mm-spec-download__label {
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: 0.025em;
  color: #656565;
}
.scalar-app .mm-spec-download button {
  border: 1px solid #e0004d;
  color: #e0004d;
  background: transparent;
  padding: 4px 12px;
  font-size: 12px;
  border-radius: 0;
  cursor: pointer;
  transition: background-color 0.15s, color 0.15s;
}
.scalar-app .mm-spec-download button:hover {
  background: #e0004d;
  color: #ffffff;
}
.scalar-app .mm-spec-download__file {
  margin-left: auto;
  font-family: var(--scalar-font-code);
  font-size: 12px;
  color: #848484;
}
`

export default function ReferenceApiDocsPage() {
  const rootRef = useRef<HTMLDivElement>(null)

  // Scalar owns the introduction DOM, so we inject our download bar there once
  // it renders, placing it directly below the API title. A MutationObserver
  // covers Scalar's async render; the existence guard makes it idempotent.
  useEffect(() => {
    const root = rootRef.current
    if (!root) return undefined

    const tryInject = () => {
      const section = root.querySelector('.introduction-section')
      if (!section || section.querySelector('.mm-spec-download')) return
      const bar = buildDownloadBar()
      const title = section.querySelector('h1')
      if (title) {
        title.insertAdjacentElement('afterend', bar)
      } else {
        section.prepend(bar)
      }
    }

    tryInject()
    const observer = new MutationObserver(tryInject)
    observer.observe(root, { childList: true, subtree: true })
    return () => observer.disconnect()
  }, [])

  return (
    <div ref={rootRef} className="h-full overflow-auto">
      <ApiReferenceReact
        configuration={{
          content: spec,
          theme: 'none',
          forceDarkModeState: 'light',
          hideDarkModeToggle: true,
          // Default the code samples to Python with the requests library.
          defaultHttpClient: { targetKey: 'python', clientKey: 'requests' },
          showOperationId: true,
          // Keep the first tag collapsed on load instead of auto-expanding it.
          defaultOpenFirstTag: false,
          // Scalar's native download button can't be steered to a specific
          // filename, so it stays hidden — we inject our own into the intro.
          hideDownloadButton: true,
          customCss: SCALAR_CSS,
        }}
      />
    </div>
  )
}
