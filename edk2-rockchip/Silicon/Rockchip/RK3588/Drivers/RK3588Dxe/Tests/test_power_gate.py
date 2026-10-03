#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production SATA notification callback with mock boot services.

Run: python3 path/to/Tests/test_power_gate.py
Requires a host C compiler (CC, or cc). No EDK2 checkout dependencies are needed.
To demonstrate the regression, --revision e7de1c2d extracts the original callback
from Git and must fail. This tests callback sequencing, not physical rail timing.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


PREAMBLE = r"""
#include <stddef.h>
#include <stdio.h>
#define STATIC static
#define VOID void
#define EFIAPI
#define IN
#define EFI_ERROR(Status) ((Status) != 0)
#define DEBUG(Arguments) ((void)0)
typedef int EFI_STATUS;
typedef void *EFI_EVENT;
static int gRockchipPlatformPowerReadyProtocolGuid;
static int Ready, Closes, Installs;
static EFI_STATUS Locate(void *Guid, void *Registration, void **Interface) {
  if (Guid != &gRockchipPlatformPowerReadyProtocolGuid || Registration != NULL ||
      Interface == NULL) {
    return 1;
  }
  /* PowerReady is a marker protocol with a NULL interface. */
  *Interface = NULL;
  return Ready ? 0 : 1;
}
static EFI_STATUS Close(EFI_EVENT Event) {
  (void)Event;
  Closes++;
  return 0;
}
static void InstallSataDevices(void) { Installs++; }
static struct {
  EFI_STATUS (*LocateProtocol)(void *, void *, void **);
  EFI_STATUS (*CloseEvent)(EFI_EVENT);
} Services = {Locate, Close}, *gBS = &Services;
"""

HARNESS = r"""
int main(void) {
  /* EfiCreateProtocolNotifyEvent signals even if no protocol is installed. */
  OnPlatformPowerReady(NULL, NULL);
  if (Closes != 0 || Installs != 0) {
    fputs("FAIL: empty initial notification enabled SATA or closed its event\n", stderr);
    return 1;
  }
  /* Installation must then release the still-armed event. */
  Ready = 1;
  OnPlatformPowerReady(NULL, NULL);
  if (Closes != 1 || Installs != 1) {
    fputs("FAIL: protocol installation did not close event and enable SATA\n", stderr);
    return 1;
  }
  /* A separate event whose initial notification finds PowerReady installed. */
  Closes = 0;
  Installs = 0;
  OnPlatformPowerReady(NULL, NULL);
  if (Closes != 1 || Installs != 1) {
    fputs("FAIL: already-present protocol did not enable SATA\n", stderr);
    return 1;
  }
  puts("PASS: SATA waits for PowerReady, including the initial empty notification");
  return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="extract callback from a Git revision")
    args = parser.parse_args()
    source_path = Path(__file__).resolve().parents[1] / "RK3588Dxe.c"
    if args.revision:
        root = Path(subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], cwd=source_path.parent,
            text=True).strip())
        source = subprocess.check_output(
            ["git", "show", f"{args.revision}:{source_path.relative_to(root)}"],
            cwd=root, text=True)
    else:
        source = source_path.read_text()

    start = source.index("STATIC\nVOID\nEFIAPI\nOnPlatformPowerReady (")
    end = source.index("\nSTATIC\nVOID\nEFIAPI\nAfterApplyVariablesInit", start)
    with tempfile.TemporaryDirectory(prefix="rk3588-power-gate-") as directory:
        directory = Path(directory)
        test_source = directory / "test.c"
        executable = directory / "test"
        test_source.write_text(PREAMBLE + source[start:end] + HARNESS)
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(compiler + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            str(test_source), "-o", str(executable)], check=True)
        return subprocess.run([str(executable)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
