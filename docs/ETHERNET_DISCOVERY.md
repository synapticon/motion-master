# Ethernet Discovery and Address Assignment

> Background for issue #58 and for the DHCP server that follows it. This page collects what is
> known about how Motion Master can find a SOMANET drive on an Ethernet network, and how it can
> give a drive an IP address. Both are Ethernet tasks. They do not depend on SPoE: SPoE is one
> protocol that uses the IP address afterwards, and only once the drive has one. The page marks
> each fact with its source: a measurement, the firmware source, a standard, or the automation
> framework. Nothing on this page is built into the server yet, and no approach is decided.

## Terms

- **SPoE** (SOMANET Protocol over Ethernet). The protocol over TCP that Motion Master uses to
  talk to a SOMANET drive that runs an Ethernet fieldbus other than EtherCAT. The SPoE fieldbus
  driver connects to TCP port 8080 of each drive (`libs/comm/spoe_fieldbus_driver.h`).
- **netX**. The Hilscher communication chip of an Integro. It runs the fieldbus stack and the SPoE
  server. An ACTILINK-S is an Integro with a motor.
- **EtherNet/IP**. An industrial Ethernet fieldbus. It uses TCP and UDP port 44818 for its
  management messages.
- **CIP** (Common Industrial Protocol). The object model that EtherNet/IP carries. A CIP object is
  named by a class number. Its values are attributes.
- **PROFINET**. An industrial Ethernet fieldbus. Its device management runs in raw Ethernet frames
  with EtherType 0x8892, without IP.
- **DCP** (Discovery and Configuration Protocol). The part of PROFINET that finds devices and sets
  their names and IP addresses.
- **LLDP** (Link Layer Discovery Protocol, IEEE 802.1AB). A device sends a short frame that
  describes itself to its direct neighbour on the cable, at a fixed interval.
- **DHCP** (Dynamic Host Configuration Protocol, RFC 2131). A device without an IP address asks
  the network for one, and a DHCP server answers with an address.
- **Raw socket**. A socket that sends and receives whole Ethernet frames. A normal socket only
  sends and receives the data of a TCP connection or a UDP datagram.
- **Subnet**. The range of IP addresses that a host reaches without a router, for example
  192.168.100.0 to 192.168.100.255 for 192.168.100.1 with the mask 255.255.255.0.

## Which devices this concerns

Only an Integro runs a fieldbus other than EtherCAT. An ACTILINK-S is an Integro, so the same holds
for it. Node and Circulo run EtherCAT only.

SPoE runs only on an Integro. A Jasper-based drive runs EtherCAT only. The Jasper firmware has SPoE
in its simulator, which runs the firmware on a PC, and nowhere else. The firmware team does not plan
SPoE for the firmware of a real Jasper-based drive.

An Integro runs one fieldbus protocol at a time. The `.variant` file on the device selects it. The
protocols exclude each other. Motion Master reads the file and decodes it in
`libs/node/integro_variant.cc`.

| Code | Fieldbus protocol | Excludes | Order code | IP stack | SPoE |
| ---: | --- | --- | --- | --- | --- |
| 0 | EtherNet/IP - CiA 402 | 1, 2, 3, 39 | EI | Yes | Yes, measured |
| 1 | EtherCAT | 0, 2, 3, 39 | EC | No | No |
| 2 | PROFINET - CiA 402 | 0, 1, 3, 39 | EP | Yes | Not measured |
| 3 | CAN | 0, 1, 2, 39 | CB | No | No |
| 39 | EtherNet/IP - LXM | 0, 1, 2, 3 | EI+CSE | Yes | Not measured |

So discovery concerns codes 0, 2 and 39. A drive with EtherCAT is found with `POST /api/scan`. A
drive with CAN has no Ethernet.

Source: the table of fieldbus protocols of the Integro, given by the firmware team, and
`libs/node/integro_variant.cc`. The SPoE column: issue #36 measured SPoE on drives with EtherNet/IP
firmware.

## The problem

A drive with an Ethernet fieldbus has an IP address, or it has none. Before any protocol can talk
to the drive over IP, somebody must know that address, and the address must be on a subnet that
the host reaches. Today the user must find the address with another tool and type it.

Two features address this. Both work on the Ethernet layer, before any IP protocol runs:

1. **Discovery.** Motion Master lists the devices on one adapter, with their MAC addresses, their
   IP addresses and what they are.
2. **Address assignment.** Motion Master gives a device an IP address, for example with a DHCP
   server.

