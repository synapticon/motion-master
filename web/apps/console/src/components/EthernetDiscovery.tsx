import { useState } from 'react';
import { useMutation, useQuery } from '@tanstack/react-query';
import { apiErrorMessage, type DiscoveredEthernetDevice } from '@synapticon/motion-master-client';
import Checkbox from './Checkbox';
import { RequestTiming, requestTiming } from './RequestTiming';
import { useConnection } from '../contexts/ConnectionContext';

// The discovery time of Discover and of Deep search. Deep search outlasts the LLDP interval of a
// drive, about 30 s, so it also finds a drive that only LLDP finds.
const DEFAULT_DURATION_MS = 2000;
const DEEP_SEARCH_DURATION_MS = 35000;

const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide';
const fieldCls = 'border border-grey-300 px-3 h-[38px] text-sm bg-white';
// A fixed minimum width on the two discovery buttons keeps them from moving while their label
// says that a discovery runs.
const btnCls =
  'inline-flex items-center justify-center border border-grey-300 text-grey-700 px-4 h-[38px] text-xs hover:border-syn-red hover:text-syn-red disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors';

const METHOD_NAMES: Record<string, string> = {
  dcp: 'DCP',
  'list-identity': 'ListIdentity',
  lldp: 'LLDP',
};

const FIRMWARE_NAMES: Record<string, string> = {
  'ethernet-ip': 'EtherNet/IP',
  profinet: 'PROFINET',
};

