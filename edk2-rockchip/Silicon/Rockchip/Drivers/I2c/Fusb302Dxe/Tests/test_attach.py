#!/usr/bin/env python3
"""Compile the production attach routine with scripted CC/VBUS/I2C results.

Run with Python 3 and a host C compiler (CC, default cc). Optional
--baseline-ref <git-ref> first verifies that the supplied baseline fails the
sink-only regression, then runs the same cases against the working source.

SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile


def extract_routine(text):
    """Keep the complete production function body, without a copied variant."""
    start = text.index("STATIC\nEFI_STATUS\nFusb302DetectOrientation (")
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

prelude = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define STATIC static
#define IN
#define OUT
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_NOT_READY 1
#define EFI_DEVICE_ERROR 2
#define EFI_ERROR(s) ((s) != EFI_SUCCESS)
#define FUSB302_SINK_PROBE_RETRY_US 1000000
#define FUSB302_SINK_PROBE_INTERVAL_US 50000
typedef int EFI_STATUS;
typedef int BOOLEAN;
typedef size_t UINTN;
typedef uint32_t UINT32;
typedef enum { UsbTypeCOrientationNone, UsbTypeCOrientationNormal, UsbTypeCOrientationFlipped } USB_TYPE_C_ORIENTATION;
typedef enum { Fusb302RoleSink, Fusb302RoleSource } FUSB302_PROBE_ROLE;
typedef struct { UINT32 SourcePdos[7]; void (*SetVbus)(void); } PLATFORM;
typedef struct { PLATFORM *Platform; BOOLEAN PartnerIsSource; } FUSB302_CONTEXT;
static unsigned SinkCalls, SourceCalls, RestoreCalls, Delays;
static unsigned SinkWaitCalls;
static EFI_STATUS SinkStatus, SourceStatus, RestoreStatus;
static BOOLEAN Vbus;
static void SetVbus(void) {}
static void Reset(void) {
  SinkCalls = SourceCalls = RestoreCalls = Delays = SinkWaitCalls = 0;
  SinkStatus = SourceStatus = EFI_NOT_READY;
  RestoreStatus = EFI_SUCCESS;
  Vbus = FALSE;
}
UINTN Fusb302PdoCount(const UINT32 *Pdos) { return Pdos[0] != 0; }
static EFI_STATUS Fusb302ProbeRole(FUSB302_CONTEXT *Context, FUSB302_PROBE_ROLE Role, USB_TYPE_C_ORIENTATION *Orientation) {
  EFI_STATUS Status;
  (void)Context;
  if (Role == Fusb302RoleSink) {
    SinkCalls++;
    Status = SinkCalls <= SinkWaitCalls ? EFI_NOT_READY : SinkStatus;
  } else {
    SourceCalls++;
    Status = SourceStatus;
  }
  if (Status == EFI_SUCCESS) *Orientation = Role == Fusb302RoleSink ? UsbTypeCOrientationNormal : UsbTypeCOrientationFlipped;
  return Status;
}
static EFI_STATUS Fusb302SetTermination(FUSB302_CONTEXT *Context, FUSB302_PROBE_ROLE Role) {
  (void)Context;
  assert(Role == Fusb302RoleSink);
  RestoreCalls++;
  return RestoreStatus;
}
static BOOLEAN Fusb302SourceVbusPresent(FUSB302_CONTEXT *Context) { (void)Context; return Vbus; }
static void MicroSecondDelay(UINTN Delay) { assert(Delay == FUSB302_SINK_PROBE_INTERVAL_US); Delays++; }
'''
tests = r'''
int main(void) {
  PLATFORM Platform = { { 0 }, SetVbus };
  FUSB302_CONTEXT Context = { &Platform, FALSE };
  USB_TYPE_C_ORIENTATION Orientation;
  EFI_STATUS Status;

  Reset();
  Status = Fusb302DetectOrientation(&Context, &Orientation);
  assert(Status == EFI_NOT_READY && SourceCalls == 0 && RestoreCalls == 1);

  Reset(); Platform.SourcePdos[0] = 1; Platform.SetVbus = NULL;
  Status = Fusb302DetectOrientation(&Context, &Orientation);
  assert(Status == EFI_NOT_READY && SourceCalls == 0 && RestoreCalls == 1);

  Reset(); Platform.SetVbus = SetVbus; SourceStatus = EFI_SUCCESS;
  Status = Fusb302DetectOrientation(&Context, &Orientation);
  assert(Status == EFI_SUCCESS && SourceCalls == 1 && !Context.PartnerIsSource && Orientation == UsbTypeCOrientationFlipped);

  Reset(); SinkStatus = EFI_SUCCESS;
  Status = Fusb302DetectOrientation(&Context, &Orientation);
  assert(Status == EFI_SUCCESS && SourceCalls == 0 && Context.PartnerIsSource && Orientation == UsbTypeCOrientationNormal);

  Reset(); SinkStatus = EFI_SUCCESS; SinkWaitCalls = 2; Vbus = TRUE;
  Status = Fusb302DetectOrientation(&Context, &Orientation);
  assert(Status == EFI_SUCCESS && SinkCalls == 3 && Delays == 2 && SourceCalls == 0 && Context.PartnerIsSource);

  Reset(); SinkStatus = EFI_SUCCESS; RestoreStatus = EFI_DEVICE_ERROR;
  Status = Fusb302DetectOrientation(&Context, &Orientation);
  assert(Status == EFI_DEVICE_ERROR && RestoreCalls == 1);

  Reset(); Platform.SourcePdos[0] = 0; Vbus = TRUE;
  Status = Fusb302DetectOrientation(&Context, &Orientation);
  assert(Status == EFI_NOT_READY && SinkCalls == 21 && Delays == 20 && SourceCalls == 0);

  Reset(); SinkStatus = EFI_DEVICE_ERROR;
  Status = Fusb302DetectOrientation(&Context, &Orientation);
  assert(Status == EFI_DEVICE_ERROR && SourceCalls == 0);
  puts("PASS: 8 production attach/orientation cases");
  return 0;
}
'''


def run_cases(source_text, expect_failure=False):
    with tempfile.TemporaryDirectory(prefix="fusb302-attach-") as temp:
        c = Path(temp) / "attach.c"
        binary = Path(temp) / "attach"
        c.write_text(prelude + extract_routine(source_text) + tests)
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
             "-Werror", str(c), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if expect_failure:
            if result.returncode == 0:
                raise RuntimeError("Baseline passed: the expected attach regression was not reproduced")
            print("PASS: baseline rejected by production attach regression")
        else:
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            print(result.stdout, end="")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", help="Git ref expected to fail the sink-only regression")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "Fusb302Dxe.c"
    if args.baseline_ref:
        root = Path(subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], cwd=source.parent,
            text=True).strip())
        baseline = subprocess.check_output(
            ["git", "show", f"{args.baseline_ref}:{source.relative_to(root).as_posix()}"],
            cwd=root, text=True)
        run_cases(baseline, expect_failure=True)
    run_cases(source.read_text())


if __name__ == "__main__":
    main()
