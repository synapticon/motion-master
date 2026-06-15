import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { BrowserRouter } from 'react-router'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import './index.css'
import App from './App.tsx'
import { ConnectionProvider } from './contexts/ConnectionContext'
import { RequestsProvider } from './contexts/RequestsContext'
import { PreferencesProvider } from './contexts/PreferencesContext'
import { ServerStateProbe } from './components/ServerStateProbe'

const queryClient = new QueryClient()

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <QueryClientProvider client={queryClient}>
      <PreferencesProvider>
        <RequestsProvider>
          <ConnectionProvider>
            <ServerStateProbe>
              <BrowserRouter basename={import.meta.env.BASE_URL.replace(/\/$/, '')}>
                <App />
              </BrowserRouter>
            </ServerStateProbe>
          </ConnectionProvider>
        </RequestsProvider>
      </PreferencesProvider>
    </QueryClientProvider>
  </StrictMode>,
)
