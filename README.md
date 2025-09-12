## Insyde DCHU Kernel driver

> [!NOTE]
> Deprecation in near future, in favor of [Tuxedo Computers Drivers](https://github.com/tuxedocomputers/tuxedo-drivers) and [Clevo driver patch+main tuxedo fork](https://github.com/nick42d/clevo-drivers?tab=readme-ov-file)
> What are still missing on those:
> - Battery
> - Fan state and PWM
> Contrib soon!

DCHU is a kernel-space devices that integrates:
- dchu\_core: Contact with ACPI device `CLV0001` aka DCHU
- dchu\_hwmon: hwmon child exposing fans, PWM duty, and temps via the FAN package
- dchu\_leds: LED child controlling keyboard backlight levels (0–5), RGB is not planned for support since there is no test device

Features are replicated from Gigabyte's "Control Center" (CC not GCC) - ControlCenter\_3.55

Tested on `Gigabyte U4 UD`, which on latest upstream doesn't have suitable fan drivers 

### Subapps UWP implemented:

- [ ] BatteryUtility
- [x] CC3.0 (pretty much hobo)
- [ ] CPU\_OC
- [ ] EnergySave
- [x] FanSpeedSettings
- [ ] FlexiKey
- [ ] FnKey (todo)
- [ ] GPUOverclocking
- [x] Keyboard

### Requirements
- Linux kernel headers
- Firmware implementing DCHU `\_DSM` with UUID `E4 24 F2 93 DC FB BF 4B AD D6 DB 71 BD C0 AF AD`

### Build
- Build all modules:
  - `make modules_all`
- Or explicitly:
  - `make modules MODULES="dchu_core dchu_hwmon dchu_leds"`

#### Using custom kernel headers (manual build)
- Point `KDIR` at your kernel build tree (must have `make modules_prepare` run):
  - `make modules_all KDIR=/path/to/linux/build`
  - Example: if you built with `O=/path/to/build`, use that `O` dir as `KDIR`.

### DKMS (Arch Linux)
- Package build:
  - `makepkg -f` (produces `insyde-dchu-dkms-<ver>-x86_64.pkg.tar.zst`)
- Install the DKMS package:
  - `sudo pacman -U insyde-dchu-dkms-*.pkg.tar.zst`
- DKMS will auto-build for installed kernels and on updates.
- Manual DKMS (optional):
  - `sudo dkms add -m insyde-dchu-dkms -v 0.1.0`
  - `sudo dkms build -m insyde-dchu-dkms -v 0.1.0`
  - `sudo dkms install -m insyde-dchu-dkms -v 0.1.0`

#### DKMS with custom kernel headers
- Preferred: install/symlink your kernel headers to `/lib/modules/<release>/build`, then:
  - `sudo dkms build -m insyde-dchu-dkms -v 0.1.0 -k <release>`
- Advanced (enabled in dkms.conf): pass `KDIR` to override the kernel dir DKMS uses:
  - `sudo KDIR=/path/to/linux/build dkms build -m insyde-dchu-dkms -v 0.1.0 -k <release>`
  - Ensure `<release>` matches the kernel built in `KDIR` (ABI match).

### Load / Unload
- Load in order (hwmon params optional):
  - `sudo insmod ./dchu_core.ko`
  - `sudo insmod ./dchu_hwmon.ko invert=1 tach_hz=35940 ppr=1 le=1`
  - `sudo insmod ./dchu_leds.ko`
- Or:
  - `make load_all PARAMS="invert=1 tach_hz=35940 ppr=1 le=1"`
- Unload:
  - `sudo rmmod dchu_hwmon dchu_leds dchu_core`
  - Or `make unload_all`

## DCHU spec

### hwmon Child (Fans/PWM/Temps)
- Sysfs: `/sys/class/hwmon/hwmonX/` with `name` = `dchu`
- Exposed attributes:
  - Fans (RPM): `fan1_input` (CPU), `fan2_input` (GPU1), `fan3_input` (GPU2)
  - PWM (0–255): `pwm1`, `pwm2`, `pwm3` (scaled from 0–100 duty; clamped ≤255)
  - Temps (m°C): `temp1_input` (CPU remote), `temp2_input` (GPU1), `temp3_input` (GPU2)
- Debug: `fan_buf` (hex dump of FAN package 12)
- Fan controls:
  - `fan_mode` (RW): accepts numeric (0/1/3/5/6/7) or names (auto, max, silent, maxq, custom, turbo). Write invokes `_DSM` command `121` with a 4-byte payload: `payload[0]=mode`, `payload[1]=0`, `payload[2]=0`, `payload[3]=1` (subcommand).
  - `fan_mode_name` (RO): the name of the last set mode.
- Parse table (FAN package id = 12):
  - CPU RPM: `(buf[2] << 8) | buf[3]`
  - GPU1 RPM: `(buf[4] << 8) | buf[5]`
  - GPU2 RPM: `(buf[6] << 8) | buf[7]`
  - CPU duty: `buf[16]` (0–100)
  - GPU1 duty: `buf[19]` (0–100)
  - GPU2 duty: `buf[22]` (0–100)
  - CPU remote: `buf[18]` (°C)
  - GPU1 remote: `buf[21]` (°C)
  - GPU2 remote: `buf[24]` (°C)

### LEDs Child (Keyboard backlight)
- LED: `/sys/class/leds/dchu::kbd_backlight` (max\_brightness = 5)
- Read: `_DSM` function `61` returns an integer; low byte is brightness `0..5`
- Write: `_DSM` function `39` with 4-byte payload, `payload[0]=level (0..5)`, others `0`
- Helpers on platform device (debug):
  - `.../dchu-leds.0/raw_status` → prints `_DSM 61` result or error
  - `.../dchu-leds.0/raw_set` → write a number to invoke `_DSM 39`
