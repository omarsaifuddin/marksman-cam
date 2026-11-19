#!/usr/bin/env bash
# marksman-cam Linux installer: build dependencies, build, install, v4l2loopback devices, udev rule,
# and one systemd user service per camera. The Linux counterpart of marksman-cam-setup.exe.
#
#   ./install.sh                         Canon and Sony, each its own virtual camera
#   ./install.sh --camera sony           one camera make only (canon | sony | both | any)
#   ./install.sh -- --fill --mirror      everything after -- becomes the services' marksman-cam options
#   ./install.sh --uninstall
#
# Run it as your normal user; it asks for sudo where it needs root.
set -euo pipefail

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=/usr/local
CAMERA=both
DEPS=1
SERVICES=1
OBS=0
UNINSTALL=0
OPTIONS=()

MODPROBE_CONF=/etc/modprobe.d/marksman-cam.conf
MODULES_CONF=/etc/modules-load.d/marksman-cam.conf
UDEV_RULE=/etc/udev/rules.d/70-marksman-cam.rules

usage() {
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    cat <<EOF
Options:
  --camera canon|sony|both|any   which virtual cameras to create (default: both)
  --prefix DIR                   install prefix (default: /usr/local)
  --no-deps                      don't install packages; you provide the build tools and v4l2loopback
  --no-service                   don't enable the systemd user services
  --obs                          also create a spare "OBS Virtual Camera" loopback device for OBS
  --uninstall                    remove everything this script installed (packages are kept)
EOF
}

say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

while [ $# -gt 0 ]; do
    case $1 in
        --camera) CAMERA=${2:?}; shift ;;
        --camera=*) CAMERA=${1#*=} ;;
        --prefix) PREFIX=${2:?}; shift ;;
        --prefix=*) PREFIX=${1#*=} ;;
        --no-deps) DEPS=0 ;;
        --no-service) SERVICES=0 ;;
        --obs) OBS=1 ;;
        --uninstall) UNINSTALL=1 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; OPTIONS=("$@"); break ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done
case $CAMERA in canon|sony|both|any) ;; *) die "--camera must be canon, sony, both or any" ;; esac
[ "$(uname -s)" = Linux ] || die "this installer is for Linux; on Windows use marksman-cam-setup.exe"

# --- who we are: build and user services run as the desktop user, system files as root ----------
if [ "$(id -u)" -eq 0 ]; then
    TARGET_USER=${SUDO_USER:-}
    [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != root ] ||
        die "run this as your normal user (it uses sudo itself), or via sudo from that user"
else
    TARGET_USER=$(id -un)
    have sudo || die "sudo is needed for the system-wide steps"
fi
TARGET_UID=$(id -u "$TARGET_USER")
if [ "$(id -u)" -eq 0 ]; then
    TARGET_HOME=$(getent passwd "$TARGET_USER" | cut -d: -f6)
    CONFIG_DIR=$TARGET_HOME/.config/marksman-cam
else
    TARGET_HOME=$HOME
    CONFIG_DIR=${XDG_CONFIG_HOME:-$HOME/.config}/marksman-cam # the unit reads %E/marksman-cam
fi

as_root() { if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi; }
as_user() {
    if [ "$(id -u)" -eq 0 ]; then
        sudo -u "$TARGET_USER" env HOME="$TARGET_HOME" XDG_RUNTIME_DIR="/run/user/$TARGET_UID" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$TARGET_UID/bus" "$@"
    else
        "$@"
    fi
}
user_systemctl() { as_user systemctl --user "$@"; }
has_systemd() { [ -d /run/systemd/system ]; }

# --- camera choice -> service instances and loopback devices -------------------------------------
case $CAMERA in
    both)  INSTANCES=(canon sony); LABELS=("Marksman Cam (Canon)" "Marksman Cam (Sony)"); NRS=(10 11) ;;
    canon) INSTANCES=(canon);      LABELS=("Marksman Cam (Canon)");                        NRS=(10) ;;
    sony)  INSTANCES=(sony);       LABELS=("Marksman Cam (Sony)");                         NRS=(11) ;;
    any)   INSTANCES=(any);        LABELS=("Marksman Cam");                                NRS=(10) ;;
