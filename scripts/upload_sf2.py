# pio run -t uploadsf2
#   platformio.ini の custom_sf2_file を sf2 パーティションへ書き込む
# また custom_usb_touch = yes のとき、upload / uploadsf2 の前に
# TinyUSB CDC を 1200bps touch してダウンロードモードへ入れる
import csv
import os
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


def upload_sf2(target, source, env):
    sf2 = os.path.join(env.subst("$PROJECT_DIR"), env.GetProjectOption("custom_sf2_file"))
    if not os.path.isfile(sf2):
        raise SystemExit(f"SoundFont not found: {sf2}")
    offset, size = _sf2_partition()
    length = os.path.getsize(sf2)
    if length > size:
        raise SystemExit(f"{sf2} ({length} bytes) exceeds sf2 partition ({size} bytes)")
    with open(sf2, "rb") as f:
        head = f.read(12)
    if head[0:4] != b"RIFF" or head[8:12] != b"sfbk":
        raise SystemExit(f"{sf2} is not a SoundFont 2 file")

    enter_download_mode(target, source, env)
    after = env.BoardConfig().get("upload.after_reset", "hard-reset")
    cmd = (
        '$UPLOADER --chip esp32s3 --port "$UPLOAD_PORT" --baud $UPLOAD_SPEED '
        f"--before default-reset --after {after} write-flash -z "
        f'0x{offset:x} "{sf2}"'
    )
    print(f"Writing {sf2} ({length} bytes) to 0x{offset:x}")
    return env.Execute(env.VerboseAction(cmd, "Uploading SoundFont"))


env.AddPreAction("upload", enter_download_mode)

env.AddCustomTarget(
    name="uploadsf2",
    dependencies=None,
    actions=[upload_sf2],
    title="Upload SoundFont",
    description="Write custom_sf2_file to the sf2 partition",
)
