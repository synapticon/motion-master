# Frames, Repeats and Timeouts

> **Hand-maintained.** This file is derived by reading the source, not generated. The values come
> from the SOEM build in `ports/soem/portfile.cmake`, from `libs/comm/soem_fieldbus_driver.cc` and
> from `libs/comm/soem_mailbox.h`. When a timeout, a repeat rule or a mailbox rule changes, update
> this file in the same commit.

This document explains how Motion Master exchanges frames with the slaves on an EtherCAT bus. It
covers what a frame carries, which frames go out again when no answer comes, and how long each
step waits. It describes the SOEM fieldbus driver. The SPoE driver uses TCP and does not send
EtherCAT frames.

## The frame

The master is Motion Master. A slave is a device on the bus, such as a drive. The master sends
one **EtherCAT frame**, which is a normal Ethernet frame with EtherType 0x88A4. The frame passes
through every slave on the line and comes back to the master. Each slave reads and writes its part
while the frame goes through it. So one frame can serve the whole bus.

A frame carries one or more **datagrams**. Each datagram has these fields:

- **Command.** What the slaves do with the datagram, and how the datagram addresses them.
- **Index.** An 8-bit number. The master uses it to match a returned frame to the frame it sent.
- **Address and length.** The slave, and the memory in its ESC (EtherCAT Slave Controller, the
  chip that handles the frame).
- **Data.** The bytes that the datagram reads or writes.
- **Working counter.** Each slave that does the operation adds to it. The master compares the
  returned value with the value it expects.

Motion Master uses these commands:

| Command | Addresses | Used for |
| --- | --- | --- |
| FPRD, FPWR | One slave, by its station address | Registers and the mailbox |
| LRD, LWR | Logical memory across all slaves | Cyclic process data, one LWR and one LRD each cycle |
| BRD, BWR, APRD | All slaves, or one slave by its position | The scan of the bus |

### How a returned frame is matched

SOEM has 32 indexes. The real-time thread and the control plane share them. The control plane is
every operation that is not cyclic, such as a parameter read or a file transfer. It uses at most 8
indexes at a time, and the real-time thread gets the rest.

The master accepts a returned frame only when its index and its header match the datagram it sent
under that index. The header check compares the command, the offset, the length, and the slave
address for the commands whose address the slaves do not change. So a frame that comes back very
late, after its index went to a newer datagram, is dropped.

**A frame that is late and a frame that is lost look the same to the master.** The master knows
only that no frame came back before its wait ended.

## Two kinds of traffic

### Cyclic process data

Process data is the values that the master and the slaves exchange every cycle, such as a target
position and an actual position. The real-time thread sends one process-data frame each cycle. A
cycle is 1 ms by default.

The thread waits up to `EC_TIMEOUTRET`, 2 ms, for the frame. It never sends the frame again. When
the frame does not come back, or comes back with a lower working counter than expected, the cycle
counts as a short working counter. The next cycle sends a new frame.

### Register datagrams

The control plane reads and writes ESC registers with FPRD and FPWR. Examples are the AL status
(Application Layer state, such as INIT, PRE-OP, SAFE-OP and OP), the SyncManager status, and the
ESC type.

SOEM sends a register datagram, waits `EC_TIMEOUTRET`, 2 ms, and sends the same datagram again.
It continues until a frame comes back or the total wait ends. A repeat does no harm, because a
second read or write of a register changes nothing. Each call sets its own total wait. The reads
and writes that an AL state change depends on wait 100 ms.

## The mailbox

The mailbox is how the master and a slave exchange messages. SDO (Service Data Object, a read or
write of one entry of the object dictionary) and FoE (File over EtherCAT, a file transfer) both use
it. The mailbox uses two SyncManagers, which are buffers in the ESC:

- **SyncManager 0 (SM0)** carries a message from the master to the slave. The master writes the
  whole message with one FPWR. A working counter of 1 means that the message is in SM0. A working
  counter of 0 means that SM0 was still full. The ESC refused the write, and the slave did not get
  the message.
