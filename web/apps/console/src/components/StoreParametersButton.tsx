import { useEffect, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { apiErrorMessage } from '@synapticon/motion-master-client'
import { useConnection } from '../contexts/ConnectionContext'
import { barBtnOutline } from './deviceBarStyles'

const PROCEDURE = 'store-parameters'

/**
 * Stores the parameters of a device to its flash, with the store-parameters procedure.
 *
 * It sits in the device bar, on every device page, because a parameter changes from many places:
 * the Parameters page, Tuning, a procedure, a configuration load, a script. Wherever the change
 * came from, the store is one click away. It needs a mailbox, so it is disabled outside PRE-OP,
 * SAFE-OP and OP.
 */
export default function StoreParametersButton({
  slavePosition,
  mailboxActive,
}: {
  slavePosition: number
  mailboxActive: boolean
}) {
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const key = ['procedure', slavePosition, PROCEDURE]
  const snapshotQuery = useQuery({
    queryKey: key,
    queryFn: () => api.getProcedure(slavePosition, PROCEDURE).then(r => r.data),
    refetchInterval: query => (query.state.data?.status === 'running' ? 500 : false),
    enabled: mailboxActive,
    retry: false,
  })
  const store = useMutation({
    mutationFn: () => api.startProcedure(slavePosition, PROCEDURE, {}),
    onSettled: () => queryClient.invalidateQueries({ queryKey: key }),
  })

  // "Stored" shows for a few seconds after a store this page started, then the label returns.
  const [justStored, setJustStored] = useState(false)
  const finishedAt = snapshotQuery.data?.finishedAt
  const succeeded = snapshotQuery.data?.status === 'succeeded'
  useEffect(() => {
    if (!finishedAt || !succeeded || !store.isSuccess) {
      return
    }
    setJustStored(true)
    const timer = setTimeout(() => setJustStored(false), 3000)
    return () => clearTimeout(timer)
  }, [finishedAt, succeeded, store.isSuccess])

  const running = store.isPending || snapshotQuery.data?.status === 'running'
  const failed = snapshotQuery.data?.status === 'failed'
  const error = store.isError
    ? apiErrorMessage(store.error)
    : failed
      ? (snapshotQuery.data?.error ?? 'the store failed')
      : undefined

  return (
    <span className="flex items-center gap-2">
      <button
        type="button"
        className={barBtnOutline}
        disabled={!mailboxActive || running}
        onClick={() => store.mutate()}
        title={
          mailboxActive
            ? 'Store parameters — the device writes every value it holds to its flash (0x1010:01), as its config.csv, so the values survive a power cycle.'
            : 'Store parameters — unavailable: the device has no mailbox outside PRE-OP, SAFE-OP and OP.'
        }
      >
        {running ? 'Storing…' : justStored ? 'Stored' : 'Store parameters'}
      </button>
      {error && (
        <span className="max-w-[28ch] truncate text-xs text-status-bad" title={error}>
          {error}
        </span>
      )}
    </span>
  )
}
