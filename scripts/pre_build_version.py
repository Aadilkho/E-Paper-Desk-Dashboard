"""
PlatformIO pre-build script: generates src/firmware_version.h with the
current git commit SHA so the firmware can compare itself against the
SHA stored in version.json when checking for OTA updates.

Priority:
  1. GIT_SHA environment variable (set by GitHub Actions CI)
  2. `git rev-parse --short=12 HEAD` from the local working tree
  3. Falls back to "unknown" if git is unavailable
"""

import subprocess
import os

Import("env")  # noqa: F821 - injected by PlatformIO SCons


def get_firmware_sha():
    # CI sets this to the exact commit SHA that triggered the build
    sha = os.environ.get("GIT_SHA", "").strip()
    if sha:
        return sha[:12]

    # Local build: ask git directly
    try:
        project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
        result = subprocess.run(
            ["git", "rev-parse", "--short=12", "HEAD"],
            capture_output=True,
            text=True,
            cwd=project_dir,
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception as exc:
        print(f"[pre_build_version] git error: {exc}")

    return "unknown"


sha = get_firmware_sha()
project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
header_path = os.path.join(project_dir, "src", "firmware_version.h")

with open(header_path, "w") as f:
    f.write("#pragma once\n")
    f.write(f'#define FIRMWARE_SHA "{sha}"\n')

print(f"[pre_build_version] FIRMWARE_SHA = {sha}")
