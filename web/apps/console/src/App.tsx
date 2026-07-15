import { Routes, Route } from 'react-router'
import RootLayout from './layouts/RootLayout'
import ConnectionPage from './pages/ConnectionPage'
import FieldbusPage from './pages/FieldbusPage'
import EthercatStatePage from './pages/EthercatStatePage'
import ObjectDictionaryPage from './pages/ObjectDictionaryPage'
import SiiPage from './pages/SiiPage'
import ToolsSiiPage from './pages/ToolsSiiPage'
import RegistersPage from './pages/RegistersPage'
import FoePage from './pages/FoePage'
import ParametersPage from './pages/ParametersPage'
import PdoMappingPage from './pages/PdoMappingPage'
import ProcessDataPage from './pages/ProcessDataPage'
import ProcessImagePage from './pages/ProcessImagePage'
import MonitoringsPage from './pages/MonitoringsPage'
import RecorderPage from './pages/RecorderPage'
import ParameterCachesPage from './pages/ParameterCachesPage'
import BusConfigPage from './pages/BusConfigPage'
import BusDiagnosticsPage from './pages/BusDiagnosticsPage'
import DcSyncPage from './pages/DcSyncPage'
import GameLoopPage from './pages/GameLoopPage'
import LogPage from './pages/LogPage'
import RequestsPage from './pages/RequestsPage'
import EscRegistersPage from './pages/EscRegistersPage'
import AlStatusCodesPage from './pages/AlStatusCodesPage'
import FoeErrorCodesPage from './pages/FoeErrorCodesPage'
import DataTypesPage from './pages/DataTypesPage'
import ApiDocsPage from './pages/ApiDocsPage'

export default function App() {
  return (
    <Routes>
      <Route path="/" element={<RootLayout />}>
        <Route index element={<ConnectionPage />} />
        <Route path="fieldbus" element={<FieldbusPage />} />
        <Route path="process-image" element={<ProcessImagePage />} />
        <Route path="bus-config" element={<BusConfigPage />} />
        <Route path="bus-diagnostics" element={<BusDiagnosticsPage />} />
        <Route path="dc-sync" element={<DcSyncPage />} />
        <Route path="process-data" element={<ProcessDataPage />} />
        <Route path="monitorings" element={<MonitoringsPage />} />
        <Route path="recorder" element={<RecorderPage />} />
        <Route path="parameter-caches" element={<ParameterCachesPage />} />
        <Route path="game-loop" element={<GameLoopPage />} />
        <Route path="log" element={<LogPage />} />
        <Route path="requests" element={<RequestsPage />} />
        <Route path="api-docs" element={<ApiDocsPage />} />
        <Route path="meta/al-status-codes" element={<AlStatusCodesPage />} />
        <Route path="meta/esc-registers" element={<EscRegistersPage />} />
        <Route path="meta/foe-error-codes" element={<FoeErrorCodesPage />} />
        <Route path="meta/object-data-types" element={<DataTypesPage />} />
        <Route path="tools/sii" element={<ToolsSiiPage />} />
        <Route path="devices/:deviceId">
          <Route path="ethercat-state" element={<EthercatStatePage />} />
          <Route path="object-dictionary" element={<ObjectDictionaryPage />} />
          <Route path="sii" element={<SiiPage />} />
          <Route path="registers" element={<RegistersPage />} />
          <Route path="foe" element={<FoePage />} />
          <Route path="parameters" element={<ParametersPage />} />
          <Route path="pdo-mapping" element={<PdoMappingPage />} />
        </Route>
      </Route>
    </Routes>
  )
}