SPoE is the first user of the result: the SPoE fieldbus driver takes a list of IP addresses,
`fieldbus.ipAddresses`, and discovery can fill it. Other users can follow, for example a tool that
connects to the EtherNet/IP or PROFINET interface of a drive.

### Discovery must not connect to the SPoE port

Discovery itself uses no SPoE. But a drive serves one SPoE client at a time. While a client is
connected, the drive does not answer a second client (#36, measured with
`hil/spoe/second_client.py`). A sweep of port 8080 over a subnet would interfere with a client
that works with a drive. So discovery uses only methods that open no SPoE connection.

## The discovery methods

Three methods find a drive. None of them opens a connection to the drive.

| | LLDP | DCP Identify All | ListIdentity |
| --- | --- | --- | --- |
| Layer | Raw Ethernet, EtherType 0x88CC | Raw Ethernet, EtherType 0x8892 | UDP over IP, port 44818 |
| Who starts | The drive, by itself | Motion Master asks | Motion Master asks |
| Request | None | Multicast to `01:0e:cf:00:00:00` | Broadcast to `255.255.255.255` |
| Answer | Multicast to `01:80:c2:00:00:0e`, about every 30 s | Raw Ethernet frame to the MAC address of the host | UDP to the IP address of the host, after a random delay |
| Fieldbus | EtherNet/IP and PROFINET | PROFINET | EtherNet/IP |
| The drive needs an IP address | No | No | Yes |
| The host needs an IP address | No | No | Yes |
| Time, measured | 18 s | Not measured | 64 to 410 ms |

### LLDP

The drive sends an LLDP frame by itself, at a fixed interval. Motion Master only listens.

```text
drive ──(about every 30 s)──▶ LLDP frame ──▶ host listens
```

The frame is a list of fields. Each field has a type, a length and a value. The fields that matter
here:

- **Chassis ID.** The MAC address of the device.
- **Port ID.** The port of the device that sent the frame, for example `Port0`.
- **System name.**
- **Management address.** The IP address of the device. A device without an address reports
  `0.0.0.0`.
- **Organisation-specific fields.** Each one starts with the identifier of the organisation that
  defines it. The drive in the measurement sent one field of ODVA, the organisation behind
  EtherNet/IP (identifier `00:21:6c`, subtype 9).

The destination address `01:80:c2:00:00:0e` is one that a switch does not forward (IEEE 802.1AB).
So a frame reaches only the direct neighbour of the device. It is not measured whether the first
drive of a chain passes on the LLDP frames of the drive behind it.

What the firmware says:

- The Hilscher EtherNet/IP stack has an LLDP Management object, CIP class 0x109
  (`arm_application/Components/cifXApplicationStackDefinitions/Includes/EIP_API/eip_lldp_management.h`
  in `somanet_software`). It has an enable bit for each port and a send interval. The default
  interval is 30 s, the value of IEEE 802.1AB. So a client can turn LLDP off over CIP, or change the
  interval.
- The LLDP code is inside the compiled Hilscher stack. The source does not show whether LLDP is on
  by default. The measurement shows that it was on for one drive.
- The PROFINET standard is believed to require LLDP. This is not checked in the firmware or on a
  drive.

So LLDP can stop working when somebody changes a setting on the drive.

### DCP Identify All

Motion Master sends one request. Every PROFINET device answers.

```text
host ──DCP Identify All (multicast)──▶ every PROFINET device
host ◀──DCP answer (to the host MAC)── each device
```

The request is a raw Ethernet frame to `01:0e:cf:00:00:00`, the multicast address that every
PROFINET device listens to. It carries a transaction identifier, a response delay factor, and the
"all" selector, which every device matches. A response delay factor of 1 asks every device to
answer at once. A larger factor spreads the answers over a longer time, like the answer delay of
ListIdentity. Its unit is not checked against the PROFINET standard, IEC 61158-6-10, so the design
below does not set a value yet.

The answer is a raw Ethernet frame to the MAC address of the host. It carries the same transaction
identifier and a list of blocks. The blocks that matter here:

- IP address, mask and gateway.
- Name of station, the name that the PROFINET controller uses for the device.
- Type of station.
- Vendor ID and device ID.

DCP does not use IP. So it finds a drive without an IP address, and a drive on any subnet. The host
needs no IP address either.

It is not measured on a SOMANET drive. The automation framework uses DCP Identify All to find every
drive of a PROFINET chain before it assigns addresses (`mmaf/network/dcp_assign_ip.py`).

