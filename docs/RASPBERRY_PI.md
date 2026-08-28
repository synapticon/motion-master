# Motion Master on a Raspberry Pi 5

Motion Master ships as a card image for the Raspberry Pi 5. Write it to a microSD card, tell it
which Wi-Fi network to join, and it comes up as an EtherCAT master with a real-time kernel, ready
to drive the drives wired to its Ethernet port.

**Motion Master has no authentication.** Anything that can reach it over the network can enable a
drive and command motion. Put the board on a network you trust.

## What you need

- A Raspberry Pi 5. The image is built for it and will not boot a Pi 4.
- A microSD card, 8 GB or larger. The board grows the filesystem to fill whatever you use.
- A monitor and a USB keyboard, for one step. You can skip this if your router shows you the
  board's address in its list of leases.
- A Wi-Fi network. **The Ethernet port carries EtherCAT**, so it is not how you reach the board.

## 1. Download

```text
https://dezliul92qqoq.cloudfront.net/motion-master-rpi5.img.xz
```

About 1.3 GB compressed, 8 GB written to the card. Do not unpack it — the tools below read
`.xz` directly.

A checksum is published beside it, at the same address with `.sha256` on the end. To check the
download on Linux or macOS:

```bash
sha256sum -c motion-master-rpi5.img.xz.sha256
```

## 2. Write the card

### Windows

Use [Raspberry Pi Imager](https://www.raspberrypi.com/software/).

1. **Choose device** — Raspberry Pi 5.
2. **Choose OS** — scroll to the bottom, **Use custom**, and pick the `.img.xz` file.
3. **Choose storage** — your card. Check it twice; this erases the card.
4. **Next**. When it offers to apply OS customisation settings, choose **No**. This image is not
   Raspberry Pi OS and those settings do not apply to it.

[balenaEtcher](https://etcher.balena.io/) works too, and also reads `.xz`.

### Linux

Raspberry Pi Imager and balenaEtcher both run on Linux and are the safe choice. To do it from a
terminal instead, find the card first:

```bash
lsblk -o NAME,SIZE,TYPE,RM,MOUNTPOINTS,MODEL
```

Look for a removable disk the size of your card. Then, with `sdX` replaced by that name:

```bash
xzcat motion-master-rpi5.img.xz | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
```

**Check the device name before you press enter.** `dd` overwrites whatever you point it at,
including the disk you are working on.

## 3. Tell it your Wi-Fi

Take the card out and put it back in, so your computer reads the freshly written partitions.
A drive named **MM-BOOT** appears. Open it.

> On Linux you may see two drives. `MM-BOOT` is the one you want. The other is the board's root
> filesystem, and a file placed there is hidden as soon as the board starts. Windows and macOS
> only offer `MM-BOOT`.

Open `wifi.txt` in a text editor and remove the `#` from two lines:

```ini
ssid=YourNetwork
psk=YourPassword
```

There is a third line for a country code, `country=DE` or `country=US`. It is optional. Without
it the board uses a worldwide default, where some channels are unavailable.

Save the file, eject the card, and put it in the board.

You can come back to this file at any time. It is read on every boot, so editing it and restarting
moves the board to another network.

## 4. Find the board's address

Connect a monitor and a keyboard, power the board on, and wait for the login prompt. Log in:

```text
login: root
password: root
```

Then:

```bash
ip -br addr
```

Read the address next to `wlan0` — something like `192.168.0.81/24`. That is the board.

**If `wlan0` has no address**, the Wi-Fi settings did not take. Fix them here rather than
reflashing:

```bash
setup-wifi YourNetwork YourPassword DE
```

That writes the same file on the card, so it survives the next boot.

You can unplug the monitor and keyboard now. You will only need them again if the board loses its
network.

## 5. Reach it

Two ways in, and which you want depends on what you are doing.

### From a program or a script — nothing to set up

Talk to the board directly at its address. **No hosts file, no certificate step.**

| Port | Protocol |
| --- | --- |
| `61447` | HTTP API |
| `62281` | WebSocket — monitoring streams and notifications |

```bash
curl -k https://192.168.0.81:61447/api/version
```

The board serves HTTPS with a certificate issued for a hostname, not for a bare address, so a
plain address means the certificate does not match. For a script that is a flag — `-k` for curl,
`rejectUnauthorized: false` for Node. Only a browser makes it a wall.

### From the Console — one line per computer

The Console is a web page, and a browser will not let you past a certificate that does not match
the address. So you reach the board under a name the certificate does cover:

```text
192.168.0.81   ->   192-168-0-81.ip.motion-master.synapticon.com
```

Those names are deliberately absent from public DNS, so each computer maps one locally. A script
in the repository writes the line:

```bash
sudo ./add-host.sh 192.168.0.81          # Linux, macOS
```

```powershell
.\add-host.ps1 192.168.0.81              # Windows, in an elevated PowerShell
```

Then open the Console at [motion-master.synapticon.com](https://motion-master.synapticon.com) and
enter the board's address on the **Connection** page.

This is not a security compromise. TLS checks a certificate against the *name*, never against how
that name was resolved, so what you get is ordinary trusted HTTPS with nothing to click through.
It does have to be done on **every computer that opens the Console** — the lookup happens at the
browser's end, so an entry on the board itself achieves nothing.

Full detail, including what to do on a machine where you cannot edit the hosts file, is in
[LAN_DEPLOYMENT.md](LAN_DEPLOYMENT.md).

### Over SSH

```bash
ssh root@192.168.0.81
```

The password is `root`.

## 6. Keeping it up to date

On the board:

```bash
update-motion-master 6.0.0-alpha.85
```

It downloads that release, installs it, and restarts the service. Your configuration is untouched,
so the board keeps its adapter, its cycle period and its pinned core. Released versions are listed
at [the releases page](https://github.com/synapticon/motion-master/releases).

The version is named rather than picked up automatically. A board driving hardware should move when
you decide it moves, not because a release happened.

To go back, name the older version. Reflashing the card is never needed for a version change.

## Checking it works

```bash
curl -k https://192.168.0.81:61447/api/devices
```

With drives wired to the Ethernet port and the bus started, that lists them. `GET /api/game-loop`
reports the real-time cycle — `schedFifo`, `memLocked` and `cpuPinned` should all be `true`, and
`skippedCycles` should stay at zero.

If the server is not answering at all, log in and ask systemd:

```bash
systemctl status motion-master
```

## What is on the board

- Debian 14 (forky) with a `PREEMPT_RT` kernel.
- One CPU core isolated from the scheduler, with the real-time thread pinned to it.
- Motion Master as a systemd service, started at boot, bound to every interface.
- `setup-wifi` and `update-motion-master`, both described above.

How that image is built, and how to build your own, is in [rt/README.md](../rt/README.md).
