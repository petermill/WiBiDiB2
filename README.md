# WiBiDiB2: Wi-Fi WiThrottle ⇄ BiDiB gateway (Raspberry Pi Pico 2 W)

Firmware for the **Raspberry Pi Pico 2 W** that behaves as a **BiDiB guest node** and serves **Engine Driver**
throttles (**WiThrottle** protocol) over Wi-Fi.

```
Engine Driver (phone) ⇄ Wi-Fi / TCP (WiThrottle) ⇄ Pico 2 W (node 1, guest)
        ⇄ BiDiBus ⇄ GBMBoostMaster (master, node 0) ⇄ serial link ⇄ GBM_CS ⇄ BiDiB Wizard / Rocrail / iTrain
```

It is part of a set of three repositories:

| Repository | Role |
|---|---|
| **WiBiDiB2** (this one) | Wi-Fi WiThrottle gateway on a Pico 2 W |
| `GBMBoostMaster_CS` | BiDiBus master / booster on AVR (Atmel Studio) |
| `GBM_CS` | PC-side Command Station (GTK / Glade, Debian): relay between the master and the host, loco database |

## What the firmware does

- **Locos**: speed, direction, functions F0 to F28, emergency stop; the loco is stopped when the throttle leaves.
- **Roster**: received from the Command Station in BiDiB `MSG_STRING` messages (namespace `0xFA`, in chunks);
  a throttle can only log on once the roster has arrived (*logon gate*). Limit: 32 locos in the Pico.
- **Turnouts**: announced to Engine Driver (`PTT` / `PTL`), commanded with `PTA`. Built-in table of 4 turnouts
  `LT21` to `LT24` (real addresses 20 to 23: the name `LTnn` corresponds to address `nn - 1`).
  Output 1 = thrown, 0 = closed. In Engine Driver: red = closed, green = thrown.
- **State feedback**: the master sends `MSG_LOCAL_ACCESSORY` (0x73) on the bus for every accessory command
  (Engine Driver, Wizard **and** XpressNet throttles); the Pico turns it into a `PTA` to **all** connected throttles.
- Throttle **monitoring** (heartbeat / watchdog) and loco **slot reuse**.

Orders to the track are wrapped in a guest request (`D2 0C 65 ...`, target 0x0C = DCCGEN) which the Command
Station converts into `MSG_CS_ACCESSORY` / `MSG_CS_DRIVE`.

## Operating modes

The firmware supports two network layouts. They are **mutually exclusive**: pick one.

| | `BIDIB_CS` | `IF2` |
|---|---|---|
| Network master | **GBMBoostMaster** (AVR), a separate device | the **OpenDCC BiDiB-IF2** interface (see below) |
| GBM_CS | **host extension**: it sits between the master and the train-control host (BiDiB-Wizard, Rocrail, iTrain, ...), answers the guest's requests and sends the roster | an ordinary **node** on the network, **not** the master |
| Role of the Pico | guest node on the BiDiBus (node 1) | node on the BiDiBus through an RS-485 transceiver <!-- TO VERIFY by the author --> |

**OpenDCC BiDiB-IF2** (designed by Wolfgang Kufer, sold among others by FichtelBahn) is a compact USB-to-BiDiBus
interface with a built-in DCC command station, so **no separate GBM master module is needed** in that layout.