### ListIdentity

Motion Master sends one UDP broadcast. Every EtherNet/IP device answers.

```text
host ──UDP broadcast to port 44818──▶ every EtherNet/IP device
host ◀──UDP answer to the host IP──── each device, after a random delay
```

The request is an EtherNet/IP message with the command 0x0063 and no data. The answer carries one
identity item:

- The IP address of the device.
- Vendor ID, device type, product code and revision.
- Serial number.
- Product name.
- Status and state.

**The request sets the longest delay of the answer.** The first 2 bytes of the sender context of
the request are the longest random delay in ms. A broadcast reaches every device at once, and the
random delay spreads their answers so that they do not arrive at the same moment. In the first
measurement, the sender context held text that reads as 28787 ms, and the drive answered after
13.2 s. With 500 ms, five answers arrived after 64, 137, 146, 287 and 410 ms. So the request must
set this field on purpose.

**Why the answers are spread.** A broadcast reaches every drive at the same moment. If every drive
answered at once, all answers would arrive at the host together. The switch port to the host and
the receive buffer of the host hold a limited number of frames, and they drop the frames that do
not fit. The answer is a UDP datagram, and UDP does not send a lost datagram again. So a dropped
answer is a drive that the discovery does not find. The random delay spreads the answers over
time, so the buffers empty between them.

The answer delay must be shorter than the time that the host listens, or a late answer is missed.
A longer delay spreads the answers of many drives over a longer time, so fewer arrive at the same
moment. The cost is a longer wait for the user. The design below uses the listen time minus
500 ms. The 500 ms margin covers the trip back and the time that the drive needs to handle the
request. Whether a shorter delay is enough for a long chain is not measured.

**Both ends need an IP address.** The answer is a UDP datagram to the IP address of the host. A
drive without an IP address cannot send it. A host without an IP address on the adapter cannot
give one.

**A drive on another subnet may not deliver its answer.** The drive sends its answer to the host
address. If that address is not on the subnet of the drive, the drive may send the answer to its
gateway, or not at all. This is not measured.

### What each method finds

| Drive | LLDP | DCP | ListIdentity |
| --- | --- | --- | --- |
| EtherNet/IP, address on the subnet of the host | Yes | No | Yes |
| EtherNet/IP, address on another subnet | Yes | No | Not measured |
| EtherNet/IP, no address | Yes, with `0.0.0.0` | No | No |
| PROFINET, any address or none | Expected, not measured | Expected, not measured | No |
| Second drive of a chain | Not measured | Expected, not measured | Expected, not measured |

## One drive, several MAC addresses

A drive sends frames from more than one MAC address. In the measurement:

- The ListIdentity answer came from `40:49:8a:01:29:3c`.
- The LLDP frame came from `02:00:00:1e:99:01`.
- The chassis ID inside the LLDP frame was `40:49:8a:01:29:3c`.

The firmware has a separate MAC address for each port, for LLDP
(`AppEIS_ApplicationFunctions.c` and `AppPNS_ApplicationFunctions.c` in `somanet_software`). So
answers of different methods are matched by the chassis ID of LLDP or by the IP address. A match by
the source MAC address of a frame lists one drive twice.

## Measurements

All measurements used `hil/ethernet_discovery` on the adapter `spoe0` (192.168.100.1/24) on
2026-11-23. The drive was an ACTILINK-S with EtherNet/IP firmware at 192.168.100.17, alone on the
adapter. The firmware version was not recorded.

| Method | Result |
| --- | --- |
| ListIdentity | Answer from 192.168.100.17: name `Actilink`, vendor ID 1783, device type 19, product code 1025, revision 1.1, serial number 0x792 |
| LLDP | Frame after 18 s: IP address 192.168.100.17, chassis ID `40:49:8a:01:29:3c`, port `Port0`, one ODVA field |
| DCP Identify All | No answer. A drive with EtherNet/IP firmware is not expected to answer |

To repeat the measurement:

```bash
./build/x64-linux-debug/hil/ethernet_discovery/ethernet_discovery --ifname spoe0 --duration 35
```

The program needs raw socket access. Grant it once after each build:

```bash
sudo setcap cap_net_raw+ep build/x64-linux-debug/hil/ethernet_discovery/ethernet_discovery
```

## Address assignment

A drive needs an IP address on the subnet of the host before an IP protocol, such as SPoE, can
reach it. Each method
allows a different way to set one.