- **SyncManager 1 (SM1)** carries a message from the slave to the master. The master reads the SM1
  status register, 0x080D, every 200 µs. When bit 3 shows that SM1 is full, the master reads the
  whole SM1 area with one FPRD. That read empties SM1.

Each mailbox message carries a 3-bit **counter**, 1 to 7. A slave uses it to recognise a second
copy of a message.

### Why a mailbox frame is not simply sent again

SOMANET drives use two different EtherCAT stacks, and they react to the counter in opposite ways:

| Stack | ESC type | Gets the same counter twice | Gets a counter that skips one |
| --- | --- | --- | --- |
| XMOS, in a Circulo | 0xC0 | Locks its mailbox until INIT, then PRE-OP | Accepts it |
| netX, in an Integro | 0x85 | Discards the copy | Drops up to six requests, until the counter comes round |

The ESC type is the low byte of ESC register 0x0000. The scan reads it for every slave.
`GET /api/devices` reports it as `escType`, and reports the write rule as `mailboxWriteOnce`.

So the mailbox rules depend on the stack. The master writes each message once to every
Synapticon or Sensodrive device whose ESC type is not 0x85. That includes a device whose ESC type
did not read, because one write is the safe side.

- **A mailbox write to an XMOS stack goes out once.** The master waits up to `EC_TIMEOUTLATE`,
  100 ms, for the frame.
    - A working counter of 0: the master writes the message once more after SM0 is empty, and gives
      the counter back if that also fails.
    - No frame: the master does not send the message again, because the slave can have it. The
      caller waits for the answer. The master never uses that counter again.
- **A mailbox write to a netX stack follows upstream SOEM.** SOEM sends the write again every
  2 ms. The netX discards the copies.
- **A mailbox read goes out once, for every slave.** The master waits up to 100 ms for the frame.
    - No frame, and SM1 is still full: the read did not reach the slave, so the master reads again.
    - No frame, and SM1 is empty: the slave gave the message away, and the message is lost. The
      master does not ask a SOMANET drive to repeat it, because an Integro then sends the message
      before the last one. The transfer fails.
- **The counter starts from a known point.** When a SOMANET drive reaches PRE-OP from INIT, the
  master sends it a request with counter 0. Both stacks accept counter 0, whatever came before it.

A slave of another vendor follows upstream SOEM for mailbox writes. Its mailbox reads also go out
once. When its message is lost, the master asks the slave to repeat it, as upstream SOEM does.

### Waits above the frames

Each mailbox protocol has its own waits for the answer of the slave:

| Operation | Wait for the answer | Attempts |
| --- | --- | --- |
| SM0 to become empty before a write | 20 ms | Not repeated |
| SDO upload (read) | 700 ms | 2. The second only after no answer |
| SDO download (write) | 700 ms | 1. A second copy can start an OS command again |
| FoE read, each packet | 6 s | 3. Another attempt only after no answer, 100 ms apart |
| FoE write, each acknowledgement | 10 s | 3, in the firmware installation |
| File removal on a SOMANET drive, which is an FoE read | 10 s, 30 s for the ESI | As an FoE read |

### An FoE read

An FoE read is a sequence of mailbox messages:

1. The master writes a read request with the file name.
2. The slave sends data packet 1.
3. The master writes acknowledgement 1.
4. The slave sends data packet 2, and the sequence continues.
5. A packet that is shorter than the mailbox ends the file.

Nothing in the transfer carries the size of the file or a checksum. When a read fails and the slave
sent no FoE error, the master sends an FoE ERROR request. That request ends the transfer on the
slave, so the slave is ready for the next request.

## Example: a late frame during a file read

Issue #138 shows how these rules work together on a PC that holds a returned frame:

1. The master read `config.csv` from an Integro.
2. The mailbox read that carried data packet 4 got no frame within 100 ms. The PC held the frame,
   although the Integro answered at once.
3. The master gave up on the transfer and sent an FoE ERROR request.
4. The Integro ignored the request and kept its position in the file.
5. The master tried the read again. The Integro sent the rest of the file. The last packet was
   short, as at the end of every file, so the master took the rest for the whole file.

The cause on the drive is filed as synapticon/somanet_software#5817.
