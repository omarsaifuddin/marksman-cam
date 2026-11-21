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
          [--crop 0.1,0,0.9,1] [--zoom 150] [--brightness N] [--contrast N] [--saturation N] [--hue DEG]
          [--set iso=0x48] [--set kelvin=5200] [--af] [--camera canon|sony|any] [--serial SN]
          [--stats] [--control-port N]
marksman-cam --list                       # connected cameras and their serial numbers
marksman-cam [--camera sony] --control "list" | "set iso 400" | "set af 1" | "set focus -2"
```
- Start it and leave it running. It reconnects when the camera is unplugged, switched off, or restarted, and shows a grey frame while there is no camera.
- `--set` takes the camera's raw value; `--control list` shows every value the camera offers, with its label. Canon examples: iso `0x48`=100, `0x50`=200, `0x58`=400, `0x60`=800, `0x68`=1600, `0x00`=auto; kelvin is plain Kelvin. Sony: iso `400`, aperture `560` (= f/5.6).
- `--serial` picks one camera when several of the same make are connected (`--list` shows the serials). `--af` autofocuses once each time the camera connects.
- `--stats` prints, once a second, the camera's frame rate, how long each live-view transfer takes, and the output frame rate and render time. Use it to see where frames go.
- On Windows the app using the camera picks 640x360, 1280x720 or 1920x1080, and marksman-cam renders at that size. On Linux the size is fixed by `-s` (default 1280x720).

Tested under Windows 11 on an EOS Rebel T5i / 700D (live view 960x640) and a Sony a6400 in PC Remote mode (live view 1024x576 at a steady 20 fps over USB, through the stock Windows driver).

| Mode | fps | Notes |
|---|---|---|
| Movie mode 1920x1080@30 | 20 (16.5 in the first seconds after start) | The body's fixed live-view-over-USB cadence. Frame size (150 vs 313 KB) doesn't change it; the USB link is ~45% busy |
| Movie mode 1280x720@60, person in frame | 9.3 | The camera slows its USB to 3.5 MB/s (link 82% busy). **Don't use 60p** |
| Stills mode, dim room | 7-10 | Live view slows itself down |

**Use Movie mode at 1080p30.** The camera sets the frame rate, not marksman-cam. Each frame goes out the moment it arrives; there is no 30 Hz output tick, which added up to a frame of latency and judder against the camera's 20 fps. The last frame is repeated only when the camera stalls. Poll spacing makes no difference.

Per-frame CPU cost (decode + scale + colour, Ryzen 9800X3D, 1024x576 live view): 1080p NV12 4.3 ms, 720p 1.9 ms. The JPEG is decoded straight to its YUV planes and each output sample is read from them through precomputed bilinear taps, with no RGB step. The previous RGB path took 22 ms at 1080p, which slower CPUs could not fit in a frame.

Live view over USB is the limit: above 20 fps on this body you need HDMI out into a capture dongle.

Camera prep:
- Turn auto power-off off.
- Canon: use a shooting mode, Movie mode where available.
- Sony (e.g. a6400): MENU > Setup > **USB Connection: PC Remote** (not Mass Storage/MTP). Any shooting mode.
- Close any other program using the camera (gphoto2, Canon or Sony software). Only one program can own it.

## Camera controls in apps and OBS
Every running marksman-cam serves its controls on **127.0.0.1**: port 47811 Canon, 47812 Sony, 47810 any make (`--control-port` to change, `0` to turn off).
- **Picture**: brightness, contrast, saturation, hue, digital zoom, mirror, flip, fill, rotate. Always available.
- **Camera**, while an app is showing the camera (it is only woken then): autofocus, manual focus (nearer/farther in three step sizes), and every setting the camera reports with a list or range of values: ISO, shutter, aperture, exposure compensation, white balance, colour temperature, picture style, and on Sony anything else in its property list. Read-only ones (e.g. the mode dial) are shown greyed out.

**OBS**: Tools > Scripts > + > `marksman-cam.py` (Windows: `C:\Program Files\marksman-cam\marksman-cam.py`; Linux: `/usr/local/share/marksman-cam/marksman-cam.py`; or `obs/` in this repo). Pick the camera, show its source, and press Refresh once the picture is up. On Windows, OBS needs Python 3 set in Tools > Scripts > Python Settings.

**Windows apps**: brightness, contrast, hue, saturation and zoom are also standard camera controls (VideoProcAmp / CameraControl), for Settings > Bluetooth & devices > Cameras, OBS's "Configure Video", and other apps' camera settings.

**Anything else**: one text line per connection, one reply. `list` returns JSON (`name`, `label`, `type` range/bool/menu/button/step/info, `value`, `min`/`max`/`step` or `options`); `set NAME VALUE` returns `ok` or `error: ...`. Values are checked against what the camera offers. Only the first line of a connection is read, so a web page posting to the port can't send commands.

Changes made this way last until the service restarts; put them in the service options (`--set`, `--brightness`, ...) to keep them.

## Linux
> **Known issue: on Linux only one camera works at a time.** With a Canon and a Sony both connected, you can pick either one, but when both are in use (e.g. two sources in OBS) one of them shows the grey frame. On Windows both cameras work at the same time. `--camera canon` or `--camera sony` with a single camera works.

Works on any systemd distribution with v4l2loopback: Arch (and CachyOS, EndeavourOS, Manjaro), Debian/Ubuntu (and Mint, Pop!_OS), Fedora, openSUSE, Void. Run as your normal user; it asks for sudo where it needs root:
```sh
./install.sh                      # Canon and Sony, each its own virtual camera
./install.sh --camera sony        # or: canon | sony | both | any
./install.sh -- --fill --mirror   # options after -- go to the services
./install.sh --uninstall
```
It does the same job as the Windows setup:
- Installs the build tools, libusb, libjpeg-turbo and v4l2loopback with the distro's package manager. It skips v4l2loopback when the kernel already ships it (CachyOS does). On Arch it installs headers for every installed kernel; Fedora needs RPM Fusion for `akmod-v4l2loopback`. `--no-deps` skips this step.
- Builds, runs the self-test, and installs `/usr/local/bin/marksman-cam` (`--prefix` to change).
- Creates one v4l2loopback device per camera, persistent across reboots: **"Marksman Cam (Canon)"** (`/dev/video10`), **"Marksman Cam (Sony)"** (`/dev/video11`), or **"Marksman Cam"** for `--camera any`. Config: `/etc/modprobe.d/marksman-cam.conf` and `/etc/modules-load.d/marksman-cam.conf`. `--obs` adds a spare "OBS Virtual Camera" device so OBS doesn't need one of these.
- Installs `/etc/udev/rules.d/70-marksman-cam.rules`. It lets the logged-in user open Canon/Sony PTP cameras without root, and removes the libgphoto2/libmtp tags from those cameras so gvfs (GNOME, Xfce, Cinnamon) stops mounting them and locking marksman-cam out. Other cameras are not affected.
- Enables one **systemd user service** per camera, `marksman-cam@canon` / `@sony` / `@any`, started at login and restarted if it crashes.

After that there is nothing to run. Plug the camera in and pick it in any app:
- The service wakes the camera and starts live view only while an app is streaming from its device (v4l2loopback 0.13+ reports this; with older modules, any other process holding the device open counts).
- It switches live view off 3 s after the last app stops. The camera can then sleep or be used by other software.
- While nobody is watching, or no camera is plugged in, apps get a grey frame.

Day to day:
```sh
journalctl --user -u marksman-cam@sony -f          # log
$EDITOR ~/.config/marksman-cam/sony.conf           # OPTIONS="--fill -s 1920x1080 --set iso=0x48"
systemctl --user restart marksman-cam@sony
systemctl --user stop marksman-cam@sony            # frees the camera; then check it on its own:
marksman-cam --camera sony --snapshot sony.jpg
ffplay /dev/video11                                # or open the camera in a browser
```
- Two cameras of the same make: `--serial` (see `marksman-cam --list`) picks which one a service uses.
- `-d` takes a path or a card label (`-d "Marksman Cam (Sony)"`). Without it marksman-cam finds the device labelled for `--camera`, so the `/dev/videoN` numbers don't matter. It refuses to write to anything that isn't a v4l2loopback device.
- "camera held by another program": something opened it before the udev rule applied. Unplug and replug it, or `pkill -f gvfs-gphoto2-volume-monitor`.
- "the camera reports itself busy over USB": the camera is stuck in an earlier session (for example from another computer). Switch it off and on.
- Secure Boot: a DKMS-built v4l2loopback must be signed with an enrolled key (MOK), or it won't load. The installer warns when that is the problem.
- `MARKSMAN_DEBUG=1 marksman-cam ...` traces every PTP operation.
- Manual install, or a distro without systemd: `cmake -S . -B build -G Ninja && cmake --build build && sudo cmake --install build`, then set up the modprobe options and udev rule above by hand and start `marksman-cam --camera canon` from your session's autostart.

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
   - **Upgrading** from a build without camera controls: run the setup (or `--install`) again for each camera. The service and the media source DLL share a new frame layout, so they must be the same version.
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
| `src/ptp.c` | PTP over USB bulk, plus Canon EOS live view (remote mode, EVF output to PC, GetViewFinderData, keep-alive, property events, autofocus, lens drive) and Sony PC Remote live view (SDIO connect, live-view object 0xFFFFC002, property block as keep-alive and property list, property set, half-press autofocus, focus drive); value labels |
| `src/ptp_wpd.cpp` | Windows transport: the same PTP operations through the stock MTP driver |
| `src/image.c` | JPEG decode to YUV planes, geometry (crop, zoom, rotate, mirror, fit/fill) as separable bilinear taps, picture controls, YUYV/NV12 |
| `src/ctl.c` | Control port: picture and camera controls as JSON; `set` commands checked, then queued for the capture thread |
| `obs/marksman-cam.py` | OBS script: builds its controls from the control port's `list` |
| `src/sink_v4l2.c` | Linux output: v4l2loopback device by label, demand detection (client-usage events or /proc) |
| `install.sh` | Linux installer: packages, build, v4l2loopback devices, udev rule, user services |
| `linux/` | systemd user unit template and udev rule |
| `src/sink_mfvcam.c` | Windows output: shared memory plus `MFCreateVirtualCamera` (system lifetime when installed, session lifetime when run by hand) |
| `src/winsvc.c` | `--install` / `--uninstall` and the Windows service |
| `win/vcam_source.cpp` | The COM media source the Frame Server loads (IMFActivate → IMFMediaSourceEx/IKsControl → IMFMediaStream2); hands each new frame over as soon as marksman-cam signals it; VideoProcAmp/zoom controls |
| `win/mcam_shm.h` | The shared-memory contract between the two |
| `test/test_core.c` | Self-check (`ctest`): PTP framing over a fake USB pipe, live-view parsing, Canon event and Sony property parsing, focus commands, value labels, control protocol, geometry, colour, a real JPEG decode |
| `test/vcam_probe.c` | Windows end-to-end check |

## Not implemented yet
- Scenes, multi-source compositing, overlays, background blur: use OBS on top of the Marksman Cam device.
- Wi-Fi (PTP/IP).
- Camera settings (ISO, shutter, ...) as standard controls inside apps. On Windows only the picture controls are; v4l2loopback can't pass control changes back to its writer at all. Use the OBS script or the control port.
- Keeping control changes made at runtime.
- 60 fps.
- macOS.
- Two cameras at once on Linux (see the known issue under Linux; Windows is fine).
