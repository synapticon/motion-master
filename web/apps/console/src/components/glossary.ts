// The shared glossary behind `Term`, one entry per word a Learn page marks up.
//
// A term is defined once here and reused wherever it appears, so the three Learn pages cannot
// drift into three descriptions of the same thing. The id is what the prose cites, so treat one as
// stable once a page uses it.
//
// Keep a definition to a sentence or two. It is read in a small popover, and anything longer
// belongs in the prose or on the page that owns the topic.

export interface GlossaryEntry {
  /** The term as it is written in the popover heading. */
  term: string
  definition: string
}

export const glossary = {
  encoder: {
    term: 'Encoder',
    definition:
      'A sensor on the motor shaft that reports the shaft position, so the drive knows where the rotor is.',
  },
  'following-error': {
    term: 'Following error',
    definition:
      'The gap between the position the drive commanded and the position the encoder reports. A SOMANET drive reports it on 0x60F4.',
  },
  rotor: {
    term: 'Rotor',
    definition:
      'The turning part of the motor: the shaft and the permanent magnets fixed to it.',
  },
  stator: {
    term: 'Stator',
    definition: 'The stationary part of the motor, surrounding the rotor. It carries the coils.',
  },
  phase: {
    term: 'Phase',
    definition:
      'One of the three groups the stator coils are wired into. The drive sets the current in each group, and the three together decide where the magnetic field points.',
  },
  'holding-brake': {
    term: 'Holding brake',
    definition:
      'A friction brake built into the motor that grips whenever its coil is unpowered. It keeps a stationary axis where it is. It is not made to stop a moving one.',
  },
  commutation: {
    term: 'Commutation',
    definition:
      'Switching the current between a motor\u2019s windings as the rotor turns, so the magnetic pull always stays ahead of it. A brushed motor does it mechanically. A servo drive does it itself, which is why it has to know where the rotor is.',
  },
  commutator: {
    term: 'Commutator',
    definition:
      'The ring of segments on a brushed motor\u2019s shaft that the brushes press against. It switches current between the windings as the shaft turns.',
  },
  inductance: {
    term: 'Inductance',
    definition:
      'A winding\u2019s opposition to any change in the current through it. It is why a voltage switched on and off in hard steps still produces a smooth current.',
  },
  pwm: {
    term: 'PWM',
    definition:
      'Pulse-width modulation. Producing an average voltage by switching between two fixed levels and varying how long each pulse lasts.',
  },
  inverter: {
    term: 'Inverter',
    definition:
      'A power stage that makes AC out of a DC supply by switching. In a servo drive it is the six transistors that connect each phase to one rail or the other.',
  },
  'pole-pair': {
    term: 'Pole pair',
    definition:
      'One north magnet pole and one south. A rotor with two pole pairs has four magnet poles, and the winding pattern that drives them repeats twice round the circle.',
  },
  slot: {
    term: 'Slot',
    definition:
      'One of the openings in the stator iron that the winding sits in. A phase occupies several of them.',
  },
  'torque-constant': {
    term: 'Torque constant',
    definition:
      'How much torque a motor makes per amp of current, in newton metres per amp. It comes off the motor datasheet.',
  },
  'back-emf': {
    term: 'Back-EMF',
    definition:
      'The voltage a motor generates when its shaft is turned. It rises in proportion to velocity, and it is what the supply voltage has to overcome to push current in.',
  },
  saturation: {
    term: 'Saturation',
    definition:
      'The point where the iron in the motor is carrying about as much magnetic field as it can. Past it, more current buys less extra torque.',
  },
  'field-weakening': {
    term: 'Field weakening',
    definition:
      'Spending part of the current on a field that opposes the rotor magnets, which lowers the back-EMF and leaves room for more velocity. The current doing it makes no torque.',
  },
} as const satisfies Record<string, GlossaryEntry>

export type GlossaryId = keyof typeof glossary
