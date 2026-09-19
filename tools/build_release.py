#!/usr/bin/env python3
"""Build shareable firmware in a clean directory without local credentials."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
version = re.search(r'FW_VERSION "([^"]+)"', (ROOT / "include/version.h").read_text()).group(1)
output = ROOT / "dist" / version
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="smartremote-cyd-public-") as temporary:
    clean = Path(temporary)
    for directory in ("src", "include", "lib"):
        if (ROOT / directory).exists():
            shutil.copytree(ROOT / directory, clean / directory,
                            ignore=shutil.ignore_patterns("secrets.h", "tv_config.h", "__pycache__"))
    shutil.copy2(ROOT / "platformio.ini", clean / "platformio.ini")
    assert not (clean / "include/secrets.h").exists()
    assert not (clean / "include/tv_config.h").exists()
    subprocess.run(["pio", "run", "-d", str(clean)], check=True)
    build = clean / ".pio/build/cyd"
    firmware = output / f"smart-remote-plus-cyd-{version}-ota.bin"
    shutil.copy2(build / "firmware.bin", firmware)
    info = json.loads(subprocess.check_output(["pio", "system", "info", "--json-output"]))
    core = Path(info["core_dir"]["value"])
    esptool = core / "packages/tool-esptoolpy/esptool.py"
    python = Path(info["python_exe"]["value"])
    boot_app = core / "packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
    subprocess.run([str(python), str(esptool), "--chip", "esp32", "merge_bin", "-o",
                    str(output / f"smart-remote-plus-cyd-{version}-factory.bin"),
                    "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB",
                    "0x1000", str(build / "bootloader.bin"), "0x8000", str(build / "partitions.bin"),
                    "0xe000", str(boot_app), "0x10000", str(build / "firmware.bin")], check=True)
(output / "SHA256SUMS").write_text("".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n" for p in sorted(output.glob("*.bin"))))
print(f"Credential-free release artifacts: {output}")
