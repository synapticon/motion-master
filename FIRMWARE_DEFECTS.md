# SOMANET firmware defects on EtherCAT, and what Motion Master does about them

**Status, 2026-11-01.** This page is for the firmware team. It lists the filed firmware defects that concern EtherCAT or Motion Master, every firmware behaviour that Motion Master works around without an issue, and every fix in Motion Master that deals with a drive that misbehaves on the bus. Motion Master 6 is this repository. Motion Master 5 is `synapticon/motion_master`. Each item says, for each of the two, whether it is solved.

Motion Master works around most of the defects. A workaround in Motion Master does not help a customer who uses another master, such as TwinCAT, CODESYS or the IgH EtherCAT Master. It also does not help a customer who runs an older Motion Master.

## Terms

- **AL state**: the EtherCAT state of a device: INIT, PRE-OP, SAFE-OP, OP or BOOT.
- **Frame**: one Ethernet packet with EtherType 0x88A4. The master sends it, it passes through every device on the bus, and it comes back to the master. A frame carries one or more datagrams.
- **Datagram**: one command inside a frame, such as LRD (logical read), LWR (logical write), LRW (logical read and write) or FPWR (write to a register of one device). It has a header with the command, an 8-bit index, the address and the length, then the data, then the WKC.
- **Mailbox**: the acyclic channel to a device. SM0 (SyncManager 0) holds a request from the master, and SM1 holds the answer from the device.
- **Mailbox counter**: a 3-bit number in each mailbox header. ETG.1000.4 Table 29 lets the device use it to find a repeated request.
- **SDO**: a CoE read or write of one object dictionary entry over the mailbox.
- **SDO Info**: the CoE service that describes the object dictionary.
- **FoE**: File access over EtherCAT, the mailbox protocol for files.
- **PDO**: process data, exchanged every cycle. TxPDO is the data from the drive to the master. RxPDO is the data from the master to the drive.
- **WKC**: the working counter of an EtherCAT datagram. The master compares it with the expected value.
- **netX** and **SoC**: an Integro runs its EtherCAT stack on a netX processor and its application on a separate SoC. A Circulo and a Node run the XMOS EtherCAT stack, `sc_somanet_ip/module_ethercat`.
- **Jasper**: the firmware of a Jasper-based board, such as the C021. Its ESC type is 0x90.

The columns **Motion Master 6** and **Motion Master 5** start with one of these words:

- **Workaround**: the master avoids the defect or recovers from it. The defect stays in the firmware.
- **Partial**: the master avoids a part of the defect. The cell says which part.
- **None**: the master does nothing about it.
- **Not checked**: nobody checked this version yet.
- **No action needed**: the master does not trigger the defect.

In the change lists, a tag at the start of each line names where the cause is:

- **FW**: a defect or limit in the drive firmware.
- **SOEM**: a defect in the SOEM library that Motion Master uses as its EtherCAT master.
- **MM**: a defect in Motion Master itself.

## 1. Defects filed

Each issue holds the evidence, the reproduction and the source references. A number with no repository is in `synapticon/somanet_software`.

