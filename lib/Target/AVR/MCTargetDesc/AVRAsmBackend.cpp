//===-- AVRAsmBackend.cpp - AVR Asm Backend  ------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the AVRAsmBackend class.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/AVRAsmBackend.h"

#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixupKindInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include "MCTargetDesc/AVRFixupKinds.h"
#include "MCTargetDesc/AVRMCTargetDesc.h"

// FIXME: we should be doing checks to make sure asm operands
// are not out of bounds.

namespace adjust {

using namespace llvm;

template <typename T>
void signed_width(unsigned Width, T Value, std::string Description,
                  const MCFixup &Fixup, MCContext *Ctx = nullptr) {
  if (!isIntN(Width, Value)) {
    std::string Diagnostic = "out of range " + Description;

    int64_t Min = minIntN(Width);
    int64_t Max = maxIntN(Width);

    Diagnostic += " (expected an integer in the range " + std::to_string(Min) +
      " to " + std::to_string(Max) + ")";

    if (Ctx) {
      Ctx->reportFatalError(Fixup.getLoc(), Diagnostic);
    } else {
      llvm_unreachable(Diagnostic.c_str());
    }
  }
}

template <typename T>
void unsigned_width(unsigned Width, T Value, std::string Description,
                    const MCFixup &Fixup, MCContext *Ctx = nullptr) {
  if (!isUIntN(Width, Value)) {
    std::string Diagnostic = "out of range " + Description;

    int64_t Max = maxUIntN(Width);

    Diagnostic += " (expected an integer in the range 0 to " +
      std::to_string(Max) + ")";

    if (Ctx) {
      Ctx->reportFatalError(Fixup.getLoc(), Diagnostic);
    } else {
      llvm_unreachable(Diagnostic.c_str());
    }
  }
}

/// Adjusts the value of a branch target before fixup application.
template <typename T>
void adjustBranch(unsigned Size, const MCFixup &Fixup, T &Value,
                  MCContext *Ctx = nullptr) {
  // We have one extra bit of precision because the value is rightshifted by
  // one.
  unsigned_width(Size + 1, Value, std::string("branch target"), Fixup, Ctx);

  // Rightshifts the value by one.
  AVR::fixups::adjustBranchTarget(Value);
}

/// Adjusts the value of a relative branch target before fixup application.
template <typename T>
void adjustRelativeBranch(unsigned Size, const MCFixup &Fixup, T &Value,
                          MCContext *Ctx = nullptr) {
  // We have one extra bit of precision because the value is rightshifted by
  // one.
  signed_width(Size + 1, Value, std::string("branch target"), Fixup, Ctx);

  Value -= 2;

  // Rightshifts the value by one.
  AVR::fixups::adjustBranchTarget(Value);
}

/// 22-bit absolute fixup.
///
/// Resolves to:
/// 1001 kkkk 010k kkkk kkkk kkkk 111k kkkk
///
/// Offset of 0 (so the result is left shifted by 3 bits before application).
template <typename T>
void fixup_call(unsigned Size, const MCFixup &Fixup, T &Value,
                MCContext *Ctx = nullptr) {
  adjustBranch(Size, Fixup, Value, Ctx);

  auto top = Value & (0xf << 14);       // the top four bits
  auto middle = Value & (0x1ffff << 5); // the middle 13 bits
  auto bottom = Value & 0x1f;           // end bottom 5 bits

  Value = (top << 6) | (middle << 3) | (bottom << 0);
}

/// 7-bit PC-relative fixup.
///
/// Resolves to:
/// 0000 00kk kkkk k000
/// Offset of 0 (so the result is left shifted by 3 bits before application).
template <typename T>
void fixup_7_pcrel(unsigned Size, const MCFixup &Fixup, T &Value,
                   MCContext *Ctx = nullptr) {
  adjustRelativeBranch(Size, Fixup, Value, Ctx);

  // Because the value may be negative, we must mask out the sign bits
  Value &= 0x7f;
}

/// 12-bit PC-relative fixup.
/// Yes, the fixup is 12 bits even though the name says otherwise.
///
/// Resolves to:
/// 0000 kkkk kkkk kkkk
/// Offset of 0 (so the result isn't left-shifted before application).
template <typename T>
void fixup_13_pcrel(unsigned Size, const MCFixup &Fixup, T &Value,
                    MCContext *Ctx = nullptr) {
  adjustRelativeBranch(Size, Fixup, Value, Ctx);

  // Because the value may be negative, we must mask out the sign bits
  Value &= 0xfff;
}

/// 6-bit fixup for the immediate operand of the ADIW family of
/// instructions.
///
/// Resolves to:
/// 0000 0000 kk00 kkkk
template <typename T>
void fixup_6_adiw(const MCFixup &Fixup, T &Value,
                  MCContext *Ctx = nullptr) {
  unsigned_width(6, Value, std::string("immediate"), Fixup, Ctx);

  Value = ((Value & 0x30) << 2) | (Value & 0x0f);
}

/// 5-bit port number fixup on the SBIC family of instructions.
///
/// Resolves to:
/// 0000 0000 AAAA A000
template <typename T>
void fixup_port5(const MCFixup &Fixup, T &Value,
                 MCContext *Ctx = nullptr) {
  unsigned_width(5, Value, std::string("port number"), Fixup, Ctx);

  Value &= 0x1f;

  Value <<= 3;
}

/// 6-bit port number fixup on the `IN` family of instructions.
///
/// Resolves to:
/// 1011 0AAd dddd AAAA
template <typename T>
void fixup_port6(const MCFixup &Fixup, T &Value,
                 MCContext *Ctx = nullptr) {
  unsigned_width(6, Value, std::string("port number"), Fixup, Ctx);

  Value = ((Value & 0x30) << 5) | (Value & 0x0f);
}

/// Adjusts a program memory address.
/// This is a simply right-shift.
template <typename T>
void pm(T &Value) {
  Value >>= 1;
}

/// Fixups relating to the LDI instruction.
namespace ldi {

/// Adjusts a value to fix up the immediate of an `LDI Rd, K` instruction.
///
/// Resolves to:
/// 0000 KKKK 0000 KKKK
/// Offset of 0 (so the result isn't left-shifted before application).
template <typename T>
void fixup(unsigned Size, const MCFixup &Fixup, T &Value,
           MCContext *Ctx = nullptr) {
  T upper = Value & 0xf0;
  T lower = Value & 0x0f;

  Value = (upper << 4) | lower;
}

template <typename T> void neg(T &Value) { Value *= -1; }

template <typename T>
void lo8(unsigned Size, const MCFixup &Fixup, T &Value,
         MCContext *Ctx = nullptr) {
  Value &= 0xff;
  ldi::fixup(Size, Fixup, Value, Ctx);
}

template <typename T>
void hi8(unsigned Size, const MCFixup &Fixup, T &Value,
         MCContext *Ctx = nullptr) {
  Value = (Value & 0xff00) >> 8;
  ldi::fixup(Size, Fixup, Value, Ctx);
}

template <typename T>
void hh8(unsigned Size, const MCFixup &Fixup, T &Value,
         MCContext *Ctx = nullptr) {
  Value = (Value & 0xff0000) >> 16;
  ldi::fixup(Size, Fixup, Value, Ctx);
}

template <typename T>
void ms8(unsigned Size, const MCFixup &Fixup, T &Value,
         MCContext *Ctx = nullptr) {
  Value = (Value & 0xff000000) >> 24;
  ldi::fixup(Size, Fixup, Value, Ctx);
}

} // end of ldi namespace
} // end of adjust namespace

