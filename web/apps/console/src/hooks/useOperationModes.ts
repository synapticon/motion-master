import { useQuery } from '@tanstack/react-query'
import type { OperationMode } from '@synapticon/motion-master-client'
import { useConnection } from '../contexts/ConnectionContext'

// Static per device — a property of its firmware, not of its state — so it is fetched once and kept.
export const operationModesKey = (slavePosition: number) => ['operationModes', slavePosition]

/**
 * Every operation mode the device at @p slavePosition has, standard and manufacturer-specific.
 *
 * The reason this is fetched at all rather than hard-coded: 0x6061 answers with a number, and the
 * negative half of that range is the vendor's, so a drive sitting in -2 reads as "Unknown" to
 * anything holding only the CiA402 table. This is what names it.
 *
 * @param enabled Whether to fetch. False for a device that cannot answer — not a CiA402 drive, or
 *                no active mailbox.
 */
export function useOperationModes(slavePosition: number, enabled = true) {
  const { api } = useConnection()
  return useQuery({
    queryKey: operationModesKey(slavePosition),
    queryFn: () => api.getOperationModes(slavePosition).then((r) => r.data),
    staleTime: Infinity,
    enabled,
    retry: false,
  })
}

/** The mode with this 0x6060/0x6061 value, or undefined when the table does not name it. */
export function findMode(
  modes: OperationMode[] | undefined,
  value: number,
): OperationMode | undefined {
  return modes?.find((m) => m.value === value)
}
