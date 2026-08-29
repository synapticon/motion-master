# Distributed Clocks Primer

> Background reference for EtherCAT Distributed Clocks. This document is vendor-neutral. It
> describes the mechanism as the EtherCAT specification defines it, and it applies to any
> compliant device. It does not describe what any particular master or device does.

Distributed Clocks, or DC, is the EtherCAT mechanism that gives every device on the bus one shared
sense of time. It exists to answer one question: **when must a device act?**

The mechanism has two halves, and they are separate.

1. **Clock synchronisation.** Every device holds a clock. DC makes all of those clocks agree.
2. **Cyclic pulse generation.** A device can raise a signal at a chosen time, and repeat it. The
   first half is what makes every device raise that signal at the same instant.

Read the halves in that order. The second one has no value without the first.

## The problem DC solves

An EtherCAT frame travels down a line of devices. Device 1 reads its data first. Device 2 reads its
data next. The frame reaches the last device some time after it reaches the first.

For one axis this does not matter. For two axes that must move together it matters very much.

Consider a gantry with a motor at each end of a bridge. If each motor acts when its own data
arrives, the two motors act at different instants. The bridge skews. The error is small, and it
repeats every cycle.

DC removes that class of error. It does not remove it by making the frame faster. It removes it by
letting every device act on a shared time signal instead of on data arrival.

## The clock in each device

Every EtherCAT device contains an EtherCAT Slave Controller, or ESC. The ESC is the chip that
handles the fieldbus protocol.

The ESC holds a counter that counts nanoseconds. That counter is the device's **local time**. The
ESC derives a second value from it, called the **local system time**, which is the value DC
synchronises. Register 0x0910 holds it.

Most ESCs implement a 64-bit system time. Some implement 32 bits. A 32-bit nanosecond counter wraps
about every 4.3 seconds, so a master must handle the wrap. Check the device's documentation for
which width it has.

The goal of clock synchronisation is one sentence. **Every ESC on the bus reads the same value from
0x0910 at the same instant.**

## Three problems, not one

"The clocks disagree" sounds like one problem. It is three. Each one has a different cause and a
different fix.

| Problem | Cause | Character |
| --- | --- | --- |
| Offset | The devices power up at different moments, so the counters start from different values | A constant difference |
| Propagation delay | The frame needs real time to cross each cable and each ESC | A constant difference, set by the topology |
| Drift | Each counter runs from its own crystal, and no two crystals run at the same rate | A difference that grows |

Propagation delay is not a clock error. The clocks can be perfect and the delay is still there. It
is the time the signal spends in the wire and in the silicon.

Drift is the one that never finishes. A crystal that differs by 20 parts per million gains 20
nanoseconds every millisecond. That is 20 microseconds in a second, and 72 milliseconds in an hour.
So drift needs a correction that runs forever, not a correction that runs once.

## Step 1: elect a reference clock

One device becomes the **reference clock**. Every other clock is corrected toward it. The reference
clock itself is never corrected.

The reference is normally the first DC-capable device on the bus. The master chooses it and records
the choice.

There is no absolute time here, and DC does not need one. The reference clock defines what "now"
means for this bus. It does not have to agree with any clock outside the bus.

## Step 2: measure the propagation delays

The master needs to know how far each device sits from the reference, measured in time.

The ESC provides the measurement in hardware. A write to register 0x0900 makes every ESC latch its
own local time at each of its ports, at the instant the frame passes that port.

A frame that travels down a line and returns passes each port twice. So each device collects
several timestamps. The master reads them from registers 0x0900 to 0x090C, one 32-bit value per
port.

From that set, plus the topology it already knows, the master computes the delay for every device.
It writes each result to register 0x0928, the system time delay.

The measurement runs once, at startup. The delays are a property of the cabling, so they do not
change while the bus stays the same.

## Step 3: remove the offsets

The master reads each device's local time and compares it with the reference. It writes the
difference to register 0x0920, the system time offset.

The ESC adds that offset to its raw counter. The sum is the local system time at 0x0910.

After this step every device reads about the same value from 0x0910. The word "about" is doing
real work in that sentence, because step 4 has not run yet.