| Issue | Device | Defect | Motion Master 6 | Motion Master 5 | What stays broken |
| --- | --- | --- | --- | --- | --- |
| [#5806](https://github.com/synapticon/somanet_software/issues/5806) | Integro | An orderly AL state change out of OP latches CyclicHb (0x603F = 0x7500). A Circulo 7 on the same firmware does not. The check in `drive_control_service.xc` runs while the drive still counts itself in OP. | None. The master cannot avoid the fault. | None. | Every drop from OP gives a fault that needs a fault reset in OP. |
| [#5807](https://github.com/synapticon/somanet_software/issues/5807) | Circulo, Node | After OP to SAFE-OP, the TxPDO stops updating and SM2 is switched off, so the WKC is 1 instead of 3. One call, `sync_manager_activate(smindex_buffer_receive, 0, ...)` in `esm_op_state_handler`, causes both. | None. The master does not read over SDO below OP, because ETG.1000.6 requires valid inputs in SAFE-OP. The expected WKC in SAFE-OP is 3, so the drop shows as a short WKC. | None. | In SAFE-OP the master reads old values that look valid. |
| [#5808](https://github.com/synapticon/somanet_software/issues/5808) | Integro | The TxPDO also stops updating in SAFE-OP. The WKC stays correct at 3. The cause is not found. | None. | None. | The TxPDO can show Operation enabled while the drive is in Fault. |
| [#5809](https://github.com/synapticon/somanet_software/issues/5809) | Bootloader, all XMOS drives | When the master gives up on an FoE read in BOOT, the FoE layer stays in READ. It answers every later FoE request with mailbox error 0x0008, through INIT and BOOT, until a power cycle. | Workaround, alpha.91: a longer FoE timeout, and an FoE ERROR request after every FoE failure with no answer. The FoE ERROR request also frees a drive that is already stuck. | Workaround, 5.8.6: the same. | Other masters leave the drive stuck. In the field, firmware installation stopped at about 85 %. |
| [#5811](https://github.com/synapticon/somanet_software/issues/5811) | Circulo, Node, bootloader | A mailbox request that arrives twice with the same counter locks SM0 until INIT and then PRE-OP. The repeat check returns before the firmware reads the last byte of SM0, so the copy never leaves it. The bootloader of a Jasper-based C021 discards the copy correctly (one run, BOOT only, PRE-OP not measured). | Workaround, alpha.92: each mailbox request goes to a SOMANET device once and is never sent again, except on a netX. | Partial, 5.8.5: one SDO request with counter 0 after each scan. A repeated write from SOEM still locks the drive. | A master that sends a write again, which ETG.1000.4 allows, locks the drive. SOEM does this when a frame comes back after 2 ms. |
| [#5817](https://github.com/synapticon/somanet_software/issues/5817) | Integro | (1) The mailbox takes only the counter after the last accepted one, and drops every other counter with no answer. (2) A Repeat Request returns the message before the last one. (3) After an aborted FoE read, the next read returns the tail of the file as a complete file. | Partial, alpha.93 and alpha.99: (1) the master keeps the counter sequence without a gap and sends a lost write again. (2) The master never sends a Repeat Request. (3) None. | Not checked. | (1) One skipped counter costs up to six requests at 700 ms each on another master. (3) A wrong file is reported as a good one. Issue `synapticon/motion-master#138` is open for it. |
| [#5820](https://github.com/synapticon/somanet_software/issues/5820) | Circulo | An FoE read that the master stops with one packet unacknowledged leaves the file system busy (0x800B), or serving zeros, until a power cycle. INIT does not clear it. | Partial, alpha.93: a late frame no longer makes the master abort a read. | Not checked. | A packet that is really lost on the wire still wedges the file system. |
| [#5821](https://github.com/synapticon/somanet_software/issues/5821) | Integro | SDO Info Get Entry Description for a subindex that does not exist answers success, with data from uninitialised stack memory. `Handle_Get_Object_Info_Ind` has the same shape. | Workaround, alpha.112: an Integro is not asked past the max subindex. | Not checked. | The dictionary of an Integro lacks the reserved PDO mapping slots, so the OS command 21 dictionary read fails on it. |
| [#5822](https://github.com/synapticon/somanet_software/issues/5822) | Circulo, Kübler encoder service | A stream up through `fs-buffer` sends one extra empty DATA packet after the last one. The next mailbox request reads it, and the file system then wedges until a power cycle. | Workaround, alpha.112: the master reads `fs-buffer` only after the command completes. | Not checked. | An output larger than 2024 bytes cannot be read that way: command 21 on a Circulo with a safety module (2517 bytes) and command 17.7 (2072 bytes). |
| [#5823](https://github.com/synapticon/somanet_software/issues/5823) | Integro | After SAFE-OP or OP to PRE-OP, a write of 0x1C12, 0x1C13, 0x16xx or 0x1Axx aborts with 0x08000000. The source points to the SoC, which does not learn of the step down. | Workaround: the master reaches PRE-OP through INIT. | Not checked. | A master that steps down to PRE-OP to change the PDO mapping fails. |
| [#5824](https://github.com/synapticon/somanet_software/issues/5824) | Integro | A write of a locked PDO assignment or mapping object gets no answer. `Dynamic_PDO_Assignment` returns without `Pkt_SendPacket`. | None. The master clears 0x1C12:00 first, so it rarely meets this. | Not checked. | The master waits out its mailbox timeout and cannot tell a refusal from a lost frame. |
| [#5836](https://github.com/synapticon/somanet_software/issues/5836) | Integro | An FoE read request gets FoE BUSY while the file is not ready. ETG.1000.5 defines BUSY only for a write. This happens in the first seconds of PRE-OP after power-on, and on the first read after BOOT is entered from PRE-OP. | Workaround, alpha.121 (#175): the master acknowledges the BUSY and waits for the file. It asks again after FoE error 0x800B. | Not checked. | A master that follows ETG.1000.5 fails the read. |
| [#5837](https://github.com/synapticon/somanet_software/issues/5837) | Integro | An FoE abort after a BUSY on a read, with no new request, leaves the drive in BOOT. It refuses INIT and every mailbox request until a power cycle. | Workaround, alpha.121 (#175): the master never aborts a read after a BUSY. | Not checked. | A master that aborts after a BUSY hangs the drive. |
| [#5838](https://github.com/synapticon/somanet_software/issues/5838) | All XMOS drives | When the last ACK of an FoE read does not reach the FoE handler, the next read request fails with FoE error 0x8004 "Missing reply". An AL state request within about 0.1 ms of that ACK loses it. The FoE state survives the state change. | Workaround, alpha.121 (#175): the master asks again after 0x8004. | Not checked. | The first read after such a state change fails on another master. |
| [jasper#712](https://github.com/synapticon/jasper/issues/712) | Jasper-based (C021) | 56 objects have another data type than in the SOMANET object dictionary. For example, 0x2110:03 is INTEGER32, 0x2003:05 is UNSIGNED16, and the 0x2013 gains are UNSIGNED32 instead of REAL32. | Partial, unreleased: a typed read of an integer object takes any integer type that the value fits in. A float, a string or a byte array must still have the expected type. | Not checked. | A client that trusts the SOMANET dictionary reads wrong values. |
| [jasper#694](https://github.com/synapticon/jasper/issues/694) | Jasper-based (C021) | A process-data watchdog trip in OP (AL status code 0x001B) drops the drive to Switch On Disabled with no error code in 0x603F, no bit in 0x1001, no CoE emergency and no log-book entry. 0x6007, the abort connection option code, does not exist. | None. The master cannot add the report. | None. | A customer who watches only the CiA 402 objects or the log book sees a drive that switched off for no reason. |
| [jasper#695](https://github.com/synapticon/jasper/issues/695) | Jasper-based (C021) | A Complete Access upload of a PDO mapping object always returns all 10 entries. The entries past the runtime value of subindex 0 hold uninitialised heap memory, such as earlier mailbox traffic or `config.csv` text. | Not checked. The master reads multi-subindex objects with Complete Access. | Not checked. | Not checked. |
| [jasper#704](https://github.com/synapticon/jasper/issues/704) | Jasper-based (C021, NGC) | When one LRD covers two read FMMU areas of the drive that are far apart, and the datagram goes on after the second area, the drive cuts the frame at 102 bytes. SOEM 2.x creates this layout when it maps the mailbox status register 0x080D of each slave into the cyclic inputs, and the drive is not the last slave with a mailbox. | Workaround: the driver turns off the mailbox status FMMU of every slave after each map, unless the configuration keeps it. | Not checked. | A master that keeps the mailbox status FMMU gets a wrong WKC on every cycle with two or more drives. |
| [jasper#711](https://github.com/synapticon/jasper/issues/711) | Jasper-based (C021) | The first PRE-OP to SAFE-OP after power-on takes about 3.9 s, because the application holds the step until its start-up settles. Later starts take about 10 ms. SOEM's default state timeout, `EC_TIMEOUTSTATE`, is 2 s. | No action needed yet: `POST /api/devices/state` waits 5 s unless the request gives a timeout. | Not checked. | A master that waits 2 s, such as SOEM's `ec_sample`, times out on the first start. |
| [jasper#682](https://github.com/synapticon/jasper/issues/682) | Jasper-based (C021) | An expedited SDO download with the size bit `s` = 0 counts as 4 bytes, so a write to a 1-byte or 2-byte entry is refused with abort 0x06070012. | No action needed: SOEM always sends `s` = 1, as the issue says. | No action needed, for the same reason. | A master that sends `s` = 0. No EtherCAT master that does is known. |

## 2. Firmware behaviour that is not filed

Motion Master works around each item below, and the changelogs record it. None of them has an issue. Each is a candidate for one.

| Device | Behaviour | Motion Master 6 | Motion Master 5 |
| --- | --- | --- | --- |
| Integro | A read of a free PDO mapping slot gets no answer. A Node answers in under 1 ms. A read of every parameter took 36 s. | Workaround, alpha.83: ask once and remember the failure. | Not checked. |
| Integro | A read of `0xF050:02`, an object it does not have, gets no answer instead of an abort. A Node aborts in about 150 ms. | Not checked. | Workaround, 5.7.0: read it only when the dictionary lists it. |
| Integro | The netX serves `0x10F8` over CoE, but OS command 21 leaves it out of its output. | Workaround, alpha.83: the master leaves `0x10F8` out of the split. | Not checked. |
| Integro | The netX accepts the ESI only in chunks named `.partNNN`, and refuses the single `.zip`. | Not checked. | Workaround, 5.5.6: a failed ESI write is a warning. |
| Integro | The netX restarts about 500 ms after it reports INIT on the step from BOOT, and clears its station address. After a com binary, the restart comes 0.7 s after INIT. | Workaround: the step to INIT gets 30 s and recovery of the station address. alpha.121 (#189): with `finalState` INIT, the firmware installation waits for the restart. | Not checked. |
| Jasper-based (C021) | The bootloader restarts the drive on the step from BOOT to INIT, and the restart clears the station address. A broadcast read sees INIT before the drive has its address back. | Workaround, alpha.121 (#189): a drive that can restart on its way out of BOOT counts as in a state only when it answers at its station address. It gets the address back as soon as it is found without it. | Not checked. |
| All XMOS drives | Get Object Description reports the runtime value of subindex 0 as the size of the object. For an unmapped `0x1603` that is 0, while the object holds ten entries. The notes of #5821 describe it. | Workaround, alpha.83: probe past the max subindex on a device that refuses correctly. | Not checked. |
| All drives after a bootloader restart | The station address is cleared on the step from BOOT to PRE-OP. | Workaround, alpha.66 and alpha.67: re-address the device by its position on the bus. | Not checked. |
| Drive with an SMM | The safety PDO mapping names `0x2001` in entries that the ESI declares as padding. Writes of `0x2001` go into the safety frame. | Workaround, alpha.91: on a SOMANET device, `0x2001` and `0x2007` in a PDO count as padding. | Not checked. |
| SOMANET drives | A shorter VISIBLE_STRING written over a longer one keeps the old bytes after its NUL. `0x2E00:06` read back as `0` followed by the rest of an old text. | Workaround, alpha.120: a read ends the string at its first NUL. | Not checked. |
| Jasper (ESC type 0x90) | The ESC discards the combined LRW datagram. About 95 % of the cyclic frames were lost. | Workaround: the driver uses separate LRD and LWR datagrams for every TI PRU-ICSS slave. | Workaround, 5.5.11: separate LRD and LWR datagrams. |
| Jasper with an older bootloader | The SII revision is the revision of the bootloader, whatever application runs. | Workaround, alpha.112: the dictionary cache is keyed on `0x100A`. | Not checked. |
| Jasper-based (C021) | The firmware has no 0x2009:03, the offset measurement method object. | Workaround, unreleased: offset detection runs without it, and the firmware chooses the method. | Not checked. |
| Jasper-based (C021) | The firmware holds 0 or 1 in 0x2110:0C and 0x2112:0C, where the ring size belongs. | Workaround, unreleased: a BiSS or internal encoder whose resolution is 2^18, 2^19 or 2^20 takes the exponent as its singleturn bits, with a warning. | Not checked. |
| Jasper-based (C021) | The firmware has no 0x2004, the brake object. | Workaround, alpha.116 (#160): a drive without 0x2004 counts as a drive with no brake. | Not checked. |
| Jasper firmware package | The package holds the ESI as `SOMANET_CiA_402.xml`, not as `SOMANET_CiA_402.xml.zip`. It also holds a `CHANGELOG.md`. | Workaround, alpha.121 (#191, #192): the installation reads either ESI name, and writes an extra file only when the request names it. | Not checked. |
| Firmware older than v5.6.0 | The mailbox counter survives INIT. Firmware v5.6.0 resets it, so this is fixed in the firmware. | Workaround, alpha.88 and alpha.99: send counter 0 on each INIT to PRE-OP. | Workaround, 5.8.5: send counter 0 after each scan. |
| Unknown firmware | SDO traffic to non-PDO objects starves the process data. The cause is not stated. | Not checked. | Workaround, 5.5.10: `--no-sdo-polling`. |
| SPoE | One firmware installation stopped while it wrote the application, and the drive refused every connection until a power cycle. The drive keeps one packet state for file reads, file writes and the parameter list. | Partial, alpha.89: file transfers to one drive run one after the other. The installation stop is not fixed. | Not checked. |

## 3. Motion Master 6 changes

A number in brackets is an issue or pull request in `synapticon/motion-master`, unless it starts with 58.

### Mailbox counters, late frames and lost frames

- alpha.92, FW and SOEM (#100): SOEM sent a late write again, and the Circulo locked SM0. Each request is now written once.
- alpha.93, FW (#113): the Integro dropped requests after the counters 0 and 1. The master keeps the sequence without a gap.
- alpha.93, SOEM (#113): a late frame was matched to a newer request by its 8-bit index alone. Every frame must now match the datagram header of its request.
- alpha.93, SOEM and FW (#113): on a late mailbox read, SOEM sent a Repeat Request, and the Integro returned the wrong message. The master now waits 100 ms for the frame and reads SM1 again.
- alpha.93, SOEM (#113): a mailbox error or emergency during a transfer crashed the server on a null pointer. The SOEM port is fixed.
- alpha.99, MM (#132): the master reused the counter of a write with no frame back. The late write arrived in between, and the Circulo locked. The counter of such a write is never used again.
- alpha.99, FW (#124): a lost write to an Integro left a counter gap. On a netX, ESC type 0x85, the master sends the write again.
- alpha.88 and alpha.99, FW: firmware older than v5.6.0 hung after about one rescan in seven. The master sends counter 0 on each INIT to PRE-OP, and once more when it gets no answer.
- alpha.99, wire (#127): one lost frame lost an SDO read. An unanswered read is tried once more. A write is never tried again, because a second write of `0x1023:01` starts the OS command again.

### FoE, bootloader and firmware installation

- alpha.91, FW (#5809): the timeouts are 6 s per read, 10 s per removal, 30 s for the ESI removal and 10 s per packet acknowledgement of a write. Every FoE failure with no answer ends with an FoE ERROR request.
- alpha.66, SOEM: a `FOE_BUSY` answer stalled a transfer, and a `FOE_BUSY` before the first packet reported success with nothing sent. The SOEM port is fixed.
- alpha.66 and alpha.67, FW: the device lost its station address after BOOT. The master re-addresses it by position, with no rescan.
- alpha.66, MM: a timed-out file transfer left an old answer in the mailbox. Each file operation now discards it first.
- alpha.94, MM (#115): an Integro installation failed at the exit from BOOT on a PC without a real-time kernel. The registers of a state change now wait 100 ms for their frame. 24 installations ran with 2 % of the frames 25 ms late.
- alpha.112, FW (#148, #5822): `fs-buffer` is read only after the command completes.
- alpha.121, FW (#175, #5836, #5837, #5838): an Integro answered a read with FoE BUSY, and an abort after it could leave the drive in BOOT until a power cycle. The read acknowledges the BUSY and waits for the file. It asks again after 0x800B and 0x8004.
- alpha.121, FW (#189): a firmware installation on a Jasper-based drive failed with "the device reported UNKNOWN rather than INIT", because the bootloader restart cleared the station address. The master counts the state only at the station address and gives the address back.
- alpha.121, FW (#189): an Integro with `finalState` INIT was off the bus for up to 16 s after a reported success. The `final-state` step waits for the netX restart.
- alpha.121, FW and MM (#191, #192): an install of a Jasper package wrote `SOMANET_CiA_402.xml` and `CHANGELOG.md` to the flash, and stored no ESI. An extra file is written only when the request names it, and the ESI is read under either name.

### Object dictionary

- alpha.83, FW: the dictionary was read short of its free PDO mapping slots. The master probes past the max subindex.
- alpha.112, FW (#145, #5821): an Integro is not probed.
- alpha.83, FW: a read of every parameter of an Integro took 36 s. It now takes about 3 s.
- alpha.83, FW: OS command 21 values after `0x10F8` were shifted on an Integro. `0x10F8` is left out.
- alpha.83, MM: a late answer freed one mailbox buffer twice, and in the end every transfer failed. Fixed.
- alpha.83, MM: a dictionary read with lost entries was cached for good. It is not cached now.
- alpha.74, MM: an old answer for another subindex was taken as the answer. The master checks the index and the subindex.
- alpha.73 and alpha.76, SOEM: the shared error list reported an old reason against another object or drive. The master empties it.
- alpha.112, FW: Jasper v10.0.4 leaves subindices out of a record. Each one was asked 11 times. An answer of "does not exist" is now final after one request.
- alpha.112, FW: an older Jasper bootloader reports the wrong SII revision. The cache is keyed on `0x100A`.
- alpha.116, FW (#160): Jasper has no 0x2004, and every procedure that touched the brake failed. A drive without 0x2004 counts as a drive with no brake.
- alpha.120, FW: a VISIBLE_STRING read returned the old bytes after the NUL. A read ends at the first NUL.
- Unreleased, FW (jasper#712): Jasper declares 56 objects with another type, and the tuning and integral-limit procedures failed with "holds a different type". An integer read takes any integer type that the value fits in.
- Unreleased, FW: Jasper has no 0x2009:03, and the offset detection refused to start. It runs without the object.
- Unreleased, FW: Jasper holds no ring size in 0x2110:0C and 0x2112:0C. The iC-MU procedures take the ring from the resolution.

### AL state and process data

- alpha.97, SOEM (#119): `ecx_readPDOassign` takes a lost read for an empty mapping, so a late frame mapped a device short. A failed read now fails the map, and the map gets three attempts.
- alpha.20, SOEM: a new map after BOOT crashed. Illegal AL state changes are refused, and the FMMU state is reset before each map.
- alpha.91, FW: the SMM safety PDO names `0x2001`. On SOMANET, `0x2001` and `0x2007` count as padding.
- alpha.91, MM: the expected WKC in SAFE-OP was 1, not 3, so a device that stopped its outputs looked healthy.
- alpha.91, MM: the inputs of a device below SAFE-OP were overwritten every cycle. That device is now skipped.
- alpha.71, MM: a fault reset worked only once, because controlword bit 7 stayed high. The master sets it low first.
- alpha.89, FW (SPoE): known limits, not fixed. An installation can stop and leave the drive unreachable until a power cycle. A delete reports success while the file stays.

## 4. Motion Master 5 changes

- 5.8.7, MM: after a link loss, a WKC of 46849 was stored as the device count, and the master wrote past the device list. A count that no bus can have is ignored.
- 5.8.7, FW: Jasper v10.0.4 leaves subindices out of a record. The dictionary read took up to 90 s and was not cached. The aborts 0x06090011 and 0x06020000 are now final.
- 5.8.7, FW: `0x2110:0A` is missing on an NGC with Jasper v10.0.4, and narrow angle calibration crashed. The encoder counts as singleturn.
- 5.8.6, FW (#5809): firmware installation on a drive with no valid firmware stopped at about 85 %. The read timeout is 6 s, with three attempts and an FoE ERROR request after each failure.
- 5.8.5, FW (#5811, after a reconnect): a SensoJoint on v5.1.0 locked its mailbox. The master sends counter 0 after each scan.
- 5.8.4, MM: a device that stopped answering held init for minutes. Init now stops, and the port is opened again. A second master (IgH `ec_master`) on the same adapter is detected.
- 5.8.2, SOEM: `ecx_readOE` hid lost and late answers, so `0x2011:04` on a Circulo NG read as REAL32. Each subindex is checked on its own.
- 5.7.0, FW: the Integro does not answer `0xF050:02`, which cost about 8 s per device. The master reads it only when the dictionary lists it.
- 5.6.4, SOEM: a 20 ms timeout counted too few devices. It is 100 ms.
- 5.6.3, SOEM: the PDO assignment read did not check the WKC, so `0x1602` was dropped and SAFE-OP failed with 0x001D. Each read is retried, and `EC_TIMEOUTRET` is 100 ms.
- 5.6.2, MM: an SDO write of 0 into placeholder entries erased the PDO mapping. Fixed.
- 5.6.2, FW and SOEM: an emergency from the drive was taken as the answer to another transfer. The master discards it and empties the error list.
- 5.6.2, MM: two threads received each other's mailbox answers. One mutex now guards the mailbox.
- 5.5.11, MM: after a brown-out, the devices were never set up again. A full init now runs.
- 5.5.11, FW: Jasper discards the LRW datagram. The master sends LRD and LWR.
- 5.5.10, FW: on some firmware, SDO traffic starved the process data. `--no-sdo-polling` was added.
- 5.5.9, MM: an object mapped outside 0x1600 to 0x1AFF, such as `0x20F0`, was not seen as mapped. The master reads 0x1C12 and 0x1C13.
- 5.5.7 and 5.5.8, MM: PDO mapping writes were not retried or checked. They are retried three times and read back.
- 5.5.6, FW: the Integro refuses the ESI as one `.zip`. A failed ESI write is a warning.
- 5.5.5, MM: FoE removal timeouts were too short, and the counter was not reset in BOOT. The timeouts are 10 s and 30 s, with a 2 s wait after BOOT.
- 5.5.4, MM: some devices stayed in INIT. The mailbox SyncManagers are set again only after BOOT, and the EEPROM goes back to the PDI.
- 5.5.2, MM: a file goes in one FoE transfer. This change caused the 0x0008 regression of #5809, fixed in 5.8.6.
- 5.0 to 5.2: about 15 more fixes, such as BOOL SDO writes, recovery of an Integro from SAFE-OP with an error, and SPoE state checks. Most of them do not state a cause.

## Sources

- The issues in `synapticon/somanet_software` and `synapticon/jasper`.
- `CHANGELOG.md` in this repository.
- `CHANGELOG.md` in `synapticon/motion_master`.

The change lists are summaries. The changelog text is the reference.
