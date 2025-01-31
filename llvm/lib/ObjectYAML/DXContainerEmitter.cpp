//===- DXContainerEmitter.cpp - Convert YAML to a DXContainer -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Binary emitter for yaml to DXContainer binary
///
//===----------------------------------------------------------------------===//

#include "llvm/BinaryFormat/DXContainer.h"
#include "llvm/ObjectYAML/ObjectYAML.h"
#include "llvm/ObjectYAML/yaml2obj.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
class DXContainerWriter {
public:
  DXContainerWriter(DXContainerYAML::Object &ObjectFile)
      : ObjectFile(ObjectFile) {}

  Error write(raw_ostream &OS);

private:
  DXContainerYAML::Object &ObjectFile;

  Error computePartOffsets();
  Error validatePartOffsets();
  Error validateSize(uint32_t Computed);

  void writeHeader(raw_ostream &OS);
  void writeParts(raw_ostream &OS);
};
} // namespace

Error DXContainerWriter::validateSize(uint32_t Computed) {
  if (!ObjectFile.Header.FileSize)
    ObjectFile.Header.FileSize = Computed;
  else if (*ObjectFile.Header.FileSize < Computed)
    return createStringError(errc::result_out_of_range,
                             "File size specified is too small.");
  return Error::success();
}

Error DXContainerWriter::validatePartOffsets() {
  if (ObjectFile.Parts.size() != ObjectFile.Header.PartOffsets->size())
    return createStringError(
        errc::invalid_argument,
        "Mismatch between number of parts and part offsets.");
  uint32_t RollingOffset =
      sizeof(dxbc::Header) + (ObjectFile.Header.PartCount * sizeof(uint32_t));
  for (auto I : llvm::zip(ObjectFile.Parts, *ObjectFile.Header.PartOffsets)) {
    if (RollingOffset > std::get<1>(I))
      return createStringError(errc::invalid_argument,
                               "Offset mismatch, not enough space for data.");
    RollingOffset =
        std::get<1>(I) + sizeof(dxbc::PartHeader) + std::get<0>(I).Size;
  }
  if (Error Err = validateSize(RollingOffset))
    return Err;

  return Error::success();
}

Error DXContainerWriter::computePartOffsets() {
  if (ObjectFile.Header.PartOffsets)
    return validatePartOffsets();
  uint32_t RollingOffset =
      sizeof(dxbc::Header) + (ObjectFile.Header.PartCount * sizeof(uint32_t));
  ObjectFile.Header.PartOffsets = std::vector<uint32_t>();
  for (const auto &Part : ObjectFile.Parts) {
    ObjectFile.Header.PartOffsets->push_back(RollingOffset);
    RollingOffset += sizeof(dxbc::PartHeader) + Part.Size;
  }
  if (Error Err = validateSize(RollingOffset))
    return Err;

  return Error::success();
}

void DXContainerWriter::writeHeader(raw_ostream &OS) {
  dxbc::Header Header;
  memcpy(Header.Magic, "DXBC", 4);
  memcpy(Header.FileHash.Digest, ObjectFile.Header.Hash.data(), 16);
  Header.Version.Major = ObjectFile.Header.Version.Major;
  Header.Version.Minor = ObjectFile.Header.Version.Minor;
  Header.FileSize = *ObjectFile.Header.FileSize;
  Header.PartCount = ObjectFile.Parts.size();
  if (sys::IsBigEndianHost)
    Header.swapBytes();
  OS.write(reinterpret_cast<char *>(&Header), sizeof(Header));
  for (auto &O : *ObjectFile.Header.PartOffsets)
    if (sys::IsBigEndianHost)
      sys::swapByteOrder(O);
  OS.write(reinterpret_cast<char *>(ObjectFile.Header.PartOffsets->data()),
           ObjectFile.Header.PartOffsets->size() * sizeof(uint32_t));
}
void DXContainerWriter::writeParts(raw_ostream &OS) {
  uint32_t RollingOffset =
      sizeof(dxbc::Header) + (ObjectFile.Header.PartCount * sizeof(uint32_t));
  for (auto I : llvm::zip(ObjectFile.Parts, *ObjectFile.Header.PartOffsets)) {
    if (RollingOffset < std::get<1>(I)) {
      uint32_t PadBytes = std::get<1>(I) - RollingOffset;
      std::vector<uint8_t> FillData(PadBytes, 0);
      OS.write(reinterpret_cast<char *>(FillData.data()), PadBytes);
    }
    DXContainerYAML::Part P = std::get<0>(I);
    OS.write(P.Name.c_str(), 4);
    if (sys::IsBigEndianHost)
      sys::swapByteOrder(P.Size);
    OS.write(reinterpret_cast<const char *>(&P.Size), sizeof(uint32_t));
    RollingOffset = std::get<1>(I) + sizeof(dxbc::PartHeader);

    // TODO: Write Part data
  }
}

Error DXContainerWriter::write(raw_ostream &OS) {
  if (Error Err = computePartOffsets())
    return Err;
  writeHeader(OS);
  writeParts(OS);
  return Error::success();
}

namespace llvm {
namespace yaml {

bool yaml2dxcontainer(DXContainerYAML::Object &Doc, raw_ostream &Out,
                      ErrorHandler EH) {
  DXContainerWriter Writer(Doc);
  if (Error Err = Writer.write(Out)) {
    handleAllErrors(std::move(Err),
                    [&](const ErrorInfoBase &Err) { EH(Err.message()); });
    return false;
  }
  return true;
}

} // namespace yaml
} // namespace llvm