## Step 4: compensate the drift, forever

This step never stops, and it is the heart of DC.

The master adds one small datagram to the regular cyclic frame. That datagram reads the system time
from the reference clock and writes it into register 0x0910 of every other device. It runs on every
cycle.

A write to 0x0910 does not set the clock. The ESC treats the written value as the correct time and
compares it with its own. It stores the signed difference in register 0x092C.

The ESC then adjusts its own counting rate. It counts slightly faster or slightly slower until the
difference reaches zero. Three registers configure that internal loop.

| Register | Name | Purpose |
| --- | --- | --- |
| 0x0930 | Speed counter start | The bandwidth of the adjustment |
| 0x0932 | Speed counter difference | The current deviation of the clock period |
| 0x0934 | System time difference filter depth | How heavily the measurement is filtered |

So every device runs a control loop on its own clock, in hardware, with no software involved. The
master's only job is to keep sending the datagram.

Register 0x092C is also the diagnostic. Read it from any device to see how far that device sits
from the reference. A value near zero means the clock is locked. A value that grows means the
compensation is not working.

### Accuracy

The accuracy depends on the implementation and on the topology. Well-built systems commonly hold
every clock within 100 nanoseconds of the reference, and often much closer. Treat any specific
figure as a property of the hardware, and measure it with 0x092C rather than assume it.

## Step 5: generate the pulse

Now the payoff. Every clock agrees, so a time can be named and every device can act on it together.

The ESC contains a unit that raises a signal at a chosen system time and then repeats it. The
signal is called **SYNC0**. A second signal, **SYNC1**, is derived from it.

The master configures the unit through these registers.

| Register | Width | Purpose |
| --- | --- | --- |
| 0x0980 | 8 bit | Cyclic unit control: which side controls the unit |
| 0x0981 | 8 bit | Activation: enables the unit, SYNC0 and SYNC1 |
| 0x0982 | 16 bit | Pulse length, in units of 10 ns |
| 0x0984 | 8 bit | Activation status |
| 0x0990 | 64 bit | System time of the first pulse |
| 0x0998 | 64 bit | System time of the next SYNC1 pulse |
| 0x09A0 | 32 bit | SYNC0 cycle time in ns. Zero means a single pulse |
| 0x09A4 | 32 bit | SYNC1 offset from SYNC0 in ns |

The start time at 0x0990 is a system time, not a delay. That is what makes the scheme work. The
master names one instant, every device holds the same clock, so every device raises SYNC0 together.

Two rules apply to the start time.

1. **Put it in the future.** Leave enough margin for the configuration to reach every device.
2. **Round it to a whole multiple of the cycle time.** Devices that share a cycle time then share a
   pulse grid, whatever order the master configured them in.

SYNC1 is not a second independent pulse. It fires at a fixed offset after SYNC0, set by 0x09A4. Use
it when a device needs two events in a cycle, such as one instant to latch inputs and a later
instant to apply outputs.

## Step 6: the device must use the pulse

A pulse that nobody acts on changes nothing. This is the step that is easiest to overlook.

The application inside the device chooses what it reacts to. There are three choices, and the
device reports which ones it supports.

| Mode | The application acts when | Consequence |
| --- | --- | --- |
| Free run | Its own internal timer fires | The bus timing does not reach the application at all |
| SM-synchronous | The process data arrives | The master's timing jitter becomes the device's jitter |
| DC synchronous | The SYNC0 pulse fires | Every device acts at the same instant |

Only the third mode gives you what DC was built for.

The device publishes this through two objects in its object dictionary. An object dictionary is the
set of named parameters a device exposes over the mailbox protocol.

- **0x1C32** describes the output side, meaning the data the master sends to the device.
- **0x1C33** describes the input side, meaning the data the device sends to the master.

Both use the same layout.

| Subindex | Name | Meaning |
| --- | --- | --- |
| 0x01 | Synchronization Type | The mode in use. 0 is free run, 1 is SM-synchronous, 2 is DC Sync0, 3 is DC Sync1 |
| 0x02 | Cycle Time | The meaning depends on the mode. In DC mode it is the SYNC0 cycle time |
| 0x03 | Shift Time | The delay between the event and the action |
| 0x04 | Synchronization Types supported | A bit field of the modes the device supports |
| 0x05 | Minimum Cycle Time | The shortest cycle the device can hold |

