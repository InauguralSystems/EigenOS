# T3200 first-visit inventory

Raw output from the M7 prep step, taken on the target machine
(Gateway M-7317U, Pentium Dual-Core T3200 @ 2.00 GHz) running Ubuntu 24.04.4
LTS, kernel 6.8.0-124-generic. Captured 2026-07-02.

## Flags for the dev side

- **CPU features match spec**: `ssse3` present, `sse4_1` / `sse4_2` absent,
  `rdrand` absent. The T3200 sim gate (`-cpu core2duo`) is the right
  approximation; the freestanding image's post-Merom-clean invariant holds.
- **e820 is NOT the clean QEMU blob.** Fragmented reserved holes throughout
  `0xbb6b1000–0xbbb1efff`, ACPI NVS at `0xbbb65000–0xbbb9efff` (232 KiB),
  ACPI data at `0xbbbe3000–0xbbbfcfff` (104 KiB), the chipset PCI-hole
  extends the reservation up to 4 GiB, and RAM continues as a **1 GiB region
  above 4 GiB** (`0x100000000–0x13fffffff`). The PMM's bitmap must
  respect every reserved range from the Multiboot2 map or it will corrupt
  ACPI / chipset windows. Total usable ≈ 3.94 GiB (~3.8 GiB after low-mem
  reservations), matching the spec sheet.
- **No live serial console.** `/proc/tty/driver/serial` lists the four
  legacy ISA ports (0x3F8/0x2F8/0x3E8/0x2E8) but every one reports
  `uart:unknown` — the classical addresses are enumerated but no chip
  answered probing. Practical answer for M7: **VGA text is the only report
  path.** A `setserial` + loopback experiment could still find one, but
  don't count on it. Persistent-RAM crash log (playbook item 2) becomes
  the main post-mortem seam.
- **Firmware is UEFI, not pure BIOS.** `/boot/efi` is mounted; `sda2` is a
  FAT32 ESP. Ubuntu boots via `grub-efi-amd64`. The playbook's
  `menuentry` recipe still applies (GRUB2 speaks `multiboot2` in either
  mode), but `update-grub` writes to `/boot/grub/grub.cfg` for BIOS and
  the loader entry is picked up from the ESP for UEFI — verify which one
  actually gets consulted at boot before committing to a `menuentry`
  edit. Machine firmware date is 2008-07-23 (SMBIOS 2.5), so this is a
  UEFI 1.x / early-2.0 vintage — expect quirks.
  - **SUPERSEDED 2026-07-02** (sentinel run, see `T3200-BOOT-LOG.md`):
    firmware supports UEFI (the ESP exists) but the machine actually
    **boots via BIOS/CSM, not UEFI** — `/sys/firmware/efi` is absent and
    `mokutil` reports EFI variables unsupported. The "firmware is UEFI"
    header above was an unverified inference from the mounted ESP.
- **/boot is NOT a separate partition** (`findmnt /boot` empty). GRUB
  path is `/boot/EigenOS.elf`, matching the primary case in the playbook.
- **NIC for M13**: Marvell 88E8040 PCI-E Fast Ethernet + Qualcomm Atheros
  AR928X wireless (working, on Wi-Fi at capture time).
- **No FireWire / no ExpressCard bridge visible** in unprivileged lspci —
  the M13 fallback path (ExpressCard 16550) needs a real card first.
