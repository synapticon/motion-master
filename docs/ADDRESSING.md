# EtherCAT Device Addressing

> Background reference for how an EtherCAT master reaches one device on the bus. The first half
> describes the mechanism as the EtherCAT specification defines it. The second half describes what
> SOEM and Motion Master do with it.

An EtherCAT master sends a frame down a line of devices. A datagram inside that frame must name the
device that executes it. The specification offers several ways to name a device. Two of them matter
here:

1. **Position addressing.** The datagram names the device by its place in the line.
2. **Node addressing.** The datagram names the device by an address that the master gave it.

Commands that use position addressing start with A, for auto-increment: APRD (auto-increment
physical read) and APWR (auto-increment physical write). Commands that use node addressing start
with F, for fixed: FPRD (configured address physical read) and FPWR (configured address physical
write).

A third kind, broadcast, reaches every device at once: BRD (broadcast read) and BWR (broadcast
write). Process data uses a fourth kind, logical addressing, which this page does not cover.

## The working counter

Every datagram ends with a working counter (WKC), a 16-bit field. The master sends it as 0. Each
device that executes the datagram increments it. When the frame returns, the counter tells the
master how many devices executed the datagram.

For an APRD or an FPRD, a counter of 1 means that the device answered. A counter of 0 means that no
device answered. For a BRD, the counter is the number of devices on the wire.

Source: ETG.1000.4, section 5.4.1.

## Position addressing

The datagram carries a 16-bit address field. Each device adds 1 to the field as the frame passes
through it. The device that receives the value 0 executes the datagram. It also adds 1 before it
sends the frame on.

So the master sends `1 − position` to reach a position. To reach position 3, the master sends −2,
which is 0xFFFE as a 16-bit value:

| Device | Receives | Is it 0? | Sends on |
| --- | --- | --- | --- |
| Position 1 | −2 | No | −1 |
| Position 2 | −1 | No | 0 |
| Position 3 | 0 | Yes. It executes the datagram. | 1 |
| Position 4 | 1 | No | 2 |

Exactly one device receives 0, so exactly one device executes the datagram.

Position addressing needs no configuration. A device that has just started answers it. This is why
a master uses it at start-up, to give each device its station address.

Position addressing counts devices. It does not identify them. If a device in front of position 3
is removed, the device that was at position 4 now receives the 0.

Source: ETG.1000.3, section 4.8.3.2. ETG.1000.4, section 5.4.1.2.

## Node addressing and the station address

Each EtherCAT Slave Controller (ESC, the chip that handles EtherCAT in a device) has a register
named Configured Station Address, at 0x0010. A device executes an FPRD or an FPWR when the address
in the datagram equals the value in that register.

| Property | Value |
| --- | --- |
| Register | 0x0010, 16 bits |
| Value after power-on or reset | 0 |
| Written by | The master only. The device's own firmware can read it but cannot write it. |

The master writes the register during start-up, with an APWR by position. From then on, the master
uses the station address for nearly everything it sends to that device. The address stays with the
device when other devices are added to the chain or removed from it.

A second register, Configured Station Alias at 0x0012, can also match an FPRD or an FPWR. The ESC
loads it from the device's EEPROM after power-on. It matches only when bit 24 of the DL Control
register (0x0100) is set. That bit also resets to 0.

Source: ETG.1000.3, section 4.8.3.3. ETG.1000.4, section 6.1.2 and Table 32. ESC datasheet, section
II, Tables 10 and 11.

## How SOEM sets the station address

SOEM (Simple Open EtherCAT Master, the master library Motion Master uses) sets the addresses in
`ecx_config_init`, which a scan calls:

1. It writes 0 to register 0x0103 by broadcast. That clears the alias enable bit, so no device
   answers to its alias.
2. It counts the devices with a broadcast read.
3. For each position, it writes `0x1000 + position` to register 0x0010 with an APWR. Position 1
   gets 0x1001, position 2 gets 0x1002, and so on. The offset 0x1000 has no function. It makes the
   addresses easy to read in a network capture.
4. It reads the register back with an APRD and stores the value as `configadr` for that position.

After the scan, SOEM and Motion Master reach a device through `configadr`. This covers the AL state
reads and writes, the CoE mailbox (SDO and FoE), and the ESC register reads and writes.

## When a device loses its station address

A power cycle resets the ESC, and register 0x0010 returns to 0. The device is still on the wire,
but no FPRD or FPWR reaches it any more. Every configured-address command to it gets a working
counter of 0.

Motion Master tells the cases apart with two reads of the AL Status register (0x0130):

| FPRD at the station address | APRD at the position | Reported `reachability` |
| --- | --- | --- |
| Answers | Not sent | `answering` |
| No answer | Answers | `addressLost` |
| No answer | No answer | `notAnswering` |

`GET /api/devices/state` reports the value for each device. A state request leaves out a device that
does not answer, so it does not wait for the transition timeout.

A device that lost its station address still answers a broadcast. So the working counter of a BRD
still counts it. `GET /api/scan` compares that count with the number of devices the last scan found.
A difference shows that devices were added to the chain or removed from it, even when every known
device answers.

A scan is the only recovery. It writes every station address again, by position.

When the chain changed, the APRD reaches whichever device is now at that position. That can be a
different device. A shorter chain leaves nothing at the last positions, so those report
`notAnswering` although nothing is wrong with them. Both cases need a scan.
