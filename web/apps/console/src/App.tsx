import { Routes, Route } from 'react-router'
import RootLayout from './layouts/RootLayout'
import ConnectionPage from './pages/ConnectionPage'
import FieldbusControlPage from './pages/FieldbusControlPage'
import FieldbusConfigurationPage from './pages/FieldbusConfigurationPage'
import FieldbusProcessImagePage from './pages/FieldbusProcessImagePage'
import FieldbusDiagnosticsPage from './pages/FieldbusDiagnosticsPage'
import FieldbusDcSyncPage from './pages/FieldbusDcSyncPage'
import DataProcessDataPage from './pages/DataProcessDataPage'
import DataMonitoringsPage from './pages/DataMonitoringsPage'
import DataRecorderPage from './pages/DataRecorderPage'
import StorageParameterCachePage from './pages/StorageParameterCachePage'
import ServerGameLoopPage from './pages/ServerGameLoopPage'
import ServerLogPage from './pages/ServerLogPage'
import ServerRequestsPage from './pages/ServerRequestsPage'
import ReferenceApiDocsPage from './pages/ReferenceApiDocsPage'
import MetaAlStatusCodesPage from './pages/MetaAlStatusCodesPage'
import MetaEscRegistersPage from './pages/MetaEscRegistersPage'
import MetaFoeErrorCodesPage from './pages/MetaFoeErrorCodesPage'
import MetaIcHausRegistersPage from './pages/MetaIcHausRegistersPage'
import MetaKueblerRegistersPage from './pages/MetaKueblerRegistersPage'
import MetaMailboxErrorCodesPage from './pages/MetaMailboxErrorCodesPage'
import MetaObjectDataTypesPage from './pages/MetaObjectDataTypesPage'
import MetaSdoAbortCodesPage from './pages/MetaSdoAbortCodesPage'
import ToolsAutoTuningPage from './pages/ToolsAutoTuningPage'
import ToolsEsiPage from './pages/ToolsEsiPage'
import ToolsIntegroVariantPage from './pages/ToolsIntegroVariantPage'
import ToolsSiiPage from './pages/ToolsSiiPage'
import ToolsUtilitiesPage from './pages/ToolsUtilitiesPage'
import StorageUserCachePage from './pages/StorageUserCachePage'
import DeviceEthercatStatePage from './pages/DeviceEthercatStatePage'
import DeviceObjectDictionaryPage from './pages/DeviceObjectDictionaryPage'
import DeviceSiiPage from './pages/DeviceSiiPage'
import DeviceRegistersPage from './pages/DeviceRegistersPage'
import DeviceFoePage from './pages/DeviceFoePage'
import DeviceHrdPage from './pages/DeviceHrdPage'
import DeviceParametersPage from './pages/DeviceParametersPage'
import DevicePdoMappingPage from './pages/DevicePdoMappingPage'
import DeviceMotionPage from './pages/DeviceMotionPage'
import DeviceProceduresPage from './pages/DeviceProceduresPage'

export default function App() {
  return (
    <Routes>
      <Route path="/" element={<RootLayout />}>
        <Route index element={<ConnectionPage />} />
        <Route path="fieldbus">
          <Route path="control" element={<FieldbusControlPage />} />
          <Route path="configuration" element={<FieldbusConfigurationPage />} />
          <Route path="process-image" element={<FieldbusProcessImagePage />} />
          <Route path="diagnostics" element={<FieldbusDiagnosticsPage />} />
          <Route path="dc-sync" element={<FieldbusDcSyncPage />} />
        </Route>
        <Route path="data">
          <Route path="process-data" element={<DataProcessDataPage />} />
          <Route path="monitorings" element={<DataMonitoringsPage />} />
          <Route path="recorder" element={<DataRecorderPage />} />
        </Route>
        <Route path="storage">
          <Route path="parameter-cache" element={<StorageParameterCachePage />} />
          <Route path="user-cache" element={<StorageUserCachePage />} />
        </Route>
        <Route path="server">
          <Route path="game-loop" element={<ServerGameLoopPage />} />
          <Route path="log" element={<ServerLogPage />} />
          <Route path="requests" element={<ServerRequestsPage />} />
        </Route>
        <Route path="reference">
          <Route path="api-docs" element={<ReferenceApiDocsPage />} />
        </Route>
        <Route path="meta">
          <Route path="al-status-codes" element={<MetaAlStatusCodesPage />} />
          <Route path="esc-registers" element={<MetaEscRegistersPage />} />
          <Route path="foe-error-codes" element={<MetaFoeErrorCodesPage />} />
          <Route path="ic-haus-registers" element={<MetaIcHausRegistersPage />} />
          <Route path="kuebler-registers" element={<MetaKueblerRegistersPage />} />
          <Route path="mailbox-error-codes" element={<MetaMailboxErrorCodesPage />} />
          <Route path="object-data-types" element={<MetaObjectDataTypesPage />} />
          <Route path="sdo-abort-codes" element={<MetaSdoAbortCodesPage />} />
        </Route>
        <Route path="tools">
          <Route path="auto-tuning" element={<ToolsAutoTuningPage />} />
          <Route path="esi" element={<ToolsEsiPage />} />
          <Route path="integro-variant" element={<ToolsIntegroVariantPage />} />
          <Route path="sii" element={<ToolsSiiPage />} />
          <Route path="utilities" element={<ToolsUtilitiesPage />} />
        </Route>
        <Route path="devices/:deviceId">
          <Route path="ethercat-state" element={<DeviceEthercatStatePage />} />
          <Route path="object-dictionary" element={<DeviceObjectDictionaryPage />} />
          <Route path="sii" element={<DeviceSiiPage />} />
          <Route path="registers" element={<DeviceRegistersPage />} />
          <Route path="foe" element={<DeviceFoePage />} />
          <Route path="hrd" element={<DeviceHrdPage />} />
          <Route path="parameters" element={<DeviceParametersPage />} />
          <Route path="pdo-mapping" element={<DevicePdoMappingPage />} />
          <Route path="motion" element={<DeviceMotionPage />} />
          {/* The selected procedure lives in the URL, so one page serves both paths: no selection
              (the list plus a prompt) and a named procedure (the list plus its detail). */}
          <Route path="procedures" element={<DeviceProceduresPage />} />
          <Route path="procedures/:procedureName" element={<DeviceProceduresPage />} />
        </Route>
      </Route>
    </Routes>
  )
}
