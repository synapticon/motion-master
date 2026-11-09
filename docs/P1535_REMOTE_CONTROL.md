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
- A Linux computer with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/linux-macos-setup.html)
  installed. ESP-IDF is the Espressif build system for the ESP32.
- The password of the WiFi network.
- An address on that network that is outside the DHCP pool of the router. Otherwise the router can
  give the same address to another device.

The ESP-IDF version that the firmware was last built with is not recorded. The ESP32 chip on the
board is not recorded either. The steps below use `esp32`, which is also the default target of
`idf.py`. Run `idf.py set-target <chip>` once if the board has a different chip.

## Install ESP-IDF

Do these steps once on the computer that flashes the board.

1. Install ESP-IDF. Follow the Espressif guide that is linked above.

   ```bash
   mkdir -p ~/esp
   cd ~/esp
   git clone --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf
   ./install.sh esp32
   ```

2. Give your user access to the serial port. Log out and log in again after this command.

   ```bash
   sudo usermod -aG dialout "$USER"
   ```

Load the ESP-IDF environment in each new shell before you use `idf.py`:

```bash
. ~/esp/esp-idf/export.sh
```

## Configure

The firmware reads its network settings from the ESP-IDF configuration. These are the defaults:

| Setting | Default |
| --- | --- |
| SSID | `Wifi Office` (the network of Work Space One) |
| Password | Empty |
| Address | `10.10.1.250` |
| Gateway | `10.10.1.1` |
| Netmask | `255.255.255.0` |

1. Go to the firmware directory.

   ```bash
   cd hil/p1535
   ```

2. Open the configuration menu.

   ```bash
   ./configure.sh
   ```

3. Open **P1535 remote control**.

4. Set **WiFi password**.

5. Change the other settings if your network is different.

6. Save, and exit the menu.

The menu writes the settings to `hil/p1535/sdkconfig`. Git ignores that file, because it holds the
WiFi password and this repository is public. Never commit the password.

## Build and flash

1. Connect the ESP32 to the computer with the USB cable.

2. Find its serial port. It is usually `/dev/ttyUSB0`.

   ```bash
   ls /dev/ttyUSB*
   ```

3. Build, flash, and open the serial monitor. `run.sh` uses `/dev/ttyUSB0`.

   ```bash
   ./run.sh
   ```

   For a different port, run `idf.py` yourself:

   ```bash
   idf.py -p /dev/ttyUSB1 flash monitor
   ```

4. Read the serial monitor. A good start shows these lines:

   ```text
   I (...) wifi station: got ip:10.10.1.250
   I (...) wifi station: connected to ap SSID:Wifi Office, IP:10.10.1.250
   I (...) http server: HTTP server started
   ```

5. Press `Ctrl+]` to close the monitor. The ESP32 keeps running.

## Use

The computer must be on the same network as the ESP32.

| Request | Does |
| --- | --- |
| `GET /` | Show a page that describes the endpoints |
| `GET /output-on` | Turn the output on |
| `GET /output-off` | Turn the output off |

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
| `idf.py` cannot open the serial port | The port name, and that your user is in the `dialout` group. |
| `idf.py: command not found` | Load the ESP-IDF environment with `export.sh`. |

After a fixed number of failed connection attempts, the firmware stops the attempts. It still starts
the HTTP server, but nothing can reach it. Reset the ESP32 to try again.