namespace llvm {

// Prepare value for the target space for it
void AVRAsmBackend::adjustFixupValue(const MCFixup &Fixup, uint64_t &Value,
                                     MCContext *Ctx) const {

  // The size of the fixup in bits.
  uint64_t Size = AVRAsmBackend::getFixupKindInfo(Fixup.getKind()).TargetSize;

  unsigned Kind = Fixup.getKind();

  switch (Kind) {
  default:
    llvm_unreachable("unhandled fixup");
  case AVR::fixup_7_pcrel:
    adjust::fixup_7_pcrel(Size, Fixup, Value, Ctx);
    break;
  case AVR::fixup_13_pcrel:
    adjust::fixup_13_pcrel(Size, Fixup, Value, Ctx);
    break;
  case AVR::fixup_call:
    adjust::fixup_call(Size, Fixup, Value, Ctx);
    break;

  case AVR::fixup_ldi:
    adjust::ldi::fixup(Size, Fixup, Value, Ctx);
    break;
  case AVR::fixup_lo8_ldi:
  case AVR::fixup_lo8_ldi_pm:
    if (Kind == AVR::fixup_lo8_ldi_pm) adjust::pm(Value);

    adjust::ldi::lo8(Size, Fixup, Value, Ctx);
    break;
  case AVR::fixup_hi8_ldi:
  case AVR::fixup_hi8_ldi_pm:
    if (Kind == AVR::fixup_hi8_ldi_pm) adjust::pm(Value);

    adjust::ldi::hi8(Size, Fixup, Value, Ctx);
    break;
  case AVR::fixup_hh8_ldi:
  case AVR::fixup_hh8_ldi_pm:
    if (Kind == AVR::fixup_hh8_ldi_pm) adjust::pm(Value);

    adjust::ldi::hh8(Size, Fixup, Value, Ctx);
    break;
  case AVR::fixup_ms8_ldi:
    adjust::ldi::ms8(Size, Fixup, Value, Ctx);
    break;

  case AVR::fixup_lo8_ldi_neg:
  case AVR::fixup_lo8_ldi_pm_neg:
    if (Kind == AVR::fixup_lo8_ldi_pm_neg) adjust::pm(Value);

    adjust::ldi::neg(Value);
    adjust::ldi::lo8(Size, Fixup, Value, Ctx);
    break;
  case AVR::fixup_hi8_ldi_neg:
  case AVR::fixup_hi8_ldi_pm_neg:
    if (Kind == AVR::fixup_hi8_ldi_pm_neg) adjust::pm(Value);

    adjust::ldi::neg(Value);
    adjust::ldi::hi8(Size, Fixup, Value, Ctx);
    break;
  case AVR::fixup_hh8_ldi_neg:
  case AVR::fixup_hh8_ldi_pm_neg:
    if (Kind == AVR::fixup_hh8_ldi_pm_neg) adjust::pm(Value);

    adjust::ldi::neg(Value);
    adjust::ldi::hh8(Size, Fixup, Value, Ctx);
    break;
  case AVR::fixup_ms8_ldi_neg:
    adjust::ldi::neg(Value);
    adjust::ldi::ms8(Size, Fixup, Value, Ctx);
    break;

  case AVR::fixup_16:
    adjust::unsigned_width(16, Value, std::string("port number"), Fixup, Ctx);

    Value &= 0xffff;
    break;

  case AVR::fixup_6_adiw:
    adjust::fixup_6_adiw(Fixup, Value, Ctx);
    break;

  case AVR::fixup_port5:
    adjust::fixup_port5(Fixup, Value, Ctx);
    break;

  case AVR::fixup_port6:
    adjust::fixup_port6(Fixup, Value, Ctx);
    break;

  // Fixups which do not require adjustments.
  case FK_Data_2:
  case FK_Data_4:
  case FK_Data_8:
    break;

  case FK_GPRel_4:
    llvm_unreachable("don't know how to adjust this fixup");
    break;
  }
}

MCObjectWriter *AVRAsmBackend::createObjectWriter(raw_pwrite_stream &OS) const {
  return createAVRELFObjectWriter(OS,
                                  MCELFObjectTargetWriter::getOSABI(OSType));
}

/// Apply the \p Value for given \p Fixup into the provided
/// data fragment, at the offset specified by the fixup and following the
/// fixup kind as appropriate.
void AVRAsmBackend::applyFixup(const MCFixup &Fixup, char *Data,
                               unsigned DataSize, uint64_t Value,
                               bool IsPCRel) const {
  if (Value == 0)
    return; // Doesn't change encoding.

  MCFixupKindInfo Info = getFixupKindInfo(Fixup.getKind());

  // The number of bits in the fixup mask
  auto NumBits = Info.TargetSize + Info.TargetOffset;
  auto NumBytes = (NumBits / 8) + ((NumBits % 8) == 0 ? 0 : 1);

  // Shift the value into position.
  Value <<= Info.TargetOffset;

  unsigned Offset = Fixup.getOffset();
  assert(Offset + NumBytes <= DataSize && "Invalid fixup offset!");

  // For each byte of the fragment that the fixup touches, mask in the
  // bits from the fixup value.
  for (unsigned i = 0; i < NumBytes; ++i) {
    uint8_t mask = (((Value >> (i * 8)) & 0xff));
    Data[Offset + i] |= mask;
  }
}

MCFixupKindInfo const &AVRAsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  // NOTE: Many AVR fixups are non-contignous. We work around this by
  // saying that the fixup is the size of the entire instruction (16 or 32 bits).
  const static MCFixupKindInfo Infos[AVR::NumTargetFixupKinds] = {
      // This table *must* be in same the order of fixup_* kinds in
      // AVRFixupKinds.h.
      //
      // name                    offset  bits  flags
      {"fixup_32", 0, 32, 0},

      {"fixup_7_pcrel", 3, 7, MCFixupKindInfo::FKF_IsPCRel},
      {"fixup_13_pcrel", 0, 12, MCFixupKindInfo::FKF_IsPCRel},

      {"fixup_16", 0, 16, 0},
      {"fixup_16_pm", 0, 16, 0},

      {"fixup_ldi", 0, 8, 0},

      {"fixup_lo8_ldi", 0, 8, 0},
      {"fixup_hi8_ldi", 0, 8, 0},
      {"fixup_hh8_ldi", 0, 8, 0},
      {"fixup_ms8_ldi", 0, 8, 0},

      {"fixup_lo8_ldi_neg", 0, 8, 0},
      {"fixup_hi8_ldi_neg", 0, 8, 0},
      {"fixup_hh8_ldi_neg", 0, 8, 0},
      {"fixup_ms8_ldi_neg", 0, 8, 0},

      {"fixup_lo8_ldi_pm", 0, 8, 0},
      {"fixup_hi8_ldi_pm", 0, 8, 0},
      {"fixup_hh8_ldi_pm", 0, 8, 0},

      {"fixup_lo8_ldi_pm_neg", 0, 8, 0},
      {"fixup_hi8_ldi_pm_neg", 0, 8, 0},
      {"fixup_hh8_ldi_pm_neg", 0, 8, 0},

      {"fixup_call", 0, 22, 0},

      {"fixup_6", 0, 16, 0}, // non-contiguous
      {"fixup_6_adiw", 0, 6, 0},

      {"fixup_lo8_ldi_gs", 0, 8, 0},
      {"fixup_hi8_ldi_gs", 0, 8, 0},

      {"fixup_8", 0, 8, 0},
      {"fixup_8_lo8", 0, 8, 0},
      {"fixup_8_hi8", 0, 8, 0},
      {"fixup_8_hlo8", 0, 8, 0},

      {"fixup_sym_diff", 0, 32, 0},
      {"fixup_16_ldst", 0, 16, 0},

      {"fixup_lds_sts_16", 0, 16, 0},

      {"fixup_port6", 0, 16, 0}, // non-contiguous
      {"fixup_port5", 3, 5, 0},

  };