The two layouts are implemented by **two code bases**: this repository is the `BIDIB_CS` one, the
[`akuhtz/WiBiDiB2`](https://github.com/akuhtz/WiBiDiB2) fork is the `IF2` one (see "Related work" below).
<!-- TO COMPLETE: if both modes can be selected from one code base, how (CMake option, #define, separate main). -->

The rest of this document describes the **`BIDIB_CS`** mode, which is the one that has been tested.

## System prerequisites (`BIDIB_CS` mode)

The Pico **does not work alone**:

1. **GBMBoostMaster** on the BiDiBus (the Pico is node 1).
2. **GBM_CS** in the chain: it answers the Pico's `SUBSCRIBE` requests and sends it the roster.
3. A **BiDiB host** (BiDiB-Wizard; Rocrail and iTrain still to be tested) that sends `SYS_ENABLE` to the node.
   **Without a host, no order is forwarded**: the Pico log shows `cs_accessory DROP: SYS_ENABLE pas recu`.
4. The `roster.xml` produced with the **Roster** program (see its own README).

## Hardware

Raspberry Pi Pico 2 W, with an **RS-485 transceiver** (for example a MAX485) between the Pico and the BiDiBus:

| Signal | GPIO | Pico pin | Note |
|---|---|---|---|
| `BIDIB_PIN_TX` | GP18 | 24 | to the transceiver's driver input |
| `BIDIB_PIN_RX` | GP19 | 25 | from the transceiver's receiver output |
| `BIDIB_PIN_DE` | GP6 | 9 | driver enable (and receiver enable) |
| `BIDIB_PIN_TEST` | GP16 | 21 | test point |

The 9-bit BiDiBus serial link is emulated with the Pico PIO, at `BIDIB_BAUD` = 500000 baud.

## Build

Requirements: Pico SDK (CMake), the `arm-none-eabi-gcc` toolchain, Pico 2 W support.
Developed with **VS Code** and the Raspberry Pi Pico extension.

```
mkdir build && cd build
cmake ..
make
```

Copy the resulting `.uf2` file to the Pico in BOOTSEL mode.

## Configuration

- **Wi-Fi**: the Pico works as an **access point only** (no station mode): the phones join the Pico's own network.
  The SSID and password are read from a **local file that is not committed**:
  copy `wifi_config.example.h` to `wifi_config.h` (same folder as `config.h`) and edit the copy before building.
  `wifi_config.h` is listed in `.gitignore`; the build stops with a clear error if it is missing.
  The Pico announces itself with **mDNS** (`WiBiDiB._withrottle._tcp.local.`), so Engine Driver finds the
  WiThrottle server automatically once the phone is on the Pico's network. For a manual connection, the Pico's
  address on its own network is `192.168.4.1`.
- **WiThrottle port**: TCP **5550** (`WITHROTTLE_PORT`).
- **Turnouts**: array `wt_turnouts[]` in `withrottle_if.c` (name `LTnn`, system name, address, initial state).

## Serial log

The Pico writes its log on its USB link (`/dev/ttyACM0` on Linux), for example:

```
picocom -b 115200 /dev/ttyACM0
```

Useful lines: `[bidib_parser] LOCAL_ACCESSORY ...`, `feedback LT23 -> etat ...`, `cs_accessory DROP ...`.

## Source layout

| File | Role |
|---|---|
| `withrottle_if.c/.h` | WiThrottle server: throttles, locos, turnouts, state feedback |
| `bidib_client_if.c` | sending BiDiB messages (drive, accessory, subscription) |
| `bidib_client_parser.c` | parsing messages received from the bus (roster, `MSG_LOCAL_ACCESSORY`, ...) |
| `tcp_server.c` | TCP server (lwIP) |

## Known limitations

- When an accessory order is dropped (no `SYS_ENABLE`), the Pico still sends an optimistic `PTA` to
  Engine Driver: the displayed state may then be wrong. Planned fix: confirm only if the order was really sent.
- Turnouts are a fixed table in the code (4 turnouts); managing them from the Roster program is planned.
- The display of function states in Engine Driver after a reconnection still needs improvement.
- Hosts other than BiDiB-Wizard (Rocrail, iTrain): **not tested**.

## Planned improvements

- [x] **Wi-Fi credentials out of the source**: the access-point SSID and password are in a local, git-ignored header
  (`wifi_config.h`) with a committed template (`wifi_config.example.h`).
- [ ] Optional **runtime Wi-Fi setup**: defaults in `config.h`, changeable without rebuilding (for example a small
  HTTP page served by the Pico in access-point mode, stored in flash).
- [ ] Optional **per-device password** derived from the Pico's unique ID, so that units do not share one password.
- [ ] **Turnout feedback only when the order was sent**: make `bidib_send_cs_accessory()` report whether the order
  left, and send the `PTA` confirmation to the throttles only in that case.
- [ ] **Turnouts from the Roster program** instead of the fixed 4-entry table.
- [ ] Function-state display in Engine Driver after a reconnection.
- [ ] Test with other hosts (Rocrail, iTrain) in place of BiDiB-Wizard.

## Related work: the `akuhtz/WiBiDiB2` fork

A fork of this project exists at <https://github.com/akuhtz/WiBiDiB2>. It targets the same Pico 2 W and the same
WiThrottle / Engine Driver use, but it is the **`IF2`** variant: the network master is the OpenDCC BiDiB-IF2.
It is organised differently, so code is not interchangeable between the two:

| | This repository | `akuhtz/WiBiDiB2` |
|---|---|---|
| BiDiB link | guest node on the BiDiBus behind the GBMBoostMaster; 9-bit UART emulated with the PIO at 500 kbaud, RS-485 transceiver on GP18 / GP19 / GP6 (same wiring as the fork) | 9-bit PIO UART at 500 kbaud, RS-485 transceiver on the Pico (GP18 / GP19 / GP6) |
| Roster | received from the Command Station (GBM_CS) | `roster.c`, plus a small HTTP server (`http_server.c`) |
| Märklin M4 locos | supported on the network, through GBMBoostMaster and GBM_CS | not part of that layout |
| Wi-Fi | **access point only** | station mode with an access-point fallback (192.168.4.1) and mDNS discovery |
| Throttle layer | `withrottle_if.c` | `withrottle_if.c` and `smartphone_if.c` |
| Persistent data | <!-- TO COMPLETE --> | optional external SPI flash (`flash_store.c`) |

### History

Both code bases start from the same early work, in which the **9-bit UART equivalent** for BiDiB and the **Wi-Fi
link** were first made to work on the Pico. The fork stayed on that base and the **IF2** layout. This repository went
further with **GBM_CS**, for more possibilities, and above all to be able to carry the **Märklin M4 protocol** on the
network (M4 locos log on through the GBMBoostMaster, GBM_CS assigns their address and keeps the loco database).

<!-- TO COMPLETE by the author: how the fork's main differs from this repository's `BIDIB_CS` main. The fork's
     README and main.c do not mention `IF2` or `bidib_cs`, so nothing more is claimed here. -->


## License

This project uses the BiDiB protocol headers from [bidib.org](http://www.bidib.org) and the DHCP server from the
Raspberry Pi Pico Examples. See the individual file headers for license terms.
<!-- TO COMPLETE: license of the project's own code (the fork lists GPL-3.0; the Roster program headers say GPL v2). -->

Pierre Moulin.
