import { createContext, useContext, useState, type ReactNode } from 'react'

// User-facing UI preferences, persisted to localStorage so they survive reloads.
interface Preferences {
  // When true, field hints render as visible text below the value; when false, as a hover tooltip.
  hintsInline: boolean
  setHintsInline: (value: boolean) => void
}

const STORAGE_KEY = 'mm.hintsInline'

// Default to inline so hints are visible without hovering; a stored value overrides it.
const readStored = (): boolean => {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    return raw === null ? true : raw === 'true'
  } catch {
    return true
  }
}

const PreferencesContext = createContext<Preferences | null>(null)

export function PreferencesProvider({ children }: { children: ReactNode }) {
  const [hintsInline, setHintsInlineState] = useState<boolean>(readStored)

  const setHintsInline = (value: boolean) => {
    setHintsInlineState(value)
    try {
      localStorage.setItem(STORAGE_KEY, String(value))
    } catch {
      // Ignore storage failures (e.g. private mode) — the in-memory value still applies.
    }
  }

  return (
    <PreferencesContext.Provider value={{ hintsInline, setHintsInline }}>
      {children}
    </PreferencesContext.Provider>
  )
}

export function usePreferences(): Preferences {
  const ctx = useContext(PreferencesContext)
  if (!ctx) {
    throw new Error('usePreferences must be used within a PreferencesProvider')
  }
  return ctx
}
