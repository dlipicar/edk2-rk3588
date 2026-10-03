/** @file
  Host regression tests using the production PD implementation and EDK2 types.
  The register/FIFO and clock mocks model messages, not the analog PHY.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include PD_SOURCE_FILE

#define FIXED(Mv, Ma)  ((((UINT32)(Mv) / 50) << 10) | ((Ma) / 10))
#define VARIABLE(Min, Max, Ma)  ((2U << 30) | (((Max) / 50) << 20) | (((Min) / 50) << 10) | ((Ma) / 10))
#define CHECK(Expression) do { if (!(Expression)) { \
  fprintf (stderr, "%s:%u: %s\n", __func__, __LINE__, #Expression); return 0; \
} } while (0)

typedef enum {
  Normal, RejectFirst, WaitOnce, WaitForever, SoftOnRequest, SoftAfterAccept,
  HardOnRequest, HardOnSoftTransmit, RepeatedCaps, QuietIoError, NoPd, NoPsReady,
  WaitSoftReset, WaitNewCaps, WaitNewCapsForever, WarmReboot, LateSource
} TEST_MODE;

STATIC UINT8 mRegisters[256], mRx[4096], mTx[64];
STATIC UINTN mRxHead, mRxTail, mTxLength, mRequests, mHardSent, mSinkCaps;
STATIC UINTN mFifoReads, mFailFifoRead, mSoftSent, mFifoWrites, mFailFifoWrite;
STATIC UINT8 mSourceId;
STATIC UINT64 mNow, mRequestTimes[32];
STATIC UINT32 mRequestedObjects[32];
STATIC BOOLEAN mInitialCaps, mPendingHardReset, mCapsAfterReset;
STATIC TEST_MODE mMode;
STATIC FUSB302_PLATFORM_DEVICE_PROTOCOL mPlatform;
STATIC FUSB302_CONTEXT mContext;

UINT64 EFIAPI GetPerformanceCounter (VOID) { return mNow; }
UINT64 EFIAPI GetTimeInNanoSecond (UINT64 Ticks) { return Ticks; }
UINTN EFIAPI MicroSecondDelay (UINTN Us) { mNow += Us * 1000; return Us; }
UINT64 EFIAPI MultU64x32 (UINT64 A, UINT32 B) { return A * B; }
VOID *EFIAPI CopyMem (VOID *To, CONST VOID *From, UINTN Size) { return memcpy (To, From, Size); }
VOID *EFIAPI ZeroMem (VOID *To, UINTN Size) { return memset (To, 0, Size); }
BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { (void)Level; return TRUE; }
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) { (void)Level; (void)Format; }
VOID EFIAPI DebugAssert (CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Description) {
  fprintf (stderr, "ASSERT %s:%lu: %s\n", File, (unsigned long)Line, Description); abort ();
}
UINTN Fusb302PdoCount (CONST UINT32 *Pdos) {
  UINTN Count;
  for (Count = 0; Count < FUSB302_MAX_PDOS && Pdos[Count] != 0; Count++) {}
  return Count;
}
UINT32 Fusb302SourcePdo (CONST FUSB302_CONTEXT *Context) { (void)Context; return 0; }

STATIC VOID QueueMessage (UINT16 Header, CONST UINT32 *Objects) {
  UINTN Index, Byte, Count = PD_HEADER_COUNT (Header);
  if (mRxTail + 7 + 4 * Count >= sizeof (mRx)) { abort (); }
  mRx[mRxTail++] = FUSB302_RX_TOKEN_SOP;
  mRx[mRxTail++] = (UINT8)Header;
  mRx[mRxTail++] = (UINT8)(Header >> 8);
  for (Index = 0; Index < Count; Index++) {
    for (Byte = 0; Byte < 4; Byte++) {
      mRx[mRxTail++] = (UINT8)(Objects[Index] >> (8 * Byte));
    }
  }
  for (Byte = 0; Byte < 4; Byte++) { mRx[mRxTail++] = 0; }
}
STATIC VOID QueueControl (UINT8 Type) {
  QueueMessage (PD_HEADER_BUILD (Type, mSourceId++ & 7, 0, 1, 1, PD_REV_2_0), NULL);
}
STATIC VOID QueueCaps (VOID) {
  CONST UINT32 Objects[] = { FIXED (5000, 3000), FIXED (9000, 3000), FIXED (20000, 1000) };
  QueueMessage (PD_HEADER_BUILD (PD_DATA_SOURCE_CAP, mSourceId++ & 7, 3, 1, 1, PD_REV_2_0), Objects);
}
STATIC VOID OnTransmit (VOID) {
  UINT16 Header = mTx[5] | ((UINT16)mTx[6] << 8);
  UINT8 Type = PD_HEADER_TYPE (Header);
  UINTN Count = PD_HEADER_COUNT (Header);
  mRegisters[FUSB302_REG_INTERRUPTA] |= FUSB302_INTERRUPTA_TX_SUCCESS;
  if (Count == 1 && Type == PD_DATA_REQUEST) {
    if (mRequests >= 32) { abort (); }
    mRequestedObjects[mRequests] = mTx[7] | ((UINT32)mTx[8] << 8) |
      ((UINT32)mTx[9] << 16) | ((UINT32)mTx[10] << 24);
    mRequestTimes[mRequests++] = mNow;
    if (mMode == RejectFirst && mRequests == 1) { QueueControl (PD_CTRL_REJECT); return; }
    if ((mMode == WaitOnce && mRequests == 1) || mMode == WaitForever) {
      QueueControl (PD_CTRL_WAIT); return;
    }
    if ((mMode == WaitSoftReset || mMode == WaitNewCaps || mMode == WaitNewCapsForever) &&
        (mRequests == 1 || mMode == WaitNewCapsForever)) {
      QueueControl (PD_CTRL_WAIT);
      if (mMode == WaitSoftReset) { mSourceId = 0; QueueControl (PD_CTRL_SOFT_RESET); }
      else { QueueCaps (); }
      return;
    }
    if (mMode == SoftOnRequest && mRequests == 1) {
      mSourceId = 0; QueueControl (PD_CTRL_SOFT_RESET); return;
    }
    if (mMode == HardOnRequest && mRequests == 1) {
      mPendingHardReset = TRUE; mCapsAfterReset = TRUE; mSourceId = 0; return;
    }
    QueueControl (PD_CTRL_ACCEPT);
    if (mMode == SoftAfterAccept && mRequests == 1) {
      mSourceId = 0; QueueControl (PD_CTRL_SOFT_RESET); return;
    }
    if (mMode != NoPsReady) { QueueControl (PD_CTRL_PS_RDY); }
    if (mMode == RepeatedCaps) { QueueCaps (); }
  } else if (Count == 0 && Type == PD_CTRL_ACCEPT &&
             (mMode == SoftOnRequest || mMode == SoftAfterAccept || mMode == WaitSoftReset)) {
    QueueCaps ();
  } else if (Count == 0 && Type == PD_CTRL_SOFT_RESET) {
    mSoftSent++;
    if (mMode == WarmReboot) { QueueControl (PD_CTRL_ACCEPT); QueueCaps (); }
    if (mMode == LateSource) { mRegisters[FUSB302_REG_INTERRUPTA] = FUSB302_INTERRUPTA_RETRYFAIL; mCapsAfterReset = TRUE; }
    if (mMode == NoPd) { mRegisters[FUSB302_REG_INTERRUPTA] = FUSB302_INTERRUPTA_RETRYFAIL; }
    if (mMode == HardOnSoftTransmit) {
      mPendingHardReset = TRUE; mCapsAfterReset = TRUE; mSourceId = 0;
    }
  } else if (Count > 0 && Type == PD_DATA_SINK_CAP) {
    mSinkCaps++;
  }
}

EFI_STATUS Fusb302RegRead (FUSB302_CONTEXT *Context, UINT8 Register, UINT8 *Value) {
  (void)Context;
  mNow += 10000;
  if (Register == FUSB302_REG_INTERRUPTA) {
    if (mMode == QuietIoError && mRequests != 0 && mRxHead == mRxTail) { return EFI_DEVICE_ERROR; }
    *Value = mRegisters[Register]; mRegisters[Register] = 0;
    if (mPendingHardReset) { *Value |= FUSB302_INTERRUPTA_HARDRESET; mPendingHardReset = FALSE; }
  } else if (Register == FUSB302_REG_STATUS1) {
    if (mInitialCaps || mCapsAfterReset) {
      mInitialCaps = FALSE; mCapsAfterReset = FALSE; QueueCaps ();
    }
    *Value = mRxHead == mRxTail ? FUSB302_STATUS1_RX_EMPTY : 0;
  } else if (Register == FUSB302_REG_FIFOS) {
    if (++mFifoReads == mFailFifoRead) { return EFI_DEVICE_ERROR; }
    if (mRxHead == mRxTail) { return EFI_DEVICE_ERROR; }
    *Value = mRx[mRxHead++];
  } else { *Value = mRegisters[Register]; }
  return EFI_SUCCESS;
}
EFI_STATUS Fusb302RegWrite (FUSB302_CONTEXT *Context, UINT8 Register, UINT8 Value) {
  (void)Context;
  mNow += 10000;
  if (Register == FUSB302_REG_FIFOS) {
    if (++mFifoWrites == mFailFifoWrite) { return EFI_DEVICE_ERROR; }
    if (mTxLength >= sizeof (mTx)) { abort (); }
    mTx[mTxLength++] = Value;
    if (Value == FUSB302_TKN_TXON && mTxLength > 6 && mTxLength == (UINTN)(mTx[4] & 0x1f) + 9) {
      OnTransmit (); mTxLength = 0;
    }
  } else {
    mRegisters[Register] = Value;
    if (Register == FUSB302_REG_CONTROL1 && (Value & FUSB302_CONTROL1_RX_FLUSH)) {
      mRxHead = mRxTail = 0; mRegisters[Register] &= ~FUSB302_CONTROL1_RX_FLUSH;
    }
    if (Register == FUSB302_REG_CONTROL0 && (Value & FUSB302_CONTROL0_TX_FLUSH)) {
      mTxLength = 0; mRegisters[Register] &= ~FUSB302_CONTROL0_TX_FLUSH;
    }
    if (Register == FUSB302_REG_CONTROL3 && (Value & FUSB302_CONTROL3_SEND_HARD_RESET)) { mHardSent++; }
  }
  return EFI_SUCCESS;
}
EFI_STATUS Fusb302RegUpdate (FUSB302_CONTEXT *Context, UINT8 Register, UINT8 Mask, UINT8 Value) {
  UINT8 Old;
  EFI_STATUS Status = Fusb302RegRead (Context, Register, &Old);
  if (EFI_ERROR (Status)) { return Status; }
  return Fusb302RegWrite (Context, Register, (Old & ~Mask) | (Value & Mask));
}
STATIC VOID Reset (TEST_MODE Mode) {
  memset (mRegisters, 0, sizeof (mRegisters));
  memset (&mPlatform, 0, sizeof (mPlatform)); memset (&mContext, 0, sizeof (mContext));
  mRxHead = mRxTail = mTxLength = mRequests = mHardSent = mSinkCaps = mFifoReads = mFailFifoRead = 0;
  mNow = mSourceId = mSoftSent = mFifoWrites = mFailFifoWrite = 0; mPendingHardReset = mCapsAfterReset = FALSE;
  mMode = Mode; mInitialCaps = Mode != NoPd && Mode != HardOnSoftTransmit && Mode != WarmReboot && Mode != LateSource;
  mRegisters[FUSB302_REG_STATUS0] = FUSB302_STATUS0_VBUSOK;
  mPlatform.SinkPdos[0] = FIXED (5000, 3000);
  mPlatform.SinkPdos[1] = VARIABLE (5000, 20000, 3000);
  mPlatform.SelfPowered = FALSE;
  mContext.Platform = &mPlatform; mContext.PartnerIsSource = TRUE;
}
STATIC EFI_STATUS Select (PD_MESSAGE *Caps, UINTN *Position, UINT32 *Mv, UINT32 *Ma) {
#ifdef BASELINE_CODE
  return Fusb302PdSelectPdo (Caps, mPlatform.SinkPdos, Position, Mv, Ma);
#else
  return Fusb302PdSelectPdo (Caps, mPlatform.SinkPdos, 0, Position, Mv, Ma);
#endif
}
STATIC int TestPowerSelection (VOID) {
  PD_MESSAGE Caps = { 0 }; UINTN Position; UINT32 Mv, Ma;
  Reset (Normal); Caps.Header = PD_HEADER_BUILD (PD_DATA_SOURCE_CAP, 0, 3, 1, 1, PD_REV_2_0);
  Caps.Objects[0] = FIXED (5000, 3000); Caps.Objects[1] = FIXED (9000, 3000); Caps.Objects[2] = FIXED (20000, 1000);
  CHECK (Select (&Caps, &Position, &Mv, &Ma) == EFI_SUCCESS && Position == 2 && Mv == 9000 && Ma == 3000);
  Caps.Objects[2] = FIXED (18000, 1500);
  CHECK (Select (&Caps, &Position, &Mv, &Ma) == EFI_SUCCESS && Position == 3);
  Caps.Objects[2] = FIXED (20000, 5000);
  CHECK (Select (&Caps, &Position, &Mv, &Ma) == EFI_SUCCESS && Position == 3 && Ma == 3000);
  return 1;
}
STATIC int TestMalformedPdos (VOID) {
  PD_MESSAGE Caps = { 0 }; UINTN Position; UINT32 Mv, Ma;
  Reset (Normal); Caps.Header = PD_HEADER_BUILD (PD_DATA_SOURCE_CAP, 0, 3, 1, 1, PD_REV_2_0);
  Caps.Objects[0] = FIXED (5000, 3000); Caps.Objects[1] = FIXED (9000, 2000); Caps.Objects[2] = FIXED (20000, 0);
  CHECK (Select (&Caps, &Position, &Mv, &Ma) == EFI_SUCCESS && Position == 2);
  Caps.Objects[2] = FIXED (20000, 5010);
  CHECK (Select (&Caps, &Position, &Mv, &Ma) == EFI_SUCCESS && Position == 2);
  Caps.Objects[2] = FIXED (21000, 3000);
  CHECK (Select (&Caps, &Position, &Mv, &Ma) == EFI_SUCCESS && Position == 2);
  Caps.Objects[2] = 3U << 30;  // PPS/APDO is not a fixed supply.
  CHECK (Select (&Caps, &Position, &Mv, &Ma) == EFI_SUCCESS && Position == 2);
  Caps.Objects[0] = FIXED (9000, 3000);
  CHECK (EFI_ERROR (Select (&Caps, &Position, &Mv, &Ma)));
  return 1;
}
STATIC int RunNegotiation (TEST_MODE Mode) {
  Reset (Mode);
  CHECK (Fusb302PdNegotiateSink (&mContext, UsbTypeCOrientationNormal) == EFI_SUCCESS);
  CHECK (mContext.Contract.PdNegotiated && mHardSent == 0);
  return 1;
}
STATIC int TestRequest (VOID) {
  CHECK (RunNegotiation (Normal));
  CHECK (mRequests == 1 && mContext.Contract.VoltageMv == 9000 && mContext.Contract.CurrentMa == 3000);
  CHECK ((mRequestedObjects[0] & BIT24) != 0);
  return 1;
}
STATIC int TestReject (VOID) {
  CHECK (RunNegotiation (RejectFirst));
  CHECK (mRequests == 2 && PD_RDO_OBJECT_POSITION (mRequestedObjects[0]) == 2 &&
         PD_RDO_OBJECT_POSITION (mRequestedObjects[1]) == 3 && mContext.Contract.VoltageMv == 20000);
  return 1;
}
STATIC int TestWait (VOID) {
  CHECK (RunNegotiation (WaitOnce));
  CHECK (mRequests == 2 && mRequestTimes[1] - mRequestTimes[0] >= 100000000);
  Reset (WaitForever);
  CHECK (EFI_ERROR (Fusb302PdNegotiateSink (&mContext, UsbTypeCOrientationNormal)));
  CHECK (mRequests == 3 && !mContext.Contract.PdNegotiated && mContext.Contract.VoltageMv == 0 && mHardSent == 0);
  return 1;
}
STATIC int TestWaitServicing (VOID) {
  CHECK (RunNegotiation (WaitSoftReset) && mRequests == 2);
  CHECK (RunNegotiation (WaitNewCaps) && mRequests == 2);
  Reset (WaitNewCapsForever);
  CHECK (EFI_ERROR (Fusb302PdNegotiateSink (&mContext, UsbTypeCOrientationNormal)));
  CHECK (mRequests == 21 && !mContext.Contract.PdNegotiated && mHardSent == 0);
  return 1;
}
STATIC int TestWarmAndOrientation (VOID) {
  CHECK (RunNegotiation (LateSource) && mSoftSent == 1);
  CHECK (RunNegotiation (WarmReboot) && mSoftSent == 1);
  CHECK ((mRegisters[FUSB302_REG_SWITCHES1] & (FUSB302_SWITCHES1_TXCC1_EN | FUSB302_SWITCHES1_TXCC2_EN)) == FUSB302_SWITCHES1_TXCC1_EN);
  Reset (Normal);
  CHECK (Fusb302PdNegotiateSink (&mContext, UsbTypeCOrientationFlipped) == EFI_SUCCESS);
  CHECK ((mRegisters[FUSB302_REG_SWITCHES1] & (FUSB302_SWITCHES1_TXCC1_EN | FUSB302_SWITCHES1_TXCC2_EN)) == FUSB302_SWITCHES1_TXCC2_EN);
  CHECK ((mRegisters[FUSB302_REG_SWITCHES0] & (FUSB302_SWITCHES0_MEAS_CC1 | FUSB302_SWITCHES0_MEAS_CC2)) == FUSB302_SWITCHES0_MEAS_CC2);
  return 1;
}
STATIC int TestSoftReset (VOID) {
  CHECK (RunNegotiation (SoftOnRequest) && mRequests == 2);
  CHECK (RunNegotiation (SoftAfterAccept) && mRequests == 2);
  return 1;
}
STATIC int TestHardReset (VOID) {
  CHECK (RunNegotiation (HardOnRequest) && mRequests == 2);
  CHECK (RunNegotiation (HardOnSoftTransmit) && mRequests == 1);
  return 1;
}
STATIC int TestUnconfirmedContract (VOID) {
  Reset (QuietIoError);
  CHECK (Fusb302PdNegotiateSink (&mContext, UsbTypeCOrientationNormal) == EFI_DEVICE_ERROR);
  CHECK (!mContext.Contract.PdNegotiated && mContext.Contract.VoltageMv == 0 && mContext.Contract.CurrentMa == 0);
  Reset (NoPsReady);
  CHECK (Fusb302PdNegotiateSink (&mContext, UsbTypeCOrientationNormal) == EFI_TIMEOUT);
  CHECK (!mContext.Contract.PdNegotiated && mHardSent == 0);
  return 1;
}
STATIC int TestNonPdNoHardReset (VOID) {
  Reset (NoPd);
  CHECK (EFI_ERROR (Fusb302PdNegotiateSink (&mContext, UsbTypeCOrientationFlipped)));
  CHECK (mHardSent == 0 && !mContext.Contract.PdNegotiated && mContext.Contract.VoltageMv == 0);
  CHECK (mNow < 4000000000ULL);
  return 1;
}
STATIC int TestCrcReadError (VOID) {
  PD_MESSAGE Message;
  Reset (Normal); mInitialCaps = FALSE; QueueControl (PD_CTRL_ACCEPT); mFailFifoRead = 4;
  CHECK (Fusb302PdReceive (&mContext, &Message) == EFI_DEVICE_ERROR);
  return 1;
}
STATIC int TestResponseIoErrors (VOID) {
  CONST UINT8 Types[] = { PD_CTRL_GET_SINK_CAP, PD_CTRL_GET_SOURCE_CAP, PD_CTRL_PR_SWAP };
  PD_MESSAGE Message; UINTN Index;
  for (Index = 0; Index < sizeof (Types); Index++) {
    Reset (Normal); mInitialCaps = FALSE; QueueControl (Types[Index]); mFailFifoWrite = 1;
    CHECK (Fusb302PdWaitFor (&mContext, TRUE, PD_DATA_SOURCE_CAP, 60000, &Message) == EFI_DEVICE_ERROR);
  }
  return 1;
}
STATIC int TestDuplicates (VOID) {
  PD_MESSAGE Message;
  Reset (Normal); mInitialCaps = FALSE;
  QueueControl (PD_CTRL_GET_SINK_CAP); mSourceId = 0; QueueControl (PD_CTRL_GET_SINK_CAP);
  QueueControl (PD_CTRL_ACCEPT);
  CHECK (Fusb302PdWaitFor (&mContext, FALSE, PD_CTRL_ACCEPT, 60000, &Message) == EFI_SUCCESS);
  CHECK (mSinkCaps == 1);
  return 1;
}
STATIC int TestRepeatBound (VOID) {
  CHECK (RunNegotiation (RepeatedCaps));
  CHECK (mRequests == PD_SOURCE_CAP_REPEAT_LIMIT && mRxHead < mRxTail);
  return 1;
}
int main (void) {
  struct { const char *Name; int (*Run)(VOID); } Tests[] = {
    { "power selection and current cap", TestPowerSelection }, { "malformed and non-fixed PDOs", TestMalformedPdos },
    { "Request and confirmed contract", TestRequest }, { "Reject fallback", TestReject },
    { "bounded Wait retry", TestWait }, { "Wait services resets and new capabilities", TestWaitServicing },
    { "warm reboot and both PD CC routes", TestWarmAndOrientation }, { "partner Soft_Reset recovery", TestSoftReset },
    { "partner Hard_Reset recovery", TestHardReset }, { "unconfirmed contract on errors", TestUnconfirmedContract },
    { "non-PD never sends Hard_Reset", TestNonPdNoHardReset }, { "CRC I2C read errors", TestCrcReadError },
    { "policy response I2C errors", TestResponseIoErrors }, { "duplicate RX IDs", TestDuplicates }, { "repeat capability bound", TestRepeatBound }
  };
  unsigned Index, Failures = 0;
  for (Index = 0; Index < sizeof (Tests) / sizeof (Tests[0]); Index++) {
    int Passed = Tests[Index].Run (); Failures += !Passed;
    printf ("%s: %s\n", Passed ? "PASS" : "FAIL", Tests[Index].Name);
  }
  printf ("%u/%lu groups passed\n", Index - Failures, (unsigned long)Index);
  return Failures != 0;
}
