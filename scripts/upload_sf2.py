# pio run -t uploadsf2
#   platformio.ini の custom_sf2_file をフラッシュの SF2 領域へ書き込む
#     ESP32:  sf2 パーティション (esptool)
#     RP2350: filesystem 領域 (board_build.filesystem_size, picotool)
# また ESP32 で custom_usb_touch = yes のとき、upload / uploadsf2 の前に
# TinyUSB CDC を 1200bps touch してダウンロードモードへ入れる
import csv
import os
import re
import subprocess
import time

import serial
from platformio.util import get_serial_ports

Import("env")  # noqa: F821

SF2_PARTITION = "sf2"


def _sf2_partition():
    path = os.path.join(env.subst("$PROJECT_DIR"), env.GetProjectOption("board_build.partitions"))
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].strip().startswith("#"):
                continue
            cols = [c.strip() for c in row]
            if cols[0] == SF2_PARTITION:
                return int(cols[3], 0), int(cols[4], 0)
    raise SystemExit(f"partition '{SF2_PARTITION}' not found in {path}")


def enter_download_mode(target, source, env):
    """Reboot a TinyUSB CDC device into the ROM download mode (1200bps touch)."""
    if env.GetProjectOption("custom_usb_touch", "no") != "yes":
        return
    if not env.subst("$UPLOAD_PORT"):
        env.AutodetectUploadPort()
    port = env.subst("$UPLOAD_PORT")
    before = {p["port"] for p in get_serial_ports()}
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 1200
        s.open()
        s.dtr = False  # the CDC reboots when DTR drops at 1200bps
        time.sleep(0.1)
        s.close()
    except serial.SerialException:
        pass
    # The ROM USB-Serial-JTAG port shows up under a different name.
    for _ in range(16):
        time.sleep(0.25)
        now = {p["port"] for p in get_serial_ports()}
        new = now - before
        if new:
            env.Replace(UPLOAD_PORT=sorted(new)[0])
            print(f"Download mode port: {env.subst('$UPLOAD_PORT')}")
            return
        before &= now
    print("No new port appeared; assuming the device is already in download mode")


def _parse_size(text):
    m = re.fullmatch(r"([0-9]*\.?[0-9]+)([kKmM]?)[bB]?", text.strip())
    if not m:
        raise SystemExit(f"cannot parse size '{text}'")
    return int(float(m.group(1)) * {"": 1, "k": 1024, "m": 1024 * 1024}[m.group(2).lower()])


def _rp_sf2_area():
    """Filesystem area as laid out by platform-raspberrypi (flash end - 4KB EEPROM - filesystem)."""
    board = env.BoardConfig()
    flash = int(board.get("upload.maximum_size"))
    fs = _parse_size(str(board.get("build.filesystem_size", "0")))
    if fs <= 0:
        raise SystemExit("board_build.filesystem_size is not set")
    return 0x10000000 + flash - 4096 - fs, fs


def _picotool():
    return os.path.join(env.PioPlatform().get_package_dir("tool-picotool-rp2040-earlephilhower") or "", "picotool")


def _bootsel_devices(picotool):
    out = subprocess.run([picotool, "info", "-d"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    return out.count(b"type:")


# Recognised by src/midi_in.cpp: reboot into the bootloader
BOOT_SYSEX = bytes([0xF0, 0x7D]) + b"BOOT" + bytes([0xF7])


def _wait_bootsel(picotool, seconds):
    for _ in range(int(seconds * 4)):
        time.sleep(0.25)
        if _bootsel_devices(picotool):
            return True
    return False


def enter_bootsel(env):
    """Reboot the running firmware into BOOTSEL mode (1200bps touch, then a SysEx over USB serial)."""
    picotool = _picotool()
    if _bootsel_devices(picotool):
        return
    if not env.subst("$UPLOAD_PORT"):
        env.AutodetectUploadPort()
    port = env.subst("$UPLOAD_PORT")
    try:
        s = serial.Serial(port, 1200)
        s.dtr = False
        time.sleep(0.1)
        s.close()
    except serial.SerialException:
        pass
    if _wait_bootsel(picotool, 3):
        return
    try:
        s = serial.Serial(port, 115200, write_timeout=1)
        s.write(BOOT_SYSEX)
        s.flush()
        s.close()
    except serial.SerialException:
        pass
    if _wait_bootsel(picotool, 5):
        return
    print("No RP2350 in BOOTSEL mode found; hold BOOT while pressing RESET and retry")


def _check_sf2(sf2, size):
    if not os.path.isfile(sf2):
        raise SystemExit(f"SoundFont not found: {sf2}")
    length = os.path.getsize(sf2)
    if length > size:
        raise SystemExit(f"{sf2} ({length} bytes) exceeds the SF2 area ({size} bytes)")
    with open(sf2, "rb") as f:
        head = f.read(12)
    if head[0:4] != b"RIFF" or head[8:12] != b"sfbk":
        raise SystemExit(f"{sf2} is not a SoundFont 2 file")
    return length


def upload_sf2_rp(target, source, env):
    sf2 = os.path.join(env.subst("$PROJECT_DIR"), env.GetProjectOption("custom_sf2_file"))
    offset, size = _rp_sf2_area()
    length = _check_sf2(sf2, size)
    enter_bootsel(env)
    print(f"Writing {sf2} ({length} bytes) to 0x{offset:x}")
    picotool = _picotool()
    # -u skips sectors that already hold the same data
    cmd = f'"{picotool}" load -u -v -t bin "{sf2}" -o 0x{offset:x} && "{picotool}" reboot'
    return env.Execute(env.VerboseAction(cmd, "Uploading SoundFont"))


def upload_sf2(target, source, env):
    if env.get("PIOPLATFORM") == "raspberrypi":
        return upload_sf2_rp(target, source, env)
    sf2 = os.path.join(env.subst("$PROJECT_DIR"), env.GetProjectOption("custom_sf2_file"))
    offset, size = _sf2_partition()
    length = _check_sf2(sf2, size)

    enter_download_mode(target, source, env)
    after = env.BoardConfig().get("upload.after_reset", "hard-reset")
    cmd = (
        '$UPLOADER --chip esp32s3 --port "$UPLOAD_PORT" --baud $UPLOAD_SPEED '
        f"--before default-reset --after {after} write-flash -z "
        f'0x{offset:x} "{sf2}"'
    )
    print(f"Writing {sf2} ({length} bytes) to 0x{offset:x}")
    return env.Execute(env.VerboseAction(cmd, "Uploading SoundFont"))


if env.get("PIOPLATFORM") != "raspberrypi":
    env.AddPreAction("upload", enter_download_mode)

env.AddCustomTarget(
    name="uploadsf2",
    dependencies=None,
    actions=[upload_sf2],
    title="Upload SoundFont",
    description="Write custom_sf2_file to the SF2 flash area",
)