  if (Kind < FirstTargetFixupKind)
    return MCAsmBackend::getFixupKindInfo(Kind);

  assert(unsigned(Kind - FirstTargetFixupKind) < getNumFixupKinds() &&
         "Invalid kind!");

  return Infos[Kind - FirstTargetFixupKind];
}

/// Write an (optimal) nop sequence of Count bytes
/// to the given output. If the target cannot generate such a sequence,
/// it should return an error.
bool AVRAsmBackend::writeNopData(uint64_t Count, MCObjectWriter *OW) const {
  // If the count is not 2-byte aligned, we must be writing data into the text
  // section (otherwise we have unaligned instructions, and thus have far
  // bigger problems), so just write zeros instead.
  assert((Count % 2) == 0 && "NOP instructions must be 2 bytes");

  OW->WriteZeros(Count);
  return true;
}

/// Target hook to process the literal value of a fixup
/// if necessary.
void AVRAsmBackend::processFixupValue(const MCAssembler &Asm,
                                      const MCAsmLayout &Layout,
                                      const MCFixup &Fixup,
                                      const MCFragment *DF,
                                      const MCValue &Target, uint64_t &Value,
                                      bool &IsResolved) {
  switch ((unsigned) Fixup.getKind()) {
  // Fixups which should always be record as relocations.
  case AVR::fixup_7_pcrel:
  case AVR::fixup_13_pcrel:
  case AVR::fixup_call:
    IsResolved = false;
    break;
  default:
    // Parsed LLVM-generated temporary labels are already
    // adjusted for instruction size, but normal labels aren't.
    //
    // To handle both cases, we simply un-adjust the temporary label
    // case so it acts like all other labels.
    if (Target.getSymA()->getSymbol().isTemporary())
      Value += 2;

    adjustFixupValue(Fixup, Value, &Asm.getContext());
  }

}

MCAsmBackend *createAVRAsmBackend(const Target &T, const MCRegisterInfo &MRI,
                                  const Triple &TT, StringRef CPU) {
  return new AVRAsmBackend(T, TT.getOS());
}

} // end of namespace llvm
