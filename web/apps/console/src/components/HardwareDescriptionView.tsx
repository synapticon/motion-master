import type { HardwareDescription, HardwareProduct } from '@synapticon/motion-master-client'
import FieldRow from './FieldRow'

const labelCls = 'text-[10px] uppercase tracking-wide text-grey-500 font-display block'

/** One product — device or assembly — with its components listed. */
function ProductFields({ title, product }: { title: string; product: HardwareProduct }) {
  return (
    <div>
      <h4 className="font-display uppercase text-[11px] tracking-wide text-grey-500 mb-1">
        {title}
      </h4>
      <FieldRow label="Name" value={product.name} />
      <FieldRow label="Build descriptor" value={product.buildDescriptor} hint="id and version" />
      {product.keyId !== '' && (
        <FieldRow label="Key ID" value={product.keyId} hint="firmware encryption key" />
      )}
      {product.serialNumber !== '' && (
        <FieldRow label="Serial number" value={product.serialNumber} />
      )}
      {product.macAddress !== '' && <FieldRow label="MAC address" value={product.macAddress} />}
      {product.imageId !== '' && <FieldRow label="Image ID" value={product.imageId} />}
      {product.components.length > 0 && (
        <div className="pt-2">
          <div className={labelCls}>Components</div>
          <ul className="text-sm mt-1 space-y-0.5">
            {product.components.map((component, index) => (
              <li key={`${component.name}-${index}`} className="text-grey-700">
                {component.name}
                {component.version !== '' && (
                  <span className="text-grey-400"> — rev {component.version}</span>
                )}
                {component.serialNumber !== '' && (
                  <span className="font-mono text-xs text-grey-400"> {component.serialNumber}</span>
                )}
              </li>
            ))}
          </ul>
        </div>
      )}
    </div>
  )
}

/**
 * A decoded `.hardware_description`: what a SOMANET product says it is.
 *
 * The assembly comes first because it is what firmware is matched against when there is one.
 * Shared by the Utilities tool, which parses a file from disk, and the Files page, which reads one
 * off a device.
 */
export default function HardwareDescriptionView({
  description,
}: {
  description: HardwareDescription
}) {
  return (
    <div className="space-y-4">
      <FieldRow
        label="File version"
        value={description.fileVersion}
        hint="format of the file itself"
      />
      {description.assembly && <ProductFields title="Assembly" product={description.assembly} />}
      <ProductFields title="Device" product={description.device} />
      {!description.assembly && (
        <p className="text-xs text-grey-500">
          This file carries no assembly, so the device's own descriptor is the only one its firmware
          is named with.
        </p>
      )}
    </div>
  )
}