esac
if [ "$OBS" = 1 ]; then LABELS+=("OBS Virtual Camera"); NRS+=(12); fi

stop_running() { # services first, or Restart=always brings them straight back
    local i
    if has_systemd; then
        for i in canon sony any; do user_systemctl stop "marksman-cam@$i.service" 2>/dev/null || true; done
    fi
    if pkill -x marksman-cam 2>/dev/null; then sleep 1; fi
}

# =================================================================================================
uninstall() {
    say "Stopping services"
    if has_systemd; then
        for i in canon sony any; do
            user_systemctl disable --now "marksman-cam@$i.service" 2>/dev/null || true
        done
    fi
    stop_running
    say "Removing files"
    as_root rm -f "$PREFIX/bin/marksman-cam" "$PREFIX/lib/systemd/user/marksman-cam@.service" \
        "$PREFIX/share/marksman-cam/70-marksman-cam.rules" "$MODPROBE_CONF" "$MODULES_CONF" "$UDEV_RULE"
    as_root rmdir "$PREFIX/share/marksman-cam" 2>/dev/null || true
    has_systemd && user_systemctl daemon-reload 2>/dev/null || true
    have udevadm && as_root udevadm control --reload || true
    if v4l2loopback_loaded; then
        as_root modprobe -r v4l2loopback 2>/dev/null ||
            warn "v4l2loopback is in use; its Marksman Cam devices disappear after a reboot"
    fi
    say "Done. Kept: installed packages, and your options in $CONFIG_DIR/"
}

# =================================================================================================
# Distro detection and packages. pkgconfig(...) names let dnf/zypper find the right -devel packages.
# shellcheck disable=SC1091
. /etc/os-release 2>/dev/null || true

sbin() { PATH="$PATH:/usr/sbin:/sbin" command -v "$1"; } # modinfo etc. aren't on a user's PATH everywhere
v4l2loopback_available() { "$(sbin modinfo || echo modinfo)" v4l2loopback >/dev/null 2>&1; }
v4l2loopback_loaded() { grep -q '^v4l2loopback ' /proc/modules; }

