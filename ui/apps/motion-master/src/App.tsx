import { Routes, Route } from 'react-router'
import RootLayout from './layouts/RootLayout'
import DashboardPage from './pages/DashboardPage'
import EthercatStatePage from './pages/EthercatStatePage'
import ObjectDictionaryPage from './pages/ObjectDictionaryPage'
import SiiPage from './pages/SiiPage'
import RegistersPage from './pages/RegistersPage'
import FoePage from './pages/FoePage'
import ProcessDataPage from './pages/ProcessDataPage'

export default function App() {
  return (
    <Routes>
      <Route path="/" element={<RootLayout />}>
        <Route index element={<DashboardPage />} />
        <Route path="devices/:deviceId">
          <Route path="ethercat-state" element={<EthercatStatePage />} />
          <Route path="object-dictionary" element={<ObjectDictionaryPage />} />
          <Route path="sii" element={<SiiPage />} />
          <Route path="registers" element={<RegistersPage />} />
          <Route path="foe" element={<FoePage />} />
          <Route path="process-data" element={<ProcessDataPage />} />
        </Route>
      </Route>
    </Routes>
  )
}
