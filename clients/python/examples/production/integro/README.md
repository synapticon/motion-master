# Production script for an Integro drive

`commission.py` commissions one SOMANET Integro drive in a fixed order of steps. It writes a
log and a record of what it did. Use it at the production line, so that every drive gets the
same configuration.

The script calls the routes and the procedures of the Motion Master server, and shows their
results. It decides nothing that the server decides. Each step is also available in the Console.

## Before you start

1. Set up the Python client as [the client README](../../../README.md#setup) says.
2. Start the Motion Master server.
3. Connect the drive to the bus adapter that `config.toml` names.

## Run

Run the script in the directory that holds the files for the drive. The script writes its log
and its record into `logs/` beside the script, whatever directory it runs in.

```bash
cd <directory for the drive>
python <path to motion-master>/clients/python/examples/production/integro/commission.py
```

`--slave` gives the position of the drive on the bus. The default is 1.

```bash
python commission.py --slave 2
```

**The running server is shared.** The script opens the fieldbus driver, scans, and moves the
drive to other AL states (Application Layer states, the EtherCAT state machine of a device). A
Console session that uses the same server sees every change.

## Files the script writes

The files go to `clients/python/examples/production/integro/logs/`. Git ignores that directory.

| File | Content |
| --- | --- |
| `production-<time>.log` | Every message of the script with its time: each question, each answer and each result. |
| `<build descriptor>_<serial number>-<time>.txt` | The record of the drive. |

`<time>` is the start time of the script, as `YYYYMMDD-HHMMSS`. The build descriptor and the
serial number come from the hardware description of the drive. When the script stops before it
reads them, the record is named `device-<slave>-<time>.txt`.

The script writes the record also when a step fails, with the results it has so far. The first
lines of the record give the drive name, the start time and the outcome.

## Steps

The script runs step 1 only. The other steps follow one at a time.

### Connect

Before the first step, the script opens the fieldbus driver when no driver is open. It scans
when the last scan is missing or wrong. It moves the drive to PRE-OP. A drive in SAFE-OP or OP
stays there.

### 1. Hardware description

The script reads the `.hardware_description` file of the drive over FoE (File access over
EtherCAT), and the firmware version from object 0x100A. It shows the device and the assembly,
each with its name, build descriptor, serial number and components.

After a power cycle, an Integro refuses FoE reads for about 2.5 s after it reaches PRE-OP
([#175](https://github.com/synapticon/motion-master/issues/175)). The script asks again every
0.5 s for up to 10 s, and logs each refusal. After 10 s, the step fails with the message of the
server.