| Method | Can it set an IP address? |
| --- | --- |
| LLDP | No. A drive only reports with it. |
| DCP Set | Yes, for PROFINET. It writes the IP address, mask and gateway, or the name of station, to one device addressed by its MAC address. The device needs no IP address. |
| ListIdentity | No. EtherNet/IP has no raw Ethernet request that sets an address. |
| CIP write to class 0xF5 | Yes, for EtherNet/IP. The drive must be reachable by IP. |
| DHCP | Yes, for a drive that asks for an address. |

None of these is measured on a SOMANET drive.

### DCP Set

A DCP Set request is a raw Ethernet frame to the MAC address of one device. It carries the new IP
address, mask and gateway. The automation framework first runs DCP Identify All, then sends DCP Set
to each MAC address it found (`mmaf/network/dcp_assign_ip.py` and
`mmaf/resources/scripts/profinet_assign_ip.py`).

### CIP write to the TCP/IP Interface object

EtherNet/IP devices have a TCP/IP Interface object, CIP class 0xF5. Two attributes matter:

- Attribute 3, the configuration control. Its low bits select where the address comes from: 0 for
  the stored address, 1 for BOOTP, 2 for DHCP.
- Attribute 5, the interface configuration: IP address, mask, gateway, two name servers and a
  domain name.

The automation framework writes attribute 3 with 0, then attribute 5 with the new address, then
sends a reset of the CIP Identity object, class 0x01. The drive restarts with the new address
(`mmaf/network/cip_assign_ip.py`). The write needs a connection to port 44818 of the drive, so the
drive must already have an address that the host can reach. The framework adds a second address on
the subnet of the drive to the host adapter for the time of the write (`mmaf/network/nic_alias.py`).

### DHCP

A device without an address and a DHCP server exchange four messages (RFC 2131):

1. The device broadcasts a DISCOVER, from UDP port 68 to port 67, with its MAC address.
2. The server answers with an OFFER, which carries an address.
3. The device broadcasts a REQUEST for that address.
4. The server answers with an ACK. The device takes the address.

The automation framework states that EtherNet/IP firmware ships with the configuration control set
to DHCP (`mmaf/network/dhcp_responder.py`). So a new drive asks for an address and waits. This is
not checked in the firmware or on a drive. The framework also states that PROFINET drives have no
DHCP client (`mmaf/network/dcp_assign_ip.py`). This is not checked either.

#### What the automation framework does

The framework has two DHCP mechanisms:

1. **A system DHCP server.** It writes `/etc/dhcp/dhcpd.conf` for `isc-dhcp-server` with a pool,
   by default 192.168.2.100 to 192.168.2.120, and starts the server on the bench adapter. It then
   pings every address of the pool to find the drives (`mmaf/network/dhcp.py`, `mmaf/fleet.py`).
2. **A one-shot responder.** It answers one DHCP request from one MAC address, once, and exits. It
   exists because the framework must also run inside OBLAC Drives on customer machines, where it
   must not change the network configuration of the host (`mmaf/network/dhcp_responder.py`).

#### Who the server answers

A DHCP server answers every DISCOVER it receives. On an adapter that only connects drives, that is
correct. On an adapter that also connects an office network, the server can give an address to a
PC or a printer, and the address can conflict with the real DHCP server of that network. So the
choice of whom the server answers is the main decision of the feature.

| Option | The server answers | Risk |
| --- | --- | --- |
| A. Pool | Every DHCP request on the adapter | It can give an address to a device that is not a drive |
| B. Reservations | Only the MAC addresses in a list | The user must know each MAC address first. Discovery gives it |
| C. One-shot | One MAC address, once, when the user asks | Each new drive needs one action of the user |

Whatever the option, three rules hold:

- The server is off by default. The user turns it on for one named adapter.
- The server never answers on another adapter.
- The server needs a privilege: `CAP_NET_RAW` for a raw socket, or `CAP_NET_BIND_SERVICE` for UDP
  port 67.

Discovery and address assignment fit together. LLDP reports a drive without an address with its
MAC address and `0.0.0.0`. With option B or C, the user gives that MAC address an address in one
step.

## Platforms

LLDP and DCP use raw Ethernet frames, so a normal socket cannot send or receive them.

| Platform | Raw frames | Status |
| --- | --- | --- |
| Linux | `AF_PACKET` socket, with `CAP_NET_RAW` | Decided. `hil/ethernet_discovery` uses it |
| Windows | Npcap | Decided. The SOEM driver already needs Npcap |
| macOS | BPF through libpcap, with root or access to `/dev/bpf*` | Not decided |

