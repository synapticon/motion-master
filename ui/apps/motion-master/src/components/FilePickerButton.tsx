import { useRef } from 'react'
import type { ReactNode } from 'react'
import { Upload } from 'lucide-react'
import { btnOutline } from '../utils/styles'

interface FilePickerButtonProps {
  /** Called with the chosen file. The picker resets after each pick so the same file re-fires. */
  onFile: (file: File) => void
  children: ReactNode
  accept?: string
  disabled?: boolean
  title?: string
}

// A button that opens the OS file-selection dialog and hands back the chosen File. Hides the
// native <input type="file"> behind a consistently-styled button with an upload icon, so every
// "load a file" affordance across the app looks and behaves the same.
export default function FilePickerButton({
  onFile,
  children,
  accept = '.bin,application/octet-stream',
  disabled,
  title,
}: FilePickerButtonProps) {
  const inputRef = useRef<HTMLInputElement>(null)

  function onChange(e: React.ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0]
    e.target.value = '' // allow re-selecting the same file
    if (file) {
      onFile(file)
    }
  }

  return (
    <>
      <input ref={inputRef} type="file" accept={accept} onChange={onChange} className="hidden" />
      <button
        type="button"
        onClick={() => inputRef.current?.click()}
        disabled={disabled}
        title={title}
        className={`${btnOutline} inline-flex items-center gap-1.5`}
      >
        <Upload className="h-3.5 w-3.5" aria-hidden="true" />
        {children}
      </button>
    </>
  )
}
