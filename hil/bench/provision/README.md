# Bench machine provisioning

This Ansible playbook takes a fresh Debian 13 install to a machine that runs the Bench Tests
workflow (`.github/workflows/bench-tests.yml`) against the drives of the bench. A second run
changes nothing.

The playbook runs from your own computer and reaches the bench machine over SSH.

## Install Debian 13

1. Install Debian 13 from the netinst image.
2. Keep the default desktop, GNOME. Select "SSH server" as well.
3. Leave the root password empty. Debian then gives the first user `sudo`.
4. Give that user passwordless `sudo` on the bench machine:

   ```bash
   echo '<user> ALL=(ALL) NOPASSWD:ALL' | sudo tee /etc/sudoers.d/<user>
   sudo chmod 0440 /etc/sudoers.d/<user>
   sudo visudo -cf /etc/sudoers.d/<user>
   ```

5. Copy your SSH key from your own computer:

   ```bash
   ssh-copy-id -i ~/.ssh/id_ed25519.pub <user>@<address>
   ```

## Run the playbook

Ansible must be installed on your own computer (`./tools/install-deps.sh`).

```bash
cd hil/bench/provision
cp inventory.example.yml inventory.yml   # then set the address and the user
ansible-playbook bench-machine.yml --check --diff   # dry run
ansible-playbook bench-machine.yml
ansible-playbook bench-machine.yml --tags base      # one role
```

## Roles

| Role | Does |
| --- | --- |
| `base` | Installs the C++ build toolchain, CMake 4, `gh` from GitHub's apt repository, and the git identity of the bench user. Adds the bench user to `systemd-journal`, so `journalctl` shows the whole journal. Turns off password login over SSH. |
| `power` | Stops the machine from ever suspending, turns off the GNOME screen blank, and selects the performance power profile. |
| `docker` | Installs Docker with buildx from Docker's apt repository, and adds the bench user to the `docker` group. |
| `bench_host` | Installs `ethtool`. Brings the EtherCAT interface up with no address and turns off its offloads. Stops NetworkManager from running DHCP on a wired interface. Allows `sudo` without a password, for the playbook and for `tc`. Warns when the ESP32 of the P 1535 does not answer. |
| `actions_runner` | Registers the GitHub Actions runner `ci-belgrade-linux` in the runner group `Belgrade` of the organisation, and runs it as a service under the bench user. |
| `desktop` | Starts GDM and GNOME on X11, so that RustDesk reaches the login screen and the session with nobody at the desk. Installs RustDesk and shows its ID. Selects the dark GNOME theme as the default. |
| `tools` | Installs VHS with `ttyd`, `ffmpeg` and Chromium, to record `hil/bench/demo.gif`, and Claude Code for the bench user. |
| `tailscale` | Installs Tailscale and starts it at boot, for SSH access to the bench machine from outside the office. |
| `scripts` | Copies the scripts in `roles/scripts/files/bin` to `~/bin` of the bench user. `runner-logs` prints the log of the Actions runner and passes its arguments to `journalctl`, so `runner-logs -f` follows it. |

The `actions_runner` role needs `gh` on the bench machine, signed in as the bench user with the `admin:org` scope:

```bash
gh auth login --scopes admin:org
```

`gh` keeps the token in the GNOME keyring, which opens when the bench user logs in to the desktop with the password. After a boot, log in once, at the desk or over RustDesk, before you run the playbook.

GDM starts on X11 after the next reboot. There is no automatic login: after a boot, log in once over RustDesk at the login screen. That also opens the GNOME keyring.

To set the permanent RustDesk password, put `desktop_rustdesk_password` in `inventory.yml`. The role sets it only when it installs RustDesk.

After the first run, log in to Tailscale on the bench machine. The command prints a link. Open it and log in with the Tailscale account that you use on your own computer:

```bash
sudo tailscale up --hostname ci-belgrade-linux
```

Then turn off key expiry for `ci-belgrade-linux` in the Tailscale admin console. Otherwise the machine leaves the network after 180 days and needs a new login at the desk. From outside the office, connect with `ssh <user>@ci-belgrade-linux`.

After the first run, sign in to Claude Code on the bench machine: run `claude` and follow the login.

The `base` role refuses to turn off password login when the bench user has no SSH key.
