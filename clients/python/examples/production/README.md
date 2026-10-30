# Production scripts

A production script commissions one SOMANET drive in a fixed order of steps. It writes a log
and a record of what it did. Use it at the production line, so that every drive gets the same
configuration.

Each kind of drive has its own script:

| Script | Drive |
| --- | --- |
| `integro/commission.py` | Integro |

A script for the Circulo NG, which runs the Jasper firmware, follows in `jasper/`. Code that the
scripts share is in `common.py`.

A script calls the routes and the procedures of the Motion Master server, and shows their
results. It decides nothing that the server decides. Each step is also available in the Console.

## Before you start

1. Set up the Python client as [the client README](../../README.md#setup) says.
2. Start the Motion Master server.
3. Connect the drive to the bus adapter that `config.toml` names.

## Files the script reads

| File | Content |
| --- | --- |
| `packages/*.zip` | Firmware packages, for every kind of drive. Step 2 lists them. |
| `<drive>/config.csv` | The configuration of one kind of drive, for example `integro/config.csv`. Step 3 loads it. |

Git ignores both, so each production line puts its own files there. The script reads nothing
from the directory it runs in.

## Run

```bash
python clients/python/examples/production/integro/commission.py
```

`--slave` gives the position of the drive on the bus. The default is 1.

```bash
python clients/python/examples/production/integro/commission.py --slave 2
```

**The running server is shared.** The script opens the fieldbus driver, scans, and moves the
drive to other AL states (Application Layer states, the EtherCAT state machine of a device). A
Console session that uses the same server sees every change.

## Files the script writes

The files go to `logs/` beside the script, for example
`clients/python/examples/production/integro/logs/`. Git ignores every `logs/` directory here.

| File | Content |
| --- | --- |
| `<build descriptor>_<serial number>-<time>.log` | The log. Every message of the script with its time: each question, each answer and each result. |
| `<build descriptor>_<serial number>-<time>.txt` | The record of the drive. |

The two files of one run have the same name, so they sort together. `<time>` is the start time of
the script, as `YYYYMMDD-HHMMSS`. The build descriptor and the serial number come from the
hardware description of the drive. Until the script reads them, the log is named
`device-<slave>-<time>.log`. The script renames it as soon as step 1 reads them. When the
script stops before that, both files keep the name `device-<slave>-<time>`.

The script writes the record also when a step fails, with the results it has so far. The first
lines of the record give the drive name, the start time and the outcome.

## Steps

Each step is described once. The table shows which script runs it.

**After each step that changes the object dictionary of the drive, the script stores the
parameters to flash** with the `store-parameters` procedure. A later step that fails then leaves
the drive with what the steps before it did, also after a power cycle. The drive writes every
value it holds into its `config.csv`, so the stored file is what the drive runs. Its comments
carry the firmware version of the drive.

| Step | Integro |
| --- | --- |
| Connect | Yes |
| 1. Hardware description | Yes |
| 2. Firmware | Yes |
| 3. Configuration | Yes |
| 4. Assigned name | Yes |

The other steps follow one at a time.

### Connect

Before the first step, the script opens the fieldbus driver when no driver is open. It scans
when the last scan is missing or wrong. It moves the drive to PRE-OP. A drive in SAFE-OP or OP
stays there.

A drive with no running firmware does not reach PRE-OP. The script logs why, and moves the drive
to BOOT instead. In BOOT, the bootloader of the drive serves the files that step 1 reads, and
the firmware installation writes the firmware there.

### 1. Hardware description

The script reads the `.hardware_description` file of the drive over FoE (File access over
EtherCAT), and the firmware version from object 0x100A. It shows the device and the assembly,
each with its name, build descriptor, serial number and components.

The script reads 0x100A only when the drive is in PRE-OP, SAFE-OP or OP, because only then does
the firmware run. In BOOT, the record says that no firmware runs.

After a power cycle, an Integro refuses FoE reads for about 2.5 s after it reaches PRE-OP
([#175](https://github.com/synapticon/motion-master/issues/175)). The script asks again every
0.5 s for up to 10 s, and logs each refusal. After 10 s, the step fails with the message of the
server.

### 2. Firmware

The script shows the descriptors that the drive accepts, for example `9010-05-2423-1`. A package
fits the drive when its file name carries one of them. Then the script lists the packages in
`packages/`, and the server says for each one whether it fits the drive.

The user types the number of a package, `s` to skip the step, or `x` to exit. A drive with no
running firmware cannot skip the step, because every later step needs the firmware. A package
that does not fit is installed only after a second confirmation, because the installation writes
whatever it gets.

The script uploads the package to the user cache of the server, and runs the
`firmware-installation` procedure with the final state PRE-OP. The procedure moves the drive to
BOOT, writes the firmware files over FoE, and moves the drive back to PRE-OP. The drive exchanges
no process data during the installation. The script shows each step of the procedure, then reads
the firmware version again. The record gets the version before, the package, the verdict of the
server and the version after.

### 3. Configuration

The script shows the `config.csv` beside it: its size, its number of rows and its comments. The
drive writes its name and its firmware version into those comments when it saves the file. The
script also shows the firmware version of the drive. The user types `y` to load the file, `s` to
skip the step, or `x` to exit. With no file there, the user skips the step or exits.

The script runs the `config-file-load` procedure of the server with the strategy `replace`. The
server checks every row against the object dictionary of the drive before it writes anything,
writes the file as the drive's `config.csv`, and makes the drive load it through 0x1011:01. Then
it puts each loaded value of an object in the outputs of the process image into that output, so
that the next cycle does not send the old value, and reads every loaded object back.

The drive must be in Switch On Disabled or Fault, the two CiA 402 states in which the drive
accepts a load. In any other state the server refuses the load, and the step fails.

Then the script stores the parameters to flash. The record gets the file name, the number of
rows loaded, the rows the drive skipped, and each object whose value read back differs from the
file. The drive loads `.assembly_config` after `config.csv`, so a value of the assembly can win
over the file. The whole file goes to the end of the record.

### 4. Assigned name

The script shows the assigned name of the drive, object 0x20F2, and asks for a new one. An empty
answer keeps the name and skips the step. The script writes the name, reads it back, and stops
when the drive holds something else. Then it stores the parameters to flash. The record gets the
name before and after.

Step 3 sets the assigned name too, when `config.csv` has a row for 0x20F2. Step 4 comes after it,
so the name the user types wins.
