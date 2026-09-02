#pragma once

#include "RISCVModel/RVM.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace WdRiscv {
template <typename URV> class System;
template <typename URV> class Hart;
} // namespace WdRiscv

namespace snippy_whisper {

class ModelBase {
public:
  virtual ~ModelBase() = default;

  virtual const RVMConfig *config() const = 0;
  virtual void reset() = 0;

  virtual RVMSimExecStatus executeInstr() = 0;

  virtual RVMErrorCode readMem(uint64_t addr, size_t count, char *data) const = 0;
  virtual RVMErrorCode writeMem(uint64_t addr, size_t count, const char *data) = 0;

  virtual uint64_t readPC() const = 0;
  virtual RVMErrorCode setPC(uint64_t pc) = 0;

  virtual RVMErrorCode readXReg(RVMXReg reg, RVMRegT *value) const = 0;
  virtual RVMErrorCode setXReg(RVMXReg reg, RVMRegT value) = 0;

  virtual RVMErrorCode readFReg(RVMFReg reg, RVMRegT *value) const = 0;
  virtual RVMErrorCode setFReg(RVMFReg reg, RVMRegT value) = 0;

  virtual RVMErrorCode readCSR(unsigned csr, RVMRegT *value) const = 0;
  virtual RVMErrorCode setCSR(unsigned csr, RVMRegT value) = 0;

  virtual RVMErrorCode readVReg(RVMVReg reg, char *data, size_t *maxSize) const = 0;
  virtual RVMErrorCode setVReg(RVMVReg reg, const char *data, size_t *dataSize) = 0;

  virtual RVMErrorCode raiseInterrupt(RVMRegT value) = 0;
  virtual RVMErrorCode clearInterrupt(RVMRegT value) = 0;

  virtual void setStopMode(RVMStopMode mode) = 0;
  virtual RVMErrorCode setStopPC(uint64_t pc) = 0;

  virtual void logMessage(const char *message) const = 0;
  virtual void getErrorContext(char *buf, size_t *bufSize) const = 0;
};

template <typename URV>
class Model final : public ModelBase {
public:
  explicit Model(const RVMConfig *config);
  ~Model() override;

  const RVMConfig *config() const override { return &Config; }
  void reset() override;

  RVMSimExecStatus executeInstr() override;

  RVMErrorCode readMem(uint64_t addr, size_t count, char *data) const override;
  RVMErrorCode writeMem(uint64_t addr, size_t count, const char *data) override;

  uint64_t readPC() const override;
  RVMErrorCode setPC(uint64_t pc) override;

  RVMErrorCode readXReg(RVMXReg reg, RVMRegT *value) const override;
  RVMErrorCode setXReg(RVMXReg reg, RVMRegT value) override;

  RVMErrorCode readFReg(RVMFReg reg, RVMRegT *value) const override;
  RVMErrorCode setFReg(RVMFReg reg, RVMRegT value) override;

  RVMErrorCode readCSR(unsigned csr, RVMRegT *value) const override;
  RVMErrorCode setCSR(unsigned csr, RVMRegT value) override;

  RVMErrorCode readVReg(RVMVReg reg, char *data, size_t *maxSize) const override;
  RVMErrorCode setVReg(RVMVReg reg, const char *data, size_t *dataSize) override;

  RVMErrorCode raiseInterrupt(RVMRegT value) override;
  RVMErrorCode clearInterrupt(RVMRegT value) override;

  void setStopMode(RVMStopMode mode) override { StopMode = mode; }
  RVMErrorCode setStopPC(uint64_t pc) override { StopPC = pc; return RVM_ERRC_SUCCESS; }

  void logMessage(const char *message) const override;
  void getErrorContext(char *buf, size_t *bufSize) const override;

private:
  using SystemT = WdRiscv::System<URV>;
  using HartT = WdRiscv::Hart<URV>;

  void copyConfig(const RVMConfig *config);
  void configureHart();
  void configureExtensions();
  std::string buildIsaString() const;
  bool whisperSupportsExtension(std::string_view name) const;
  void openLogs();
  void closeLogs();
  void writeTrace(const char *fmt, ...) const;
#if 1
  void writeDebug(const char *fmt, ...) const;
#endif
  void setErrorContext(const char *fmt, ...) const;
  void clearErrorContext() const;
  void invokePCUpdate(uint64_t pc);
  void invokeXRegUpdate(unsigned reg, URV value);
  void invokeMemRead(uint64_t addr, const char *data, size_t size) const;
  void invokeMemUpdate(uint64_t addr, const char *data, size_t size);

private:
  RVMConfig Config{};
  std::vector<RVMMemoryRegion> Regions;
  std::string LogPath;
#if 1
  std::string DebugLogPath;
#endif

  std::unique_ptr<SystemT> System;
  std::shared_ptr<HartT> Hart;

  FILE *TraceFile = nullptr;
#if 1
  FILE *DebugFile = nullptr;
#endif
  bool CloseTraceFile = false;
#if 1
  bool CloseDebugFile = false;
#endif

  RVMStopMode StopMode = RVM_STOP_NEVER;
  uint64_t StopPC = 0;
  mutable std::string LastErrorContext;
};

} // namespace snippy_whisper
