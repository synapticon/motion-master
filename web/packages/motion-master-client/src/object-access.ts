/** What the access word of an object allows, decoded by `objectAccess()`. */
export interface ObjectAccess {
  /** Readable in some AL state (bits 0-2). */
  readable: boolean;
  /** Writable in some AL state (bits 3-5). */
  writable: boolean;
  /** Can be mapped into an RxPDO, the outputs (bit 6). */
  rxPdoMappable: boolean;
  /** Can be mapped into a TxPDO, the inputs (bit 7). */
  txPdoMappable: boolean;
  /** Can be used for backup (bit 8). */
  backup: boolean;
  /** Can be used for settings (bit 9). */
  settings: boolean;
  /** `RW`, `RO`, `WO`, or `—` for an object that is neither readable nor writable. */
  text: 'RW' | 'RO' | 'WO' | '—';
}

/**
 * Decodes the access word (ObjAccess) of an object, as ETG.1000.6 defines it: bits 0-2 read in
 * PREOP, SAFEOP and OP, bits 3-5 write in those states, bit 6 RxPDO mappable, bit 7 TxPDO
 * mappable, bit 8 backup and bit 9 settings. The parameter routes of the server send these
 * decoded already; this is for a raw word from elsewhere, such as a parameter cache file.
 */
export function objectAccess(access: number): ObjectAccess {
  const readable = (access & 0x07) !== 0;
  const writable = (access & 0x38) !== 0;
  return {
    readable,
    writable,
    rxPdoMappable: (access & 0x40) !== 0,
    txPdoMappable: (access & 0x80) !== 0,
    backup: (access & 0x100) !== 0,
    settings: (access & 0x200) !== 0,
    text: readable && writable ? 'RW' : readable ? 'RO' : writable ? 'WO' : '—',
  };
}
