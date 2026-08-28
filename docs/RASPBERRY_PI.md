# Motion Master on a Raspberry Pi 5

Motion Master ships as a card image for the Raspberry Pi 5. Write it to a microSD card, tell it
which Wi-Fi network to join, and it comes up as an EtherCAT master with a real-time kernel, ready
to drive the drives wired to its Ethernet port.

**Motion Master has no authentication.** Anything that can reach it over the network can enable a
drive and command motion. Put the board on a network you trust.

## Why a Raspberry Pi 5

Motion Master runs on any 64-bit Linux machine. The image targets one board on purpose. These are
the reasons.

- **A real-time kernel runs on it.** Debian ships a `PREEMPT_RT` kernel for arm64. The image uses
  it, isolates one CPU core, and pins the real-time thread to that core. A Pi 5 with one SOMANET
  Circulo holds a 250 µs cycle and skips no cycles.
- **Ethernet and Wi-Fi are both on the board.** EtherCAT needs the wired port to itself, so the
  wired port cannot also be how you reach the server. The Pi 5 has Gigabit Ethernet and dual-band
  802.11ac Wi-Fi as standard. No adapter, no second network card.
- **Every board is the same board.** We tune one machine and ship the result. The kernel, the
  isolated core, the pinned thread, the service and the configuration are all set the way we
  measured them. A laptop is a different machine every time, so nobody can hand you those settings.
- **Setup is short.** Write the image to a microSD card. Put the card in the board. Power it on.
- **You can buy one anywhere.** Amazon, Farnell, RS, Digi-Key and the Raspberry Pi resellers all
  stock it.
- **It stays buyable.** Raspberry Pi Ltd states that the Pi 5 "will remain in production until at
  least January 2036" in the
  [product brief](https://pip.raspberrypi.com/categories/892-raspberry-pi-5). That is a decade in
  which the hardware under this image does not change.
- **It costs a fraction of a laptop.** The table below is the whole bill.

### What it costs

| Part | Price |
| --- | --- |
| Raspberry Pi 5, 4 GB | €144 |
| 27 W USB-C power supply | €11 |
| Case with fan | €8 |
| microSD card, 32 GB | €10 |
| Micro-HDMI to HDMI cable | €5 |
| **Total** | **about €180** |

These are German retail prices on 28 August 2026. Each one includes 19% VAT. Postage is on top,
and it is usually €5 to €10 for the whole order. **Treat the total as an order of magnitude, not
a quote.** Prices moved hard through 2026. The 4 GB board listed at $60 when it launched and rose
three times, because the price of LPDDR4 memory rose. Raspberry Pi explains that in
[More memory-driven price rises](https://www.raspberrypi.com/news/more-memory-driven-price-rises/).
The same shortage raised the price of flash memory, so cards and SSDs both cost more than they did
a year ago.

The monitor and the HDMI cable are for one step, where you read the board's address off the
screen. Skip both if your router lists its leases.

### Why a microSD card and not an SSD

- **The board boots from a card as it comes.** There is no storage on the board. An NVMe
  (Non-Volatile Memory Express, the interface a modern SSD uses) drive needs the M.2 HAT+ at €13,
  the drive itself, and a case with room for the extra board.
- **Any computer writes a card.** Card readers cost a few euros, and many laptops have one. To
  write an NVMe drive you need the board, or a USB enclosure for the drive.
- **The speed does not reach the loop.** The Pi 5 reads a card at about 90 MB/s and an NVMe drive at
  up to 500 MB/s. The board boots in about 21 seconds from a card and about 15 seconds from a
  drive. Motion Master runs from memory after that, and the real-time loop reads and writes no
  file, so neither number touches the cycle.
- **A worn-out card is a card.** Write endurance is the one real argument for a drive. The board
  writes the system log and little else. If a card does fail, write another one. The image is the
  whole system, and your own settings are one file on the card.

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

Every image carries the same key, and the private half is published beside it:

```bash
curl -o ~/.ssh/motion-master-rpi \
  https://dezliul92qqoq.cloudfront.net/motion-master-rpi
chmod 600 ~/.ssh/motion-master-rpi
ssh -i ~/.ssh/motion-master-rpi root@192.168.0.81
```

`chmod 600` is not optional. `ssh` refuses a private key that other accounts on your computer can
read.

**That key is public, and so is the password.** They are not what keeps anyone out of the board.
The API has no authentication and answers on every interface, so somebody who can reach the board
can already drive the motors without logging in.

What each one is for is different. The password is for the console, with a monitor and a keyboard —
that is the way in when the board has no network, which is the failure you most need a way in for.
The key is for SSH, so nobody has to type a password over the network.

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
