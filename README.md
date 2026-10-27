# Samsung Galaxy S21 Ultra (p3s) port for postmarketOS / Nura

An **unofficial**, out-of-tree port of [postmarketOS](https://postmarketos.org) (now called
**Nura**) to the **Samsung Galaxy S21 Ultra 5G, Exynos version (SM-G998B, codename `p3s`)**.

> **Made with AI assistance.** Large parts of this port (kernel backports, `p3s-fbmirror`,
> `p3s-console-size`, configs) were written with help from an AI assistant (Claude), and
> tested by me on my own phone. Nura's [AI policy](https://docs.nura.eco/policies-and-processes/development/ai-policy.html)
> does not accept AI-generated contributions, so this port is **not** submitted upstream and is
> **not** affiliated with or supported by the Nura / postmarketOS project. Please don't ask
> for help with it in Nura's channels; open an issue here instead.

## Status

| Feature | Status |
|---|---|
| Boot, downstream 5.4 kernel ([ProtonPlus](https://github.com/ProtonKernel/Proton)) | ✅ works |
| USB networking + SSH (172.16.42.1) | ✅ works |
| Display: text console | ✅ works (see below) |
| Display: power button / screen timeout (real panel power-down) | ✅ works |
| Wi-Fi (Broadcom BCM4389, 2.4 + 5 GHz) | ✅ works |
| Firewall (nftables), systemd-tmpfiles / sysusers | ✅ works |
| GPU (Mali-G78) | ❌ no acceleration (needs a userspace backend for `mali_kbase`) |
| Touchscreen, Bluetooth, audio, modem, battery status, suspend, cameras | ❓ not tested / not done |

Only the **console** UI has been tested.

### How the display works

The panel is a command-mode DSI panel driven by Samsung's own `decon` fbdev driver, which
only sends a frame when asked to, and which the kernel console cannot draw on. So the kernel
console draws into a `simple-framebuffer`, and **`p3s-fbmirror`** (an OpenRC service,
`p3s-display`) copies it to the panel. It also handles the power button: press to switch the
panel off and on, and it switches off after 120 s without a key press (`-t` in
`/etc/conf.d/p3s-display`).

### Damaged screen? Shrink the console

By default the console uses the full 1440×3200 panel. If the edges of your panel are damaged
(as on my phone), make the console smaller. It is then drawn at the **top right**, away from
the left and bottom edges:

```sh
p3s-console-size /boot/vendor_boot.img /tmp/vendor_boot-small.img 1280 2560
doas dd if=/dev/zero of=/dev/disk/by-partlabel/vendor_boot bs=4M   # ends with "no space left", that's fine
doas dd if=/tmp/vendor_boot-small.img of=/dev/disk/by-partlabel/vendor_boot bs=4M && sync
```

Or run the same `p3s-console-size` on the `vendor_boot.img` from `pmbootstrap export` before
flashing. (The 1280×2560 console is what runs on my phone; running the tool on the phone
itself, as shown above, has not been tested yet.)

### Kernel changes

On top of ProtonPlus (`a0d73239`), three small backports from Linux 5.8, needed by current
systemd tools (`systemd-tmpfiles`, `systemd-sysusers`) on a 5.4 kernel:

- `statx-add-mount-id.patch` – statx: add mount ID (upstream fa2fcf4f1df1)
- `statx-add-mount-root.patch` – statx: add mount_root (upstream 80340fe3605c)
- `faccessat2.patch` – vfs: add faccessat2 syscall (upstream c8ffd8bcdad2)

The kernel config passes `pmbootstrap kconfig check` and enables nftables.

## Packages

| Package | What |
|---|---|
| `device/downstream/linux-samsung-p3s` | kernel |
| `device/downstream/device-samsung-p3s` | device package (deviceinfo, DTB, `p3s-fbmirror`, `p3s-console-size`, module loading, NetworkManager config) |
| `device/downstream/firmware-samsung-p3s` | Wi-Fi/Bluetooth firmware, downloaded from [andersabra/firmware-samsung-p3s](https://github.com/andersabra/firmware-samsung-p3s) |

## Installing

**Warning:** you need an unlocked bootloader and TWRP (or another way to write partitions).
Unlocking trips Knox permanently, and flashing the rootfs **erases userdata**. Back up first
(TWRP backups of `boot`, `vendor_boot`, `dtbo` are a good idea).

1. Install [pmbootstrap](https://wiki.postmarketos.org/wiki/Pmbootstrap) and run
   `pmbootstrap init` once, so it has a pmaports checkout.
2. Copy the three packages into that checkout:
   ```sh
   git clone https://github.com/andersabra/samsung-p3s-postmarket-port
   cp -r samsung-p3s-postmarket-port/device/downstream/* "$(pmbootstrap config aports)/device/downstream/"
   ```
3. Run `pmbootstrap init` again and choose vendor `samsung`, device `p3s`, UI `console`,
   and say **yes** to non-free firmware.
4. Build and create the images (the kernel takes a while):
   ```sh
   pmbootstrap build linux-samsung-p3s
   pmbootstrap install --password 'yourpassword' --add firmware-samsung-p3s
   pmbootstrap export
   ```
   `pmbootstrap export` puts `boot.img`, `vendor_boot.img` and `samsung-p3s.img` in
   `/tmp/postmarketOS-export/`.
5. Boot the phone into TWRP and flash (from the folder with the images):
   ```sh
   adb shell umount /data
   adb push boot.img /tmp/boot.img
   adb push vendor_boot.img /tmp/vendor_boot.img
   adb push samsung-p3s.img /tmp/pmos.img
   adb shell "dd if=/dev/zero of=/dev/block/by-name/boot bs=4M"
   adb shell "dd if=/dev/zero of=/dev/block/by-name/vendor_boot bs=4M"
   adb shell dd if=/tmp/boot.img of=/dev/block/by-name/boot bs=4M
   adb shell dd if=/tmp/vendor_boot.img of=/dev/block/by-name/vendor_boot bs=4M
   adb shell dd if=/tmp/pmos.img of=/dev/block/by-name/userdata bs=4M
   adb shell sync
   adb reboot
   ```
   The two `/dev/zero` commands end with "No space left on device"; that is expected.
6. Connect a USB cable and `ssh user@172.16.42.1` (the user you chose in `pmbootstrap init`).
   Wi-Fi: `doas nmcli device wifi connect "SSID" --ask`.
   To reboot into recovery or download mode: `doas reboot-mode recovery` / `doas reboot-mode download`.

## Licenses

- Package files (APKBUILDs, scripts, `p3s-fbmirror.c`, `p3s-console-size.c`, configs): MIT.
- Kernel patches: GPL-2.0, backports of upstream Linux commits (original author Miklos Szeredi).
- The firmware is proprietary (Samsung/Broadcom) and lives in a separate repository.
