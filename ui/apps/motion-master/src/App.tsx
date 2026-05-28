import { Routes, Route } from 'react-router'
import RootLayout from './layouts/RootLayout'
import DashboardPage from './pages/DashboardPage'
import EthercatStatePage from './pages/EthercatStatePage'
import ObjectDictionaryPage from './pages/ObjectDictionaryPage'
import SiiPage from './pages/SiiPage'
import RegistersPage from './pages/RegistersPage'
import FoePage from './pages/FoePage'
import ParametersPage from './pages/ParametersPage'
import ProcessDataPage from './pages/ProcessDataPage'
import LogPage from './pages/LogPage'
import RequestsPage from './pages/RequestsPage'
import EscRegistersPage from './pages/EscRegistersPage'
import AlStatusCodesPage from './pages/AlStatusCodesPage'
import FoeErrorCodesPage from './pages/FoeErrorCodesPage'
import DataTypesPage from './pages/DataTypesPage'

export default function App() {
  return (
    <Routes>
      <Route path="/" element={<RootLayout />}>
        <Route index element={<DashboardPage />} />
        <Route path="log" element={<LogPage />} />
        <Route path="requests" element={<RequestsPage />} />
        <Route path="meta/esc-registers" element={<EscRegistersPage />} />
        <Route path="meta/al-status-codes" element={<AlStatusCodesPage />} />
        <Route path="meta/foe-error-codes" element={<FoeErrorCodesPage />} />
        <Route path="meta/data-types" element={<DataTypesPage />} />
        <Route path="devices/:deviceId">
          <Route path="ethercat-state" element={<EthercatStatePage />} />
          <Route path="object-dictionary" element={<ObjectDictionaryPage />} />
          <Route path="sii" element={<SiiPage />} />
          <Route path="registers" element={<RegistersPage />} />
          <Route path="foe" element={<FoePage />} />
          <Route path="parameters" element={<ParametersPage />} />
          <Route path="process-data" element={<ProcessDataPage />} />
        </Route>
      </Route>
    </Routes>
  )
}
