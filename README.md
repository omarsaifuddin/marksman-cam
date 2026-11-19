# marksman-cam

Turns a Canon EOS/PowerShot, or a Sony Alpha in PC Remote mode, connected over USB into a webcam on **Linux** and **Windows 11**. No vendor software, SDK, account or subscription: every feature is always on.

Not affiliated with or endorsed by Canon Inc. or Sony Group Corporation. Canon, EOS, PowerShot and Rebel are trademarks of Canon Inc.; Sony and Alpha are trademarks of Sony Group Corporation.

```
camera ──USB PTP (libusb)──► live-view JPEG ──libjpeg-turbo──► crop/rotate/mirror/fit/fill,
                                                               brightness/contrast/saturation/hue
      ──► Linux:   YUYV into a v4l2loopback device (/dev/video10)
      ──► Windows: NV12 into shared memory ──► marksman-cam-vcam.dll (Media Foundation source,
                   loaded by the Windows Camera Frame Server) ──► "Marksman Cam" in every app
```

The picture is the camera's own live-view stream, read over PTP.

## Usage
```
marksman-cam [-d DEV] [-s 1920x1080] [-r 30] [--fill] [--rotate 90] [--mirror] [--flip]
          [--crop 0.1,0,0.9,1] [--brightness N] [--contrast N] [--saturation N] [--hue DEG]
          [--set iso=0x48] [--set kelvin=5200] ...
```
- Start it and leave it running. It reconnects when the camera is unplugged, switched off, or restarted, and shows a grey frame while there is no camera.
- `--set` values are Canon raw property codes (Canon only for now). Examples: iso `0x48`=100, `0x50`=200, `0x58`=400, `0x60`=800, `0x68`=1600, `0x00`=auto. Kelvin is plain Kelvin.
- On Windows the app using the camera picks 640x360, 1280x720 or 1920x1080, and marksman-cam renders at that size.

Tested under Windows 11 on an EOS Rebel T5i / 700D (live view 960x640) and a Sony a6400 in PC Remote mode (live view 1024x576 at a steady 20 fps over USB, through the stock Windows driver).

| Mode | fps | Notes |
|---|---|---|
| Movie mode 1920x1080@30 | 20 (16.5 in the first seconds after start) | The body's fixed live-view-over-USB cadence. Frame size (150 vs 313 KB) doesn't change it; the USB link is ~45% busy |
| Movie mode 1280x720@60, person in frame | 9.3 | The camera slows its USB to 3.5 MB/s (link 82% busy). **Don't use 60p** |
| Stills mode, dim room | 7-10 | Live view slows itself down |

**Use Movie mode at 1080p30.** The camera sets the frame rate, not marksman-cam; output stays at 30 fps by repeating frames. Poll spacing makes no difference.

Live view over USB is the limit: above 20 fps on this body you need HDMI out into a capture dongle.

Camera prep:
- Turn auto power-off off.
- Canon: use a shooting mode, Movie mode where available.
- Sony (e.g. a6400): MENU > Setup > **USB Connection: PC Remote** (not Mass Storage/MTP). Any shooting mode.
- Close any other program using the camera (gphoto2, Canon or Sony software). Only one program can own it.

## Linux (Arch)
```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf libusb libjpeg-turbo v4l2loopback-dkms linux-headers
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
sudo cmake --install build                       # /usr/local/bin/marksman-cam

# virtual camera devices, persistent: one per physical camera (Canon -> video10, Sony -> video11)
echo v4l2loopback | sudo tee /etc/modules-load.d/v4l2loopback.conf
echo 'options v4l2loopback devices=2 video_nr=10,11 card_label="Marksman Cam (Canon)","Marksman Cam (Sony)" exclusive_caps=1,1' | sudo tee /etc/modprobe.d/v4l2loopback.conf
sudo modprobe -r v4l2loopback; sudo modprobe v4l2loopback

# let your user open the cameras without root
printf 'SUBSYSTEM=="usb", ATTR{idVendor}=="%s", TAG+="uaccess"\n' 04a9 054c | sudo tee /etc/udev/rules.d/70-marksman-cam.rules
sudo udevadm control --reload && sudo udevadm trigger

marksman-cam --camera canon --snapshot canon.jpg   # each camera on its own first
marksman-cam --camera sony --snapshot sony.jpg
marksman-cam --camera canon -d /dev/video10 &      # then both at once
marksman-cam --camera sony -d /dev/video11 &       # check with: ffplay /dev/video10, ffplay /dev/video11
```
- One camera only: `devices=1 video_nr=10 card_label="Marksman Cam" exclusive_caps=1`, and plain `marksman-cam` (any make, `/dev/video10`).
- "camera held by another program": gvfs grabs PTP cameras. Run `pkill -f gvfs-gphoto2-volume-monitor`, or `sudo pacman -R gvfs-gphoto2`.
- Secure Boot: sign the v4l2loopback DKMS module, or it will not load.
- Run on login: one user service per camera, the Linux counterpart of the Windows services. `~/.config/systemd/user/marksman-cam@.service`:
  ```ini
  [Service]
  ExecStart=/usr/local/bin/marksman-cam --camera %i -d /dev/video${DEV}
  Restart=always
  ```
  plus `Environment=DEV=10` / `DEV=11` in `systemctl --user edit marksman-cam@canon` / `@sony`. Then `systemctl --user enable --now marksman-cam@canon marksman-cam@sony`.
