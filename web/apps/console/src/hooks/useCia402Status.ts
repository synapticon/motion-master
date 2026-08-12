import { useQuery } from '@tanstack/react-query'
import { useConnection } from '../contexts/ConnectionContext'

// One place for the CiA402 status query, because two views now read it: the sticky status bar on
// every device page and the Motion page's controls. Sharing the key alone would not be enough —
// two observers with different refetch intervals on one key make the effective cadence whichever
// happens to be mounted, which is exactly the kind of thing nobody notices until the log is full.
export const cia402StatusKey = (slavePosition: number) => ['cia402', slavePosition]

/**
 * The drive's live CiA402 status — state, mode, statusword, controlword and the active setpoint.
 *
 * Polled rather than pushed: the state changes out of band (another client, the drive's own faults,
 * a procedure walking the state machine), and there is no notification channel for it. Half a second
 * is fast enough that a command you pressed looks immediate and slow enough that a device page left
 * open does not fill the Requests log.
 *
 * @param slavePosition Device to read.
 * @param enabled       Whether to poll at all. Pass false for a device that cannot answer — one that
 *                      is not a CiA402 drive, or whose mailbox is inactive (INIT/BOOT) — so an
 *                      ordinary page visit does not generate a failing request twice a second.
 */
export function useCia402Status(slavePosition: number, enabled = true) {
  const { api } = useConnection()
  return useQuery({
    queryKey: cia402StatusKey(slavePosition),
    queryFn: () => api.getCia402Status(slavePosition).then((r) => r.data),
    refetchInterval: 500,
    enabled,
    retry: false,
  })
}