ListIdentity works over a normal UDP socket on every platform, without a privilege. A normal
socket has two limits: the adapter needs an IPv4 address, and the socket does not see an answer
that the operating system drops. A broadcast to `255.255.255.255` can leave on another adapter than
the one the user selected. A broadcast to the subnet of the adapter, such as 192.168.100.255, does
not. Neither is measured on Windows or macOS.

Some network adapters handle LLDP in their own firmware and do not pass the frames to the
operating system. Windows also has its own LLDP driver. Whether Npcap sees LLDP frames on Windows
is not measured.

## Proposed design for discovery

This is the design in issue #58. It is not built.

1. In the Console, the user selects an adapter, sets the discovery time, and clicks **Discover**.
2. The Console calls `GET /api/ethernet/discovery?adapter=<name>&durationMs=<ms>`.
3. Motion Master sends DCP Identify All and ListIdentity at the same time and collects the answers
   for `durationMs`. The ListIdentity request sets the answer delay to `durationMs` minus 500 ms.
   With a `durationMs` long enough for LLDP, it also listens for LLDP.
4. Motion Master merges the answers into one row per drive.
5. The user selects drives. For SPoE, the Console puts their addresses into
   `fieldbus.ipAddresses` and starts SPoE with `POST /api/init`.

`durationMs` is the one value that sets the timing. It sets how long Motion Master listens, and the
answer delay of ListIdentity follows from it. A user who knows that a chain has many drives sets a
longer time, and the drives spread their answers over a longer time. The answer delay is always
shorter than the listen time, so no request can miss answers through its own settings.
`durationMs` must be more than 500 ms. The server refuses a smaller value with 400.

The Console shows the discovery time in a field next to **Discover**, with 2000 ms as the
default. The help text of the field says to raise it for a long chain. So **Discover** takes 2 s by
default, and its ListIdentity answers spread over up to 1500 ms. **Deep search** sends
`durationMs=35000`, which includes LLDP.

The response states the values that the request used, so the user sees what it did.

```json
{
  "durationMs": 2000,
  "answerDelayMs": 1500,
  "devices": [
    {
      "ipAddress": "192.168.100.17",
      "macAddress": "40:49:8a:01:29:3c",
      "name": "Actilink",
      "firmware": "ethernet-ip",
      "vendorId": 1783,
      "productCode": 1025,
      "serialNumber": 1938,
      "sameSubnet": true,
      "foundBy": ["list-identity"]
    }
  ]
}
```

`"firmware": "ethernet-ip"` does not tell code 0 from code 39. Whether the ListIdentity answer
tells them apart is not known.

## Open questions

Measurements still to make:

1. DCP Identify All on a drive with PROFINET firmware.
2. Two or more drives in a chain, for each method.
3. A drive on another subnet than the host, for ListIdentity.
4. A drive without an IP address, for LLDP and DHCP.
5. A drive with EtherNet/IP - LXM firmware, code 39: does it answer ListIdentity, and how does its
   answer differ from code 0?
6. Whether SPoE runs on PROFINET firmware and on EtherNet/IP - LXM firmware.
7. LLDP and ListIdentity on Windows with Npcap.

Decisions still to make:

1. Whom the DHCP server answers: option A, B or C.
2. Whether Motion Master also assigns addresses with DCP Set and the CIP write, or only with DHCP.
3. Whether the LLDP listener runs only during a request or in the background.
4. Discovery on macOS.
5. The response delay factor of DCP Identify All, after its unit is checked.
6. The largest `durationMs` that the server accepts.

## References

- Issue #58, Ethernet device discovery. It holds the design and the acceptance criteria.
- Issue #36, the SPoE fieldbus driver.
- `hil/ethernet_discovery/main.cc`, the measurement program.
- `libs/node/integro_variant.cc`, the fieldbus protocols of an Integro.
- `somanet_software`: `eip_lldp_management.h`, `AppEIS_ApplicationFunctions.c`,
  `AppPNS_ApplicationFunctions.c`.
- `apremji/motion_master_automation_framework`: `mmaf/network/lldp.py`,
  `mmaf/network/dcp_assign_ip.py`, `mmaf/network/cip_assign_ip.py`, `mmaf/network/dhcp.py`,
  `mmaf/network/dhcp_responder.py`, `mmaf/network/nic_alias.py`.
- IEEE 802.1AB, LLDP.
- RFC 2131, DHCP.