// Finds the drives on a network adapter of the server, so the user picks their IP addresses
// instead of typing them. The server decides everything; this lists what it found.
export default function EthernetDiscovery({ onAdd }: { onAdd: (addresses: string[]) => void }) {
  const { api } = useConnection();
  const [networkAdapter, setNetworkAdapter] = useState('');
  const [durationMs, setDurationMs] = useState(String(DEFAULT_DURATION_MS));
  const [selected, setSelected] = useState<Set<string>>(new Set());

  const networkAdaptersQuery = useQuery({
    // Its own key, so the network adapter list of the SOEM driver stays closed until its button
    // reads it.
    queryKey: ['network-adapters', 'ethernet-discovery'],
    queryFn: () => api.getNetworkAdapters(),
  });

  const discovery = useMutation({
    mutationFn: async (ms: number) => {
      const start = performance.now();
      const res = await api.discoverEthernetDevices({ networkAdapter, durationMs: ms });
      return { result: res.data, timing: requestTiming(res, start) };
    },
    onSuccess: ({ result }) => {
      // Every drive with an address on the subnet of the network adapter is preselected. A drive on
      // another subnet cannot be reached until its address or the network adapter changes.
      setSelected(
        new Set(
          result.devices
            .filter((d) => d.ipAddress !== null && d.sameSubnet !== false)
            .map((d) => d.ipAddress as string),
        ),
      );
    },
  });

  const duration = Number(durationMs);
  const durationValid = Number.isInteger(duration) && duration >= 1000 && duration <= 60000;
  const canDiscover = networkAdapter.length > 0 && !discovery.isPending;
  const devices: DiscoveredEthernetDevice[] = discovery.data?.result.devices ?? [];
  const selectable = devices.filter((d) => d.ipAddress !== null);

  function toggle(address: string, on: boolean) {
    setSelected((prev) => {
      const next = new Set(prev);
      if (on) {
        next.add(address);
      } else {
        next.delete(address);
      }
      return next;
    });
  }

  return (
    <div className="border border-grey-200 p-4 space-y-3">
      <p className={labelCls}>Discover Ethernet devices</p>
      <p className="text-xs text-grey-600">
        Finds the drives on a network adapter of the server and their IP addresses. The server opens
        no connection to a drive, so it does not disturb a client that works with one. Both buttons
        do the same on the network and differ only in how long the server listens.
      </p>
      <ul className="text-xs text-grey-600 list-disc pl-5 space-y-1">
        <li>
          <span className="font-medium">Discover</span> sends two requests on the network adapter: a
          PROFINET DCP Identify All, which a drive with PROFINET firmware answers at once, and an
          EtherNet/IP ListIdentity, which a drive with EtherNet/IP firmware and an IP address
          answers at a random time within the discovery time minus 500 ms. The server then listens
          for the discovery time, 2000 ms by default. It also keeps every LLDP frame that a drive
          sends by itself during that time.
        </li>
        <li>
          <span className="font-medium">Deep search</span> sends the same two requests and listens
          for 35 s. A drive sends LLDP about every 30 s, so Deep search always gets an LLDP frame
          from every drive that sends one. It also finds a drive that only LLDP finds, such as an
          EtherNet/IP drive without an IP address. The ListIdentity answers spread over up to 34.5
          s.
        </li>
      </ul>
      <div className="grid grid-cols-1 sm:grid-cols-[1fr_10rem] gap-3">
        <div>
          <label className={labelCls}>Network adapter</label>
          <select
            value={networkAdapter}
            onChange={(e) => setNetworkAdapter(e.target.value)}
            className={`${fieldCls} w-full`}
          >
            <option value="">
              {networkAdaptersQuery.isFetching
                ? 'Reading network adapters…'
                : 'Select a network adapter'}
            </option>
            {(networkAdaptersQuery.data?.data ?? []).map((a) => (
              <option key={a.macLinux} value={a.name}>
                {a.description || a.name} ({a.macLinux})
              </option>
            ))}
          </select>
        </div>
        <div>
          <label className={labelCls}>Discovery time (ms)</label>
          <input
            type="text"
            inputMode="numeric"
            value={durationMs}
            onChange={(e) => setDurationMs(e.target.value)}
            className={`${fieldCls} w-full font-mono`}
          />
        </div>
      </div>
      <p className="text-xs text-grey-600">
        The drives answer at a random time within the discovery time minus 500 ms, so their answers
        do not arrive at once. Raise the time for a long chain of drives. It must be at least 1000
        and at most 60000 ms.
      </p>
      <div className="flex flex-wrap items-center gap-3">
        <button
          onClick={() => discovery.mutate(duration)}
          disabled={!canDiscover || !durationValid}
          className={`${btnCls} min-w-[9rem]`}
          title="Send PROFINET DCP Identify All and EtherNet/IP ListIdentity, and collect the answers and any LLDP frame for the discovery time. Opens no connection to a drive."
        >
          {discovery.isPending && discovery.variables !== DEEP_SEARCH_DURATION_MS
            ? 'Discovering…'
            : 'Discover'}
        </button>
        <button
          onClick={() => discovery.mutate(DEEP_SEARCH_DURATION_MS)}
          disabled={!canDiscover}
          className={`${btnCls} min-w-[12rem]`}
          title="The same requests as Discover, but listen for 35 s, longer than the LLDP interval of about 30 s. Also finds a drive that only LLDP finds, such as one without an IP address."
        >
          {discovery.isPending && discovery.variables === DEEP_SEARCH_DURATION_MS
            ? 'Searching for 35 s…'
            : 'Deep search'}
        </button>
        {discovery.isSuccess && <RequestTiming timing={discovery.data.timing} />}
      </div>
      {discovery.isError && (
        <p className="text-status-bad text-xs">{apiErrorMessage(discovery.error)}</p>
      )}
      {discovery.isSuccess &&
        discovery.data.result.warnings.map((w) => (
          <p key={w} className="text-status-warn text-xs">
            {w}
          </p>
        ))}
      {discovery.isSuccess && devices.length === 0 && (
        <p className="text-xs text-grey-600">
          No drive answered. Try Deep search, check the cable, and check that the drive runs
          EtherNet/IP or PROFINET firmware.
        </p>
      )}
      {devices.length > 0 && (
        <>
          <div className="border border-grey-200 overflow-x-auto">
            <table className="w-full text-xs border-collapse">
              <thead>
                <tr className="border-b border-grey-200 bg-grey-50">
                  {['', 'IP Address', 'MAC Address', 'Name', 'Firmware', 'Found by'].map((h) => (
                    <th
                      key={h}
                      className="text-left px-3 py-2 font-display uppercase tracking-wide text-grey-600 font-medium"
                    >
                      {h}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {devices.map((d, i) => (
                  <tr
                    key={d.ipAddress ?? d.macAddress ?? i}
                    className="border-b border-grey-100 last:border-0"
                  >
                    <td className="px-3 py-2">
                      {d.ipAddress !== null && (
                        <Checkbox
                          checked={selected.has(d.ipAddress)}
                          onChange={(on) => toggle(d.ipAddress as string, on)}
                          aria-label={`Select ${d.ipAddress}`}
                        />
                      )}
                    </td>
                    <td className="px-3 py-2 font-mono">
                      {d.ipAddress ?? (
                        <span
                          className="text-grey-500 cursor-help"
                          title="The drive has no IP address. Give it one with a DHCP server or a PROFINET or EtherNet/IP tool."
                        >
                          none
                        </span>
                      )}
                      {d.sameSubnet === false && (
                        <span
                          className="ml-2 text-status-warn cursor-help"
                          title="The drive is on another subnet than the network adapter. Motion Master reaches it only after its address or the address of the network adapter changes."
                        >
                          other subnet
                        </span>
                      )}
                    </td>
                    <td className="px-3 py-2 font-mono">{d.macAddress ?? '—'}</td>
                    <td className="px-3 py-2">{d.name || '—'}</td>
                    <td className="px-3 py-2">{d.firmware ? FIRMWARE_NAMES[d.firmware] : '—'}</td>
                    <td className="px-3 py-2">
                      {d.foundBy.map((m) => METHOD_NAMES[m] ?? m).join(', ')}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="flex items-center gap-3">
            <button
              onClick={() =>
                onAdd(selectable.map((d) => d.ipAddress as string).filter((a) => selected.has(a)))
              }
              disabled={selected.size === 0}
              className={btnCls}
              title="Append the selected IP addresses to the list above. An address already in the list is not added again."
            >
              Add selected to IP addresses
            </button>
            <p className="text-xs text-grey-600">
              Appends the selected addresses that the list does not hold yet, in the order of this
              table.
            </p>
          </div>
        </>
      )}
    </div>
  );
}