- On Linux the camera streams for as long as its process runs (v4l2loopback doesn't report when an app is reading); stop the unit to let the camera sleep.
- Other distros need the same packages: Debian/Ubuntu `libusb-1.0-0-dev libturbojpeg0-dev v4l2loopback-dkms`; Fedora `libusb1-devel turbojpeg-devel` plus `akmod-v4l2loopback` from RPM Fusion.

## Windows 11
Requires build 22000 or newer, which has `MFCreateVirtualCamera`.

1. **Build** in an MSYS2 UCRT64 shell. Both binaries are fully static, so they need no MSYS2 DLLs at runtime:
   ```sh
   pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,libusb,libjpeg-turbo}
   cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
   ```
2. **Install**: double-click `build\marksman-cam-setup.exe`. It is a single file you can copy to any PC. Accept the admin prompt and pick **Canon and Sony**, **Canon only** or **Sony only**. To change the choice later, run it again or use Modify in Settings > Apps > Installed apps; Uninstall there removes everything. It runs the same commands as below, without a console window. Or, from an elevated prompt, once per camera:
   ```bat
   build\marksman-cam.exe --install --camera canon
   build\marksman-cam.exe --install --camera sony
   ```
   Each `--install --camera X` does everything for that camera:
   - Copies `marksman-cam.exe` and `marksman-cam-vcam.dll` to `C:\Program Files\marksman-cam` (shared by all cameras).
   - Registers the media source.
   - Creates a **permanent, all-users virtual camera**: "Marksman Cam (Canon)" or "Marksman Cam (Sony)".
   - Installs and starts **its own service**, `marksman-cam-canon` or `marksman-cam-sony`: automatic start, restarts if it crashes, log at `C:\ProgramData\marksman-cam-canon.log` / `-sony.log`.
   - Plain `--install` (no `--camera`) gives one "Marksman Cam" for any make, service `marksman-cam`.

   After that there is nothing to run. Plug the cameras in and pick one (or both) in any app:
   - Each service wakes its camera and starts live view only while an app is reading that virtual camera.
   - It switches live view off 3 s after the last app lets go. The camera is then free for other software, or can sleep.
   - With no camera plugged in, apps get a grey frame until one appears.

   Notes:
   - Options after `--install` become that camera's service settings (e.g. `--install --camera sony --mirror --fill`). Re-run it to change them or to upgrade; it stops all marksman-cam services and the Frame Server so the files can be replaced, then starts them again.
   - Quit any manually started marksman-cam.exe first.
   - `marksman-cam --uninstall --camera sony` removes that camera's service and virtual camera. `marksman-cam --uninstall` removes all of them and the registration. Then delete the folder.
3. **Camera driver: nothing to do.** marksman-cam talks PTP through Windows' own MTP driver (Windows Portable Devices vendor commands), so the camera keeps its normal driver and Explorer import keeps working. A camera you have bound to WinUSB yourself (e.g. with Zadig) is used through libusb instead.
   - Check the camera on its own: `marksman-cam --snapshot test.jpg` saves one live-view frame. It needs no virtual camera or admin rights.
4. **Check:** `vcam_probe.exe "Marksman Cam (Sony)"` (in `build\`) opens that virtual camera like an app and reads 90 frames (no argument: the first Marksman Cam). A camera another app is using can't be probed (0xC00D3704). It reports:
   - fallback-grey frames (flicker);
   - delivered fps and distinct-picture fps;
   - the first frame's luma: **57** = no camera found (marksman-cam's grey), **48** = nothing is feeding the DLL (service not running), anything else = a live picture.
5. **Without installing** (development): run `build\marksman-cam.exe [--camera canon|sony]` as a normal user. It creates a temporary virtual camera that disappears when it exits. The DLL must have been registered once (by `--install`, or `regsvr32`).

## Layout
| File | What |
|---|---|
| `src/ptp.c` | PTP over USB bulk, plus Canon EOS live view (remote mode, EVF output to PC, GetViewFinderData, keep-alive, properties) and Sony PC Remote live view (SDIO connect, live-view object 0xFFFFC002, property-poll keep-alive) |
| `src/ptp_wpd.cpp` | Windows transport: the same PTP operations through the stock MTP driver |
| `src/image.c` | JPEG decode, geometry (crop, rotate, mirror, fit/fill, bilinear scaling), picture controls, YUYV/NV12 |
| `src/sink_v4l2.c` | Linux output |
| `src/sink_mfvcam.c` | Windows output: shared memory plus `MFCreateVirtualCamera` (system lifetime when installed, session lifetime when run by hand) |
| `src/winsvc.c` | `--install` / `--uninstall` and the Windows service |
| `win/vcam_source.cpp` | The COM media source the Frame Server loads (IMFActivate → IMFMediaSourceEx/IKsControl → IMFMediaStream2) |
| `win/mcam_shm.h` | The shared-memory contract between the two |
| `test/test_core.c` | Self-check (`ctest`): PTP framing over a fake USB pipe, live-view parsing, geometry, colour |
| `test/vcam_probe.c` | Windows end-to-end check |

## Not implemented yet
- Scenes, multi-source compositing, overlays, background blur: use OBS on top of the Marksman Cam device.
- Wi-Fi (PTP/IP).
- Exposing camera controls (`IKsControl`, V4L2 controls) to apps.
- 60 fps.
- macOS.
