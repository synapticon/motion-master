# How a Drive Starts New Firmware

This document is for developers who work on the firmware installation or on the AL state
handling of the server. It tells when each drive family starts the firmware that the
installation wrote, and what the master must do at that moment.

## Terms

- **AL state** (Application Layer state): the EtherCAT state of a device. The states are INIT,
  PREOP, BOOT, SAFEOP, and OP.
- **BOOT**: the AL state in which the bootloader of the device runs. The installation writes the
  firmware files in BOOT, over FoE (File access over EtherCAT).
- **Station address**: the address the master gives to each device when it scans the bus. The
  master reads and writes a device at this address.

## The installation in short

1. The master moves the device from INIT to BOOT.
2. The master writes the application binary over FoE. Some devices also get a COM binary and an
   SII.
3. The master moves the device out of BOOT. The AL state machine allows only BOOT to INIT, so the
   device goes to INIT first, then to PREOP.

The new firmware starts in step 3. Each drive family starts it at a different transition.

## When the new firmware starts

| Drive family | BOOT to INIT | INIT to PREOP |
| --- | --- | --- |
| Jasper-based drives (C021, NGC) | The bootloader checks the image and does a warm reset into it. The device reports INIT after the reset. | An ordinary transition. |
| SOMANET drives with one processor (Circulo, Node) | An ordinary transition. The bootloader stays in control. | The bootloader starts the application. This restarts the device. |
| SOMANET drives with two processors (Integro, ACTILINK-S) | When the installation wrote a COM binary, the netX restarts after the device reports INIT. | The drive processor restarts into the new application. The netX does not restart. |

### Jasper-based drives

The bootloader runs on the AM2612. When the device leaves BOOT for INIT, the bootloader checks
the image in flash. It then does a warm reset, and the second-stage bootloader starts the new
image.

The device reports INIT only after the reset. On a C021, the device reported INIT about 1.9 s
after the INIT request. During that time the device does not answer at its station address.

Source: `platforms/AM2612/ecat_bootloader/README.md` in the `jasper` repository.

### SOMANET drives with one processor

The bootloader runs on the XMOS. With a valid image in flash, the bootloader waits until the
master asks for PREOP. It then starts the application, and this restarts the device.

The device keeps its station address through the step to INIT. If no valid application starts,
the device answers the PREOP request with AL status code 0x0014.

Source: `bootloader_manager.xc` in the `somanet_bootloader` repository. The log message of the
bootloader is "Firmware found. Waiting for PREOP command from master."

### SOMANET drives with two processors

These drives have a netX chip and a drive processor. The netX runs the EtherCAT stack, and the
COM binary is its firmware. The application binary is the firmware of the drive processor.

1. **BOOT to INIT.** When the installation wrote a COM binary, the boot code of the netX requests
   a reset 500 ms after it reports INIT. The reset clears the station address. On the bench, the
   netX stopped answering 0.7 s after the INIT request. It stayed off the bus for 4 s to 19 s. A
   longer time came with a different build of the netX firmware.
2. **INIT to PREOP.** The drive processor restarts into the new application. The netX keeps
   running.

When the installation wrote no COM binary, the netX did not restart on the bench.

Source: `AppECS_Functions_Boot.c` (`Boot_Handle_ESM_AL_Control_Changed_Ind`) in the
`somanet_software` repository.

## What the master must do

- **Wait long enough for each transition out of BOOT.** Each of the two transitions can include a
  restart. The server allows 30 s for each (`kBootExitTimeout` in
  `libs/node/somanet_procedures.cc`). The later steps to SAFEOP and OP get 10 s
  (`kStateTimeout`).
- **Accept a device that does not answer at its station address for a time.** A Jasper-based
  drive and an Integro lose the station address in their restart. The driver counts a device as in
  a state only when it answers at its station address. It gives the address back before the next
  request.
- **Watch for the netX restart after INIT.** After an installation with a COM binary, the server
  watches the device for 2 s after INIT (`kComRestartWindow`). When the device stops answering,
  the server waits up to 30 s for it to answer again. It then requests INIT again.
- **Do not assume where the restart happens.** A procedure that leaves BOOT must handle a restart
  at either transition. The table above shows that the drive families differ.

The function `leaveBoot` in `libs/node/somanet_procedures.cc` implements these steps. Issue #189
has the measurements on the C021, the Integro, and the Circulo.
