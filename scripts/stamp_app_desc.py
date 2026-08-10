"""PlatformIO post-build script: write the real version into esp_app_desc.

esp_app_desc lives in libesp_app_format.a, and that archive is PREBUILT: the
platform compiles it once, when a custom_sdkconfig change forces an SDK
rebuild, and links the same object into every build afterwards. Its version
field is therefore whatever `git describe` said on the day of that SDK
rebuild -- v1.5.45 and v1.5.45-rc1 both shipped claiming to be
"v1.5.0-rc-3-239-gbd47effb-dirty", naming a commit neither was built from, on a
tree that was dirty at the time. Deleting .pio/build does not regenerate it.

Nothing in CrossInk reads that field -- OtaUpdater compares CROSSINK_VERSION,
and so does everything that displays a version -- so this is about being able
to ask a binary, or a device dump, which build it is. A field that answers
confidently and wrongly is worse than one that is empty.

So the field is rewritten in the finished image, with the same string the
firmware reports. The version slot is fixed-size, so nothing moves; only the
two integrity fields covering it have to be recomputed:

  - the one-byte XOR checksum over all segment DATA, seeded 0xEF, sitting just
    before the 16-byte boundary at the end of the segment area;
  - the appended SHA-256 over everything up to and including that checksum,
    present when byte 23 of the header is 1.

Get either wrong and the device rejects the image at update time with "Invalid
firmware" rather than misbehaving, which is the failure mode to want. The
esptool check in verify_image_stamp below is what proves it, and the build
fails rather than emit an image whose integrity we have not re-verified.
"""

import hashlib
import os
import re
import struct
import subprocess
import sys
import time

IMAGE_MAGIC = 0xE9
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
VERSION_OFFSET = APP_DESC_OFFSET + 0x10
VERSION_SIZE = 32
TIME_OFFSET = APP_DESC_OFFSET + 0x50
TIME_SIZE = 16
DATE_OFFSET = APP_DESC_OFFSET + 0x60
DATE_SIZE = 16
CHECKSUM_SEED = 0xEF

GENERATED_HEADER = 'lib/AppVersion/AppVersionGenerated.h'


def _read_generated_version(project_dir):
    """The string the firmware itself reports, not a second one derived here."""
    path = os.path.join(project_dir, GENERATED_HEADER)
    try:
        with open(path, 'r', encoding='utf-8') as handle:
            match = re.search(r'#define\s+CROSSINK_VERSION_GENERATED\s+"([^"]*)"', handle.read())
    except OSError as exc:
        print(f'stamp_app_desc: cannot read {GENERATED_HEADER}: {exc}', file=sys.stderr)
        return None
    return match.group(1) if match else None


def _segment_area_end(data):
    """Offset just past the last segment's data."""
    segment_count = data[1]
    offset = 24
    for _ in range(segment_count):
        _, length = struct.unpack('<II', data[offset:offset + 8])
        offset += 8 + length
    return offset


def stamp(image_path, version):
    with open(image_path, 'rb') as handle:
        data = bytearray(handle.read())

    if not data or data[0] != IMAGE_MAGIC:
        print('stamp_app_desc: not an ESP image, leaving it alone', file=sys.stderr)
        return False
    magic, = struct.unpack('<I', data[APP_DESC_OFFSET:APP_DESC_OFFSET + 4])
    if magic != APP_DESC_MAGIC:
        print(f'stamp_app_desc: no app descriptor at {APP_DESC_OFFSET:#x}, leaving it alone', file=sys.stderr)
        return False

    encoded = version.encode('utf-8')[:VERSION_SIZE - 1]
    data[VERSION_OFFSET:VERSION_OFFSET + VERSION_SIZE] = encoded.ljust(VERSION_SIZE, b'\0')

    # The compile time comes from the same prebuilt object and is frozen just as
    # hard: left alone, every future release would claim to have been built on
    # the afternoon that SDK was rebuilt. Stamped in the shape __DATE__ and
    # __TIME__ produce, so anything reading the field sees what it expects.
    # The cost is that two builds of one commit no longer match byte for byte,
    # which nothing here depends on -- the ELF SHA-256 a few fields along
    # already moves on its own.
    now = time.localtime()
    stamped_time = time.strftime('%H:%M:%S', now).encode('ascii')
    stamped_date = '{} {:2d} {}'.format(
        time.strftime('%b', now), now.tm_mday, now.tm_year).encode('ascii')
    data[TIME_OFFSET:TIME_OFFSET + TIME_SIZE] = stamped_time.ljust(TIME_SIZE, b'\0')
    data[DATE_OFFSET:DATE_OFFSET + DATE_SIZE] = stamped_date.ljust(DATE_SIZE, b'\0')

    # Checksum byte: the segment area is zero-padded so the byte lands on the
    # last byte of a 16-byte block -- the FIRST such position at or after the
    # end of the segments. Rounding up to the next block instead puts it 16
    # bytes too far, inside the appended hash, and leaves the real checksum
    # byte holding its pre-stamp value. esptool caught exactly that.
    end = _segment_area_end(data)
    checksum_offset = end + ((15 - end % 16) % 16)
    checksum = CHECKSUM_SEED
    offset = 24
    for _ in range(data[1]):
        _, length = struct.unpack('<II', data[offset:offset + 8])
        for byte in data[offset + 8:offset + 8 + length]:
            checksum ^= byte
        offset += 8 + length
    data[checksum_offset] = checksum

    if data[23] == 1:
        digest = hashlib.sha256(bytes(data[:checksum_offset + 1])).digest()
        data[checksum_offset + 1:checksum_offset + 33] = digest

    with open(image_path, 'wb') as handle:
        handle.write(bytes(data))
    return True


def verify_image_stamp(env, image_path, version):
    """Let esptool re-check what we rewrote. Trusting our own arithmetic here is
    how a device ends up refusing an update in the field."""
    esptool = os.path.join(env.PioPlatform().get_package_dir('tool-esptoolpy') or '', 'esptool.py')
    if not os.path.isfile(esptool):
        sys.stderr.write('stamp_app_desc: esptool not found, cannot verify the stamped image\n')
        env.Exit(1)
        return
    chip = env.BoardConfig().get('build.mcu', 'esp32')
    # esptool 5 renamed image_info to image-info and dropped --version; try the
    # current spelling first and fall back so an older tool still verifies.
    for command in (['image-info', image_path], ['image_info', '--version', '2', image_path]):
        result = subprocess.run(
            [env['PYTHONEXE'], esptool, '--chip', chip, *command],
            capture_output=True, text=True,
        )
        out = result.stdout + result.stderr
        if 'No such option' not in out and 'No such command' not in out:
            break
    if result.returncode != 0 or 'invalid' in out.lower() or version not in out:
        sys.stderr.write('stamp_app_desc: esptool rejected the stamped image\n' + out)
        env.Exit(1)
        return
    print(f'App descriptor stamped and verified: {version}')


def stamp_app_desc(source, target, env):
    version = _read_generated_version(env['PROJECT_DIR'])
    if not version:
        sys.stderr.write('stamp_app_desc: no generated version to stamp\n')
        env.Exit(1)
        return
    image_path = str(target[0])
    if stamp(image_path, version):
        verify_image_stamp(env, image_path, version)


try:
    Import('env')                                           # noqa: F821  # type: ignore[name-defined]
    env.AddPostAction(                                      # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.bin',
        stamp_app_desc,
    )
except NameError:
    print('stamp_app_desc.py: must be run via PlatformIO', file=sys.stderr)