ETG.1020 defines these objects in full, including the bit layout of subindex 4 and the further
subindices that report measured times and error counters.

**Treat subindex 4 as a claim, not as proof.** It is a value the device reports about itself, and a
device can report it wrongly. Where the answer matters, confirm it against the device's
documentation or by test.

## Where the master sits

The master is not part of DC. This surprises people, so it is worth stating plainly.

- The reference clock is a device, not the master.
- The correction loops run in the devices, in hardware.
- No pulse and no clock signal ever reaches the master.

The master organises the synchronisation and then stands outside it. Its own computer clock is a
separate crystal, and nothing disciplines it.

That leaves a gap. The devices hold a shared, stable time. The master sends its frame on a clock
that runs at a slightly different rate. So the frame slowly slides in phase against the pulse grid.

A frame must arrive before the pulse that acts on its data. Once the frame slides past the pulse,
the device acts on the previous cycle's data instead. The result is a stale command, or a
synchronisation fault, or a watchdog trip.

Closing that gap is master-side work, and it has two parts.

1. **Read the reference time every cycle.** The datagram from step 4 already carries it.
2. **Steer the send instant.** Compare the frame's phase against the pulse grid, then adjust the
   next cycle a little. A proportional and integral controller is the usual choice. The
   proportional term reacts to the current error. The integral term supplies the constant
   correction that the crystal difference needs.

A master that enables SYNC0 without this correction is worse off than one that never enabled it.

## Latch inputs

The DC unit works in the other direction too, and this part is often forgotten.

An ESC can timestamp an external electrical input. When the input changes, the ESC records the
system time of that change. The master reads the timestamp later, over the bus.

| Register | Purpose |
| --- | --- |
| 0x09A8, 0x09A9 | Latch 0 and Latch 1 mode |
| 0x09AE, 0x09AF | Latch status: edge detection and the current pin state |
| 0x09B0, 0x09B8 | Latch 0 time, on the positive and the negative edge |
| 0x09C0, 0x09C8 | Latch 1 time, on the positive and the negative edge |

Because every device shares one clock, a timestamp from one device can be compared with a
timestamp from another. A probe input on one device and a probe input on a device ten metres away
give times on one scale.

## What DC does not do

Each of these is a real expectation that DC does not meet.

- **It does not make the master real-time.** A frame that leaves late is still late. DC gives the
  devices a shared clock. It gives the master no help at all with its own timing.
- **It does not help a device in free run.** The pulse arrives and the application ignores it.
- **It does not remove the propagation delay.** It measures the delay so that every device can
  compensate for it.
- **It does not improve a single axis** beyond what a steady cycle already gives. The benefit is
  in the relationship between axes.
- **It is not a safety mechanism.** A synchronised bus is still not a safety function.

## Glossary

| Term | Meaning |
| --- | --- |
| DC | Distributed Clocks. The mechanism described in this document |
| ESC | EtherCAT Slave Controller. The chip in each device that handles the protocol |
| Local system time | The synchronised time value in each ESC, at register 0x0910 |
| Reference clock | The one device whose clock defines the time for the bus |
| Propagation delay | The time the frame needs to reach a device |
| Drift | The slow separation of two clocks that run from different crystals |
| SYNC0 | The cyclic signal an ESC raises at a configured system time |
| SYNC1 | A second signal, at a fixed offset after SYNC0 |
| Object dictionary | The set of named parameters a device exposes over the mailbox protocol |
| Free run | The application acts on its own timer, not on the bus |
| SM-synchronous | The application acts when process data arrives |
| DC synchronous | The application acts on the SYNC0 pulse |

## References

- **ETG.1000-4** — the data link layer protocol, including the DC datagram types.
- **ETG.1000-6** — the application layer protocol and the ESC register map.
- **ETG.1020** — protocol enhancements. Clause 23 defines synchronisation, and Table 107 defines
  objects 0x1C32 and 0x1C33.