arch_headers() { # headers package for every installed Arch kernel, e.g. linux-cachyos-headers
    local f
    for f in /usr/lib/modules/*/pkgbase; do
        if [ -r "$f" ]; then printf '%s-headers\n' "$(cat "$f")"; fi
    done | sort -u
}

install_deps() {
    local need_loop=1 pm=""
    v4l2loopback_available && need_loop=0
    if have pacman; then pm=pacman
    elif have apt-get; then pm=apt
    elif have dnf; then pm=dnf
    elif have zypper; then pm=zypper
    elif have xbps-install; then pm=xbps
    fi
    say "Installing build dependencies ($pm, ${PRETTY_NAME:-unknown distro})"
    [ $need_loop = 0 ] && say "v4l2loopback is already available for this kernel; not installing it"
    case $pm in
        pacman)
            as_root pacman -S --needed --noconfirm base-devel cmake ninja pkgconf libusb libjpeg-turbo
            if [ $need_loop = 1 ]; then
                mapfile -t hdrs < <(arch_headers)
                [ ${#hdrs[@]} -gt 0 ] || hdrs=(linux-headers)
                as_root pacman -S --needed --noconfirm v4l2loopback-dkms "${hdrs[@]}"
            fi ;;
        apt)
            as_root apt-get update
            as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y \
                build-essential cmake ninja-build pkg-config libusb-1.0-0-dev libturbojpeg0-dev
            if [ $need_loop = 1 ]; then
                as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y v4l2loopback-dkms \
                    "linux-headers-$(uname -r)" ||
                as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y v4l2loopback-dkms
            fi ;;
        dnf)
            as_root dnf install -y gcc gcc-c++ cmake ninja-build pkgconf-pkg-config \
                'pkgconfig(libusb-1.0)' 'pkgconfig(libturbojpeg)'
            if [ $need_loop = 1 ]; then
                if as_root dnf install -y akmod-v4l2loopback v4l2loopback; then
                    as_root akmods --force >/dev/null 2>&1 || true # build for the running kernel now
                else
                    warn "v4l2loopback not found: enable RPM Fusion (free), then: sudo dnf install akmod-v4l2loopback"
                fi
            fi ;;
        zypper)
            as_root zypper --non-interactive install gcc gcc-c++ cmake ninja pkg-config \
                'pkgconfig(libusb-1.0)' 'pkgconfig(libturbojpeg)'
            if [ $need_loop = 1 ]; then
                as_root zypper --non-interactive install v4l2loopback-kmp-default ||
                warn "v4l2loopback not found: install v4l2loopback-kmp for your kernel flavour (Packman or the multimedia repo)"
            fi ;;
        xbps)
            as_root xbps-install -Sy base-devel cmake ninja pkg-config libusb-devel libjpeg-turbo-devel
            if [ $need_loop = 1 ]; then as_root xbps-install -y v4l2loopback; fi ;;
        *)
            warn "unknown package manager. Install: a C/C++ compiler, cmake, pkg-config, libusb-1.0 and"
            warn "libturbojpeg development files, and v4l2loopback for your kernel; then rerun with --no-deps"
            exit 1 ;;
    esac
}

check_build_tools() {
    local m=()
    for t in cc cmake pkg-config; do have $t || m+=("$t"); done
    [ ${#m[@]} -eq 0 ] || die "missing: ${m[*]} (rerun without --no-deps, or install them)"
    pkg-config --exists libusb-1.0 || die "libusb-1.0 development files not found"
    pkg-config --exists libturbojpeg || die "libturbojpeg development files not found"
}

build_and_install() {
    local gen=() build="$SRC/build"
    have ninja && gen=(-G Ninja)
    say "Building in $build"
    as_user cmake -S "$SRC" -B "$build" "${gen[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
    as_user cmake --build "$build" --parallel
    as_user ctest --test-dir "$build" --output-on-failure >/dev/null || die "self-test failed (ctest --test-dir build)"
    say "Installing to $PREFIX"
    as_root cmake --install "$build" >/dev/null
}

# --- v4l2loopback devices ----------------------------------------------------------------------
setup_loopback() {
    local labels nrs excl conf other
    labels=$(printf '"%s",' "${LABELS[@]}"); labels=${labels%,}
    nrs=$(IFS=,; echo "${NRS[*]}")
    excl=$(printf '1,%.0s' "${NRS[@]}"); excl=${excl%,}
    conf="options v4l2loopback devices=${#NRS[@]} video_nr=$nrs card_label=$labels exclusive_caps=$excl"

    other=$(grep -l '^[[:space:]]*options[[:space:]]\+v4l2loopback' /etc/modprobe.d/*.conf /usr/lib/modprobe.d/*.conf 2>/dev/null |
            grep -v "^$MODPROBE_CONF\$" || true)
    [ -z "$other" ] || warn "other v4l2loopback options found in: $other -- they may clash with $MODPROBE_CONF"

    say "Configuring v4l2loopback: ${LABELS[*]}"
    printf '# written by marksman-cam install.sh\n%s\n' "$conf" | as_root tee "$MODPROBE_CONF" >/dev/null
    printf '# written by marksman-cam install.sh\nv4l2loopback\n' | as_root tee "$MODULES_CONF" >/dev/null

    if ! v4l2loopback_available; then
        warn "v4l2loopback is not built for kernel $(uname -r) yet (new DKMS install or kernel update?)."
        warn "Reboot, then check with: ls /sys/class/video4linux/*/name; cat /sys/class/video4linux/*/name"
        return
    fi
    # reload so the new labels take effect; the module can't be unloaded while a device is open
    stop_running
    if v4l2loopback_loaded; then
        as_root modprobe -r v4l2loopback 2>/dev/null ||
            { warn "v4l2loopback is in use (OBS? a browser?); close it and run: sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback, or reboot"; return; }
    fi
    if ! as_root modprobe v4l2loopback; then
        warn "could not load v4l2loopback."
        if have mokutil && mokutil --sb-state 2>/dev/null | grep -qi enabled; then
            warn "Secure Boot is on: the DKMS-built module must be signed with an enrolled key (MOK)."
            warn "Debian/Ubuntu: sudo dpkg-reconfigure v4l2loopback-dkms walks you through it; Fedora: akmods + kmodgenca."
        fi
    fi
}

setup_udev() {
    say "Installing udev rule (camera access for your user, keeps gvfs from grabbing the camera)"
    as_root install -Dm644 "$SRC/linux/70-marksman-cam.rules" "$UDEV_RULE"
    if have udevadm; then
        as_root udevadm control --reload
        as_root udevadm trigger --subsystem-match=usb --attr-match=idVendor=04a9 --action=change || true
        as_root udevadm trigger --subsystem-match=usb --attr-match=idVendor=054c --action=change || true
    fi
}

# --- user services --------------------------------------------------------------------------------
setup_services() {
    local cfg=$CONFIG_DIR i
    if ! has_systemd; then
        warn "systemd not running: no services set up. Start it from your session's autostart instead, e.g."
        for i in "${INSTANCES[@]}"; do warn "  $PREFIX/bin/marksman-cam --camera $i ${OPTIONS[*]}"; done
        return
    fi
    as_user mkdir -p "$cfg"
    for i in "${INSTANCES[@]}"; do
        if [ ${#OPTIONS[@]} -gt 0 ] || [ ! -e "$cfg/$i.conf" ]; then
            printf '# marksman-cam options for marksman-cam@%s, e.g. OPTIONS="--fill --mirror -s 1920x1080"\n# then: systemctl --user restart marksman-cam@%s\nOPTIONS="%s"\n' \
                "$i" "$i" "${OPTIONS[*]}" | as_user tee "$cfg/$i.conf" >/dev/null
        fi
    done
    if ! as_user systemctl --user show-environment >/dev/null 2>&1; then
        warn "no systemd user session for $TARGET_USER right now; after logging in run:"
        warn "  systemctl --user enable --now $(printf 'marksman-cam@%s ' "${INSTANCES[@]}")"
        return
    fi
    say "Enabling user services: $(printf 'marksman-cam@%s ' "${INSTANCES[@]}")"
    user_systemctl daemon-reload
    for i in canon sony any; do # drop instances from an earlier install with a different --camera
        [[ " ${INSTANCES[*]} " == *" $i "* ]] || user_systemctl disable --now "marksman-cam@$i.service" 2>/dev/null || true
    done
    for i in "${INSTANCES[@]}"; do
        user_systemctl enable "marksman-cam@$i.service" >/dev/null 2>&1
        user_systemctl restart "marksman-cam@$i.service"
    done
}

summary() {
    local i
    echo
    say "Installed."
    cat <<EOF
  Camera prep: auto power-off off. Canon: a shooting mode (Movie mode is best).
               Sony: MENU > Setup > USB Connection: PC Remote.
  Then plug the camera in and pick one of these in any app (browser, Zoom, OBS, ffplay):
EOF
    for i in "${LABELS[@]}"; do [ "$i" = "OBS Virtual Camera" ] || echo "    - $i"; done
    cat <<EOF
  The camera is only woken while an app is using it; until then apps see a grey frame.

  Check the camera on its own:  marksman-cam --camera ${INSTANCES[0]} --snapshot test.jpg
                                (stop the service first: systemctl --user stop marksman-cam@${INSTANCES[0]})
  Logs:                         journalctl --user -u marksman-cam@${INSTANCES[0]} -f
  Options:                      ~/.config/marksman-cam/<camera>.conf, then systemctl --user restart marksman-cam@<camera>
  If the camera was plugged in during install, unplug and replug it once.
EOF
}

# =================================================================================================
if [ "$UNINSTALL" = 1 ]; then
    uninstall
    exit 0
fi
if [ "$DEPS" = 1 ]; then install_deps; fi
check_build_tools
build_and_install
setup_loopback
setup_udev
if [ "$SERVICES" = 1 ]; then setup_services; fi
summary
