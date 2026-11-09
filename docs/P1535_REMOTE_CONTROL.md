# P 1535 Remote Control

`hil/p1535` is firmware for an ESP32 board. It lets a computer switch the output of a PeakTech
P 1535 laboratory power supply on and off over HTTP. A test script can then cut and restore the
power of a drive on the bench.

This guide is for the person who sets up the bench. It covers the build, the flash, the network,
and the use of the endpoints.

**The endpoints have no authentication.** Any device on the same network can switch the output.

## How it works

The power supply has a remote connector. Its output is off when pin 5 of that connector is
connected to pin 4 (ground). Its output is on when pin 5 is open. The remote control works in the
Normal, Preset, Remote and Set modes of the power supply.

The ESP32 controls that connection through GPIO 18:

| GPIO 18 | Power supply output |
| --- | --- |
| High | On |
| Low | Off |

This guide does not describe the circuit between GPIO 18 and the connector.

When the output is off, the C.V. and C.C. LEDs of the power supply flash. The panel meter shows the
voltage and current settings. You can change both settings with the knobs while the output is off.

The ESP32 runs an HTTP server on port 80. It joins a WiFi network as a station with a static
address. It does not use DHCP.

## What you need

- The ESP32 board with the firmware circuit, and a USB cable.
- A Linux computer with `git`, `sudo` rights, and the
  [ESP-IDF prerequisites](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/linux-macos-setup.html).
  ESP-IDF is the Espressif build system for the ESP32.
- The password of the WiFi network.
- An address on that network that is outside the DHCP pool of the router. Otherwise the router can
  give the same address to another device.

The ESP32 chip on the board is not recorded. The scripts build for `esp32`, which is also the
default target of `idf.py`. Run `idf.py set-target <chip>` once if the board has a different chip.

## Scripts

All scripts are in `hil/p1535`. Run them from any directory. Each script loads the ESP-IDF
environment itself, so you do not run `export.sh` first.

| Script | Does | When |
| --- | --- | --- |
| `setup.sh` | Install ESP-IDF, and give your user access to the serial port | Once per computer |
| `configure.sh` | Open the configuration menu: WiFi and address | Once, and after each change of network |
| `run.sh` | Build, flash, and open the serial monitor | After each change of the firmware or the configuration |
| `power.sh on` | Turn the power supply output on | Any time |
| `power.sh off` | Turn the power supply output off | Any time |

`env.sh` holds what the other scripts share: the ESP-IDF version and the search for the serial
port. You do not run it.

These environment variables change what the scripts use:

| Variable | Default | Used by |
| --- | --- | --- |
| `IDF_PATH` | `~/esp/esp-idf` | `setup.sh`, `configure.sh`, `run.sh` |
| `P1535_PORT` | The first CP210x USB serial adapter, else `/dev/ttyUSB0` | `setup.sh`, `run.sh` |
| `P1535_HOST` | `10.10.1.250` | `power.sh` |

## Set up the computer

Do this once on each computer that flashes the board.

1. Connect the ESP32 to the computer with the USB cable.

2. Run the setup script. It asks for your `sudo` password.

   ```bash
   hil/p1535/setup.sh
   ```

The script does these steps. It skips a step that is already done, so you can run it again.

1. It downloads ESP-IDF to `~/esp/esp-idf`, at the version in `env.sh`.
2. It checks the Python version. ESP-IDF uses the first `python3` on `PATH`. When that is newer
   than 3.13, the scripts give ESP-IDF `python3.13` instead. On Fedora the script installs
   `python3.13` with `dnf`. On other systems, install it yourself.
3. It installs the ESP32 toolchain to `~/.espressif`.
4. It adds your user to the `dialout` group. This takes effect at your next login.
5. It gives your user access to the serial port of the board until you unplug it. So the first
   flash works without a new login.

## Configure

The firmware reads its network settings from the ESP-IDF configuration. These are the defaults:

| Setting | Default |
| --- | --- |
| SSID | `Wifi Office` (the network of Work Space One) |
| Password | Empty |
| Address | `10.10.1.250` |
| Gateway | `10.10.1.1` |
| Netmask | `255.255.255.0` |

1. Open the configuration menu. Run it in a terminal, because the menu is interactive.

   ```bash
   hil/p1535/configure.sh
   ```

2. Open **P1535 remote control**.

3. Set **WiFi password**.

4. Change the other settings if your network is different.

5. Press `S` to save, and `Q` to exit the menu.

The menu writes the settings to `hil/p1535/sdkconfig`. Git ignores that file, because it holds the
WiFi password and this repository is public. Never commit the password.

## Build and flash

1. Connect the ESP32 to the computer with the USB cable.

2. Build, flash, and open the serial monitor. Run it in a terminal, because the monitor is
   interactive.

   ```bash
   hil/p1535/run.sh
   ```

   The script stops if no WiFi password is set. Run `configure.sh` first.

   For a port that the script does not find, set `P1535_PORT`:

   ```bash
   P1535_PORT=/dev/ttyUSB1 hil/p1535/run.sh
   ```

3. Read the serial monitor. A good start shows these lines:

   ```text
   I (...) wifi station: got ip:10.10.1.250
   I (...) wifi station: connected to ap SSID:Wifi Office, IP:10.10.1.250
   I (...) http server: HTTP server started
   ```

4. Press `Ctrl+]` to close the monitor. The ESP32 keeps running.

## Use

The computer must be on the same network as the ESP32.

| Request | Does |
| --- | --- |
| `GET /` | Show a page that describes the endpoints |
| `GET /output-on` | Turn the output on |
| `GET /output-off` | Turn the output off |

Use the script:

```bash
hil/p1535/power.sh off
hil/p1535/power.sh on
```

For a board with another address, set `P1535_HOST`:

```bash
P1535_HOST=10.10.1.251 hil/p1535/power.sh off
```

Or call the endpoints directly:

```bash
curl http://10.10.1.250/output-off
curl http://10.10.1.250/output-on
```

Each request returns a short HTML page. You can also open `http://10.10.1.250/` in a browser.

The firmware sets no level on GPIO 18 at start. It configures the pin as an output only after the
WiFi connection attempt ends. The state of the power supply output after a restart of the ESP32 is
not documented. Send `/output-on` or `/output-off` after each restart to put it in a known state.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| The log shows `Failed to connect to SSID` | The SSID and the password in the configuration. Most ESP32 chips support 2.4 GHz WiFi only, so the network must offer 2.4 GHz. |
| The log shows `got ip`, but `curl` gets no answer | The computer is on the same network. No other device uses the address. |
| `idf.py` cannot open the serial port | The port name. Run `setup.sh` again after you plug the board in, or log in again after the first `setup.sh`. |
| `setup.sh` fails with `Python.h: No such file or directory` for `tree_sitter` | ESP-IDF ran with a Python newer than 3.13. Run `setup.sh` again. It switches to `python3.13`. |
| A script says `ESP-IDF is not installed` | Run `setup.sh`, or set `IDF_PATH` to your ESP-IDF directory. |

After a fixed number of failed connection attempts, the firmware stops the attempts. It still starts
the HTTP server, but nothing can reach it. Reset the ESP32 to try again.