- **Disk health caution**: `dmesg` shows `ata1.00: exception Emask 0x0
  SAct 0x1e8200 SErr 0x40000 action 0x0` from Jun 23. Not blocking M7
  (dual-boot via GRUB doesn't require the disk to be clean), but consistent
  with the earlier "T3200 on borrowed time after the 2026-06-10 fall" note.

## `sudo dmidecode -t system -t chassis`

```
# dmidecode 3.5
Getting SMBIOS data from sysfs.
SMBIOS 2.5 present.

Handle 0x0001, DMI type 1, 27 bytes
System Information
	Manufacturer: Gateway
	Product Name: M-7317U
	Version: Rev 1
	Serial Number: [redacted]
	UUID: [redacted]
	Wake-up Type: Power Switch
	SKU Number: Not Specified
	Family: Not Specified

Handle 0x0003, DMI type 3, 17 bytes
Chassis Information
	Manufacturer: Gateway
	Type: Portable
	Lock: Not Present
	Version: Rev.1
	Serial Number: [redacted]
	Asset Tag: No Asset Tag
	Boot-up State: Safe
	Power Supply State: Safe
	Thermal State: Safe
	Security Status: None
	OEM Information: 0x00001234
```

Firmware date (from `hostnamectl`): **2008-07-23**.

Firmware keys (confirmed by Jon, 2026-07-02): **Setup = F2, boot menu = F10**.

## `grep -m1 flags /proc/cpuinfo`

```
flags		: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse sse2 ht tm pbe syscall nx lm constant_tsc arch_perfmon pebs bts rep_good nopl cpuid aperfmperf pni dtes64 monitor ds_cpl est tm2 ssse3 cx16 xtpr pdcm lahf_lm pti dtherm
```

Present: `sse`, `sse2`, `ssse3`, `lm`, `nx`, `pae`, `cx16`. Absent: `sse4_1`,
`sse4_2`, `popcnt`, `rdrand`, `avx`, `aes`. Matches the QEMU
`-cpu core2duo` gate exactly.

## `sudo journalctl -b -k | grep -iE "e820|BIOS-provided"`

```
BIOS-provided physical RAM map:
BIOS-e820: [mem 0x0000000000000000-0x000000000009d3ff] usable
BIOS-e820: [mem 0x000000000009d400-0x000000000009ffff] reserved
BIOS-e820: [mem 0x00000000000d2000-0x00000000000d3fff] reserved
BIOS-e820: [mem 0x00000000000dc000-0x00000000000dffff] reserved
BIOS-e820: [mem 0x00000000000e4000-0x00000000000fffff] reserved
BIOS-e820: [mem 0x0000000000100000-0x00000000bb6b0fff] usable
BIOS-e820: [mem 0x00000000bb6b1000-0x00000000bb6b6fff] reserved
BIOS-e820: [mem 0x00000000bb6b7000-0x00000000bb7c4fff] usable
BIOS-e820: [mem 0x00000000bb7c5000-0x00000000bb80efff] reserved
BIOS-e820: [mem 0x00000000bb80f000-0x00000000bb908fff] usable
BIOS-e820: [mem 0x00000000bb909000-0x00000000bbb0efff] reserved
BIOS-e820: [mem 0x00000000bbb0f000-0x00000000bbb17fff] usable
BIOS-e820: [mem 0x00000000bbb18000-0x00000000bbb1efff] reserved
BIOS-e820: [mem 0x00000000bbb1f000-0x00000000bbb64fff] usable
BIOS-e820: [mem 0x00000000bbb65000-0x00000000bbb9efff] ACPI NVS
BIOS-e820: [mem 0x00000000bbb9f000-0x00000000bbbe2fff] usable
BIOS-e820: [mem 0x00000000bbbe3000-0x00000000bbbfcfff] ACPI data
BIOS-e820: [mem 0x00000000bbbfd000-0x00000000bbbfffff] usable
BIOS-e820: [mem 0x0000000100000000-0x000000013fffffff] usable
e820: update [mem 0x00000000-0x00000fff] usable ==> reserved
e820: remove [mem 0x000a0000-0x000fffff] usable
e820: update [mem 0xbbc00000-0xffffffff] usable ==> reserved
PCI: Using E820 reservations for host bridge windows
e820: reserve RAM buffer [mem 0x0009d400-0x0009ffff]
e820: reserve RAM buffer [mem 0xbb6b1000-0xbbffffff]
e820: reserve RAM buffer [mem 0xbb7c5000-0xbbffffff]
e820: reserve RAM buffer [mem 0xbb909000-0xbbffffff]
e820: reserve RAM buffer [mem 0xbbb18000-0xbbffffff]
e820: reserve RAM buffer [mem 0xbbb65000-0xbbffffff]
e820: reserve RAM buffer [mem 0xbbbe3000-0xbbffffff]
e820: reserve RAM buffer [mem 0xbbc00000-0xbbffffff]
```

## `sudo cat /proc/tty/driver/serial` (first 4 entries)

```
serinfo:1.0 driver revision:
0: uart:unknown port:000003F8 irq:4
1: uart:unknown port:000002F8 irq:3
2: uart:unknown port:000003E8 irq:4
3: uart:unknown port:000002E8 irq:3
```

Entries 4–31 all report `port:00000000 irq:0` (unused slots). All four
legacy ISA ports report `uart:unknown` — the addresses are listed but no
chip answered probing.

## `sudo lspci -nn` (edited to relevant devices)

```
00:00.0 Host bridge [0600]: Intel Corporation Mobile 4 Series Chipset Memory Controller Hub [8086:2a40] (rev 07)
00:02.0 VGA compatible controller [0300]: Intel Corporation Mobile 4 Series Chipset Integrated Graphics Controller [8086:2a42] (rev 07)
00:1b.0 Audio device [0403]: Intel Corporation 82801I (ICH9 Family) HD Audio Controller [8086:293e] (rev 03)
00:1c.0/2/5 PCI bridge [0604]: Intel Corporation 82801I (ICH9 Family) PCI Express Ports 1/3/6
00:1a.* / 00:1d.* USB controllers [0c03]: Intel 82801I (ICH9) UHCI + USB2 EHCI
00:1e.0 PCI bridge [0604]: Intel Corporation 82801 Mobile PCI Bridge [8086:2448] (rev 93)
00:1f.0 ISA bridge [0601]: Intel Corporation ICH9M LPC Interface Controller [8086:2919] (rev 03)
00:1f.2 SATA controller [0106]: Intel Corporation 82801IBM/IEM (ICH9M/ICH9M-E) 4 port SATA Controller [AHCI mode] [8086:2929] (rev 03)
00:1f.3 SMBus [0c05]: Intel Corporation 82801I (ICH9 Family) SMBus Controller [8086:2930] (rev 03)
02:00.0 Network controller [0280]: Qualcomm Atheros AR928X Wireless Network Adapter (PCI-Express) [168c:002a] (rev 01)
06:00.0 Ethernet controller [0200]: Marvell Technology Group Ltd. 88E8040 PCI-E Fast Ethernet Controller [11ab:4354] (rev 12)
07:09.0 Mass storage controller [0180]: O2 Micro, Inc. Integrated MS/xD Controller [1217:7130] (rev 03)
07:09.2 SD Host controller [0805]: O2 Micro, Inc. Integrated MMC/SD Controller [1217:7120] (rev 02)
```

Chipset is Intel Mobile 4 Series (GM45 class) with the ICH9M southbridge —
matches T3200-era 2008 hardware. No FireWire, no ExpressCard bridge, no
discrete GPU.

## `ip a`

```
2: enp6s0: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 qdisc fq_codel state DOWN group default qlen 1000
    link/ether 00:e0:b8:xx:xx:xx
3: wlp2s0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue state UP group default qlen 1000
    link/ether 00:22:69:xx:xx:xx
    inet 192.168.x.x/24 brd 192.168.x.255 scope global dynamic noprefixroute wlp2s0
```

Ethernet down (no cable), Wi-Fi up — `gh`/`scp` delivery from the dev box
works over the LAN.

## `lsblk -f` + `findmnt /boot`

```
sda
├─sda1
├─sda2 vfat FAT32 [redacted]                             505.8M   1% /boot/efi
└─sda3 ext4 1.0   [redacted] 228.9G  16% /
```

`findmnt /boot` returns nothing → `/boot` lives on `sda3` (ext4 root), not
its own partition.
