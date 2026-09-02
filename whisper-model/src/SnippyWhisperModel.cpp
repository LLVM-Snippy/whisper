#include "SnippyWhisperModel.h"

#include "System.hpp"
#include "Hart.hpp"
#include "CsRegs.hpp"
#include "Isa.hpp"
#include "DecodedInst.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace snippy_whisper {

static uint64_t defaultMemSize(const RVMConfig *config) {
  uint64_t end = 0;

  for (unsigned i = 0; i < config->MemoryRegionCount; ++i) {
    const auto &r = config->MemoryRegions[i];
    end = std::max(end, r.Start + r.Size);
  }

  return end ? end : 128ull * 1024ull * 1024ull;
}

template <typename URV>
Model<URV>::Model(const RVMConfig *config) {
  copyConfig(config);

  const size_t memSize = defaultMemSize(&Config);
  const size_t pageSize = 4096;

  System = std::make_unique<SystemT>(
      /*coreCount=*/1,
      /*hartsPerCore=*/1,
      /*hartIdOffset=*/1,
      memSize,
      pageSize);

  Hart = System->ithHart(0);
  if (!Hart)
    throw std::runtime_error("Whisper did not create hart0");

  openLogs();

  configureHart();

  if (Config.Mode == RVM_STOP_BY_PC)
    setStopPC(Config.StopAddr);

  writeTrace("SnippyWhisperModel created: RV%u VLEN=%u StopMode=%u StopPC=0x%016llx\n",
             Config.RV64 ? 64u : 32u,
             Config.VLEN,
             static_cast<unsigned>(StopMode),
             static_cast<unsigned long long>(StopPC));
}

template <typename URV>
Model<URV>::~Model() {
  closeLogs();
}

template <typename URV>
void Model<URV>::copyConfig(const RVMConfig *config) {
  Config = *config;

  Regions.assign(config->MemoryRegions,
                 config->MemoryRegions + config->MemoryRegionCount);
  Config.MemoryRegions = Regions.data();

  if (config->LogFilePath) {
    LogPath = config->LogFilePath;
    Config.LogFilePath = LogPath.c_str();
  }

#if 1
  if (config->DebugLogFilePath) {
    DebugLogPath = config->DebugLogFilePath;
    Config.DebugLogFilePath = DebugLogPath.c_str();
  }
#endif

  StopMode = config->Mode;
  StopPC = config->StopAddr;
}

static FILE *openRVMLogFile(const char *path, bool &shouldClose) {
  shouldClose = false;

  if (!path)
    return nullptr;

  if (path[0] == '\0')
    return stdout;

  if (std::strcmp(path, "-") == 0)
    return stderr;

  FILE *file = std::fopen(path, "w");
  if (file)
    shouldClose = true;
  return file;
}

template <typename URV>
void Model<URV>::openLogs() {
  TraceFile = openRVMLogFile(Config.LogFilePath, CloseTraceFile);
  if (Config.LogFilePath && !TraceFile)
    throw std::runtime_error("Cannot open Whisper trace file");

#if 1
  DebugFile = openRVMLogFile(Config.DebugLogFilePath, CloseDebugFile);
  if (Config.DebugLogFilePath && !DebugFile)
    throw std::runtime_error("Cannot open Whisper debug log file");
#endif
}

template <typename URV>
void Model<URV>::closeLogs() {
  if (TraceFile && CloseTraceFile)
    std::fclose(TraceFile);
#if 1
  if (DebugFile && CloseDebugFile)
    std::fclose(DebugFile);
#endif

  TraceFile = nullptr;
#if 1
  DebugFile = nullptr;
#endif
  CloseTraceFile = false;
#if 1
  CloseDebugFile = false;
#endif
}

template <typename URV>
void Model<URV>::writeTrace(const char *fmt, ...) const {
  if (!TraceFile || !fmt)
    return;

  va_list args;
  va_start(args, fmt);
  std::vfprintf(TraceFile, fmt, args);
  va_end(args);
  std::fflush(TraceFile);
}

#if 1
template <typename URV>
void Model<URV>::writeDebug(const char *fmt, ...) const {
  if (!DebugFile || !fmt)
    return;

  va_list args;
  va_start(args, fmt);
  std::vfprintf(DebugFile, fmt, args);
  va_end(args);
  std::fflush(DebugFile);
}
#endif

template <typename URV>
void Model<URV>::setErrorContext(const char *fmt, ...) const {
  if (!fmt) {
    LastErrorContext.clear();
    return;
  }

  char buf[512];
  va_list args;
  va_start(args, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (n < 0) {
    LastErrorContext = "failed to format error context";
    return;
  }

  if (static_cast<size_t>(n) < sizeof(buf)) {
    LastErrorContext.assign(buf, static_cast<size_t>(n));
    return;
  }

  std::vector<char> dyn(static_cast<size_t>(n) + 1);
  va_start(args, fmt);
  std::vsnprintf(dyn.data(), dyn.size(), fmt, args);
  va_end(args);
  LastErrorContext.assign(dyn.data(), static_cast<size_t>(n));
}

template <typename URV>
void Model<URV>::clearErrorContext() const {
  LastErrorContext.clear();
}

template <typename URV>
void Model<URV>::getErrorContext(char *buf, size_t *bufSize) const {
  if (!bufSize)
    return;

  const size_t required = LastErrorContext.size() + 1;
  if (!buf) {
    *bufSize = required;
    return;
  }

  const size_t capacity = *bufSize;
  if (capacity == 0) {
    *bufSize = required;
    return;
  }

  const size_t n = std::min(capacity - 1, LastErrorContext.size());
  std::memcpy(buf, LastErrorContext.data(), n);
  buf[n] = '\0';
  *bufSize = required;
}

namespace {

static bool rvmMisaEnabled(const RVMConfig &config, RVMMisaExt ext) {
  return ext >= 0 && ext < RVM_MISA_NUMBER && config.Extensions.MisaExt[ext];
}

static bool rvmZExtEnabled(const RVMConfig &config, RVMZExt ext) {
  return ext >= 0 && ext < RVM_ZEXT_NUMBER && config.Extensions.ZExt[ext];
}

static bool rvmXExtEnabled(const RVMConfig &config, RVMXExt ext) {
  return ext >= 0 && ext < RVM_XEXT_NUMBER && config.Extensions.XExt[ext];
}

static std::string rvmZExtName(RVMZExt ext) {
  switch (ext) {
#define RVM_ZEXT_NAME_CASE(Name, name) case Name: return std::string("z") + #name;
    RVM_FOR_EACH_ZEXT(RVM_ZEXT_NAME_CASE)
#undef RVM_ZEXT_NAME_CASE
  default:
    return {};
  }
}

static std::string rvmXExtName(RVMXExt ext) {
  switch (ext) {
#define RVM_XEXT_NAME_CASE(Name, name) case Name: return std::string("x") + #name;
    RVM_FOR_EACH_XEXT(RVM_XEXT_NAME_CASE)
#undef RVM_XEXT_NAME_CASE
  default:
    return {};
  }
}

} // namespace

template <typename URV>
bool Model<URV>::whisperSupportsExtension(std::string_view name) const {
  const auto ext = WdRiscv::Isa::stringToExtension(name);
  if (ext == WdRiscv::RvExtension::None)
    return false;

  WdRiscv::Isa isa;
  return isa.isSupported(ext);
}

template <typename URV>
std::string Model<URV>::buildIsaString() const {
  std::string isa = Config.RV64 ? "rv64" : "rv32";
  std::string singleLetterExts;
  std::set<std::string> appendedLongExts;

  auto appendSingle = [&](char ext) {
    std::string name(1, ext);
#if 1
    if (!whisperSupportsExtension(name)) {
      writeDebug("ISA mapping: skipping unsupported MISA extension '%s'\n",
                 name.c_str());
      return;
    }
#endif
    if (singleLetterExts.find(ext) == std::string::npos)
      singleLetterExts.push_back(ext);
  };

  auto appendLong = [&](const std::string &name) {
    if (name.empty())
      return;
#if 1
    if (!whisperSupportsExtension(name)) {
      writeDebug("ISA mapping: skipping unsupported extension '%s'\n",
                 name.c_str());
      return;
    }
#endif
    appendedLongExts.insert(name);
  };

  const bool hasG = rvmMisaEnabled(Config, RVM_MISA_G);

  // Base ISA must contain exactly one of i/e. Prefer E only if explicitly
  // requested without I/G. Otherwise default to I: snippy snippets normally
  // assume the full x0..x31 register file.
  if (rvmMisaEnabled(Config, RVM_MISA_E) &&
      !rvmMisaEnabled(Config, RVM_MISA_I) && !hasG)
    appendSingle('e');
  else
    appendSingle('i');

  if (hasG) {
    // G is a pseudo-extension: IMAFD plus Zicsr/Zifencei.
    appendSingle('m');
    appendSingle('a');
    appendSingle('f');
    appendSingle('d');
    appendLong("zicsr");
    appendLong("zifencei");
  }

  if (rvmMisaEnabled(Config, RVM_MISA_M)) appendSingle('m');
  if (rvmMisaEnabled(Config, RVM_MISA_A)) appendSingle('a');
  if (rvmMisaEnabled(Config, RVM_MISA_F)) appendSingle('f');
  if (rvmMisaEnabled(Config, RVM_MISA_D)) appendSingle('d');
  if (rvmMisaEnabled(Config, RVM_MISA_C)) appendSingle('c');
  if (rvmMisaEnabled(Config, RVM_MISA_B)) appendSingle('b');
  if (rvmMisaEnabled(Config, RVM_MISA_H)) appendSingle('h');
  if (rvmMisaEnabled(Config, RVM_MISA_N)) appendSingle('n');
  if (rvmMisaEnabled(Config, RVM_MISA_S)) appendSingle('s');
  if (rvmMisaEnabled(Config, RVM_MISA_U)) appendSingle('u');

  if (rvmMisaEnabled(Config, RVM_MISA_V)) {
    // Current Whisper requires scalar F/D to accept the V bit in MISA. Enabling
    // them here prevents V from being silently ignored while keeping snippy's
    // generated instruction stream unchanged.
    if (!rvmMisaEnabled(Config, RVM_MISA_F)) {
#if 1
      writeDebug("ISA mapping: enabling F because Whisper requires F/D for V\n");
#endif
      appendSingle('f');
    }
    if (!rvmMisaEnabled(Config, RVM_MISA_D)) {
#if 1
      writeDebug("ISA mapping: enabling D because Whisper requires F/D for V\n");
#endif
      appendSingle('d');
    }
    appendSingle('v');
  }

  isa += singleLetterExts;

  for (unsigned i = 0; i < RVM_ZEXT_NUMBER; ++i) {
    const auto ext = static_cast<RVMZExt>(i);
    if (rvmZExtEnabled(Config, ext))
      appendLong(rvmZExtName(ext));
  }

  for (unsigned i = 0; i < RVM_XEXT_NUMBER; ++i) {
    const auto ext = static_cast<RVMXExt>(i);
    if (rvmXExtEnabled(Config, ext))
      appendLong(rvmXExtName(ext));
  }

  for (const auto &name : appendedLongExts) {
    isa += '_';
    isa += name;
  }

  return isa;
}

template <typename URV>
void Model<URV>::configureExtensions() {
  const std::string isa = buildIsaString();
#if 1
  writeDebug("ISA mapping: configuring Whisper ISA '%s'\n", isa.c_str());
#endif

  if (!Hart->configIsa(isa, /*updateMisa=*/true)) {
#if 1
    writeDebug("ISA mapping: Hart::configIsa('%s') failed\n", isa.c_str());
#endif
    throw std::runtime_error("Whisper ISA configuration failed");
  }
}

template <typename URV>
void Model<URV>::configureHart() {
  configureExtensions();
  Hart->reset();
  Hart->setCacheLineSize(8);
  Hart->enableAbiNames(true);

  if (Config.VLEN) {
    const unsigned bytesPerVec = Config.VLEN / 8;
    Hart->configVector(bytesPerVec,
                       /*minBytesPerElem=*/1,
                       /*maxBytesPerElem=*/8,
                       /*minSewPerLmul=*/nullptr,
                       /*maxSewPerLmul=*/nullptr);
    Hart->configMaskAgnosticAllOnes(Config.ChangeMaskAgnosticElems);
    Hart->configTailAgnosticAllOnes(Config.ChangeTailAgnosticElems);
    // for each EEW, enable use of canonical NaN in vfredusum/vfwredusum result
    Hart->configVectorFpUnorderedSumCanonical(WdRiscv::ElementWidth::Byte,
                                              true);
    Hart->configVectorFpUnorderedSumCanonical(WdRiscv::ElementWidth::Half,
                                              true);
    Hart->configVectorFpUnorderedSumCanonical(WdRiscv::ElementWidth::Word,
                                              true);
    Hart->configVectorFpUnorderedSumCanonical(WdRiscv::ElementWidth::Word2,
                                              true);
  }
}

template <typename URV>
void Model<URV>::reset() {
  Hart->reset();
}

namespace {

template <typename T>
static void appendLittleEndianBytes(T value, unsigned size, std::vector<char> &out) {
  out.resize(size);
  for (unsigned i = 0; i < size; ++i)
    out[i] = static_cast<char>((static_cast<uint64_t>(value) >> (8 * i)) & 0xffu);
}

} // namespace

template <typename URV>
RVMSimExecStatus Model<URV>::executeInstr() {
  const uint64_t pcBefore = readPC();
  writeTrace("step: pc_before=0x%016llx\n",
             static_cast<unsigned long long>(pcBefore));

  if (StopMode == RVM_STOP_BY_PC && pcBefore == StopPC)
    return RVM_STEP_FINISH;

  WdRiscv::DecodedInst di;

  try {
    Hart->singleStep(di, TraceFile);
  } catch (const WdRiscv::CoreException &e) {
    writeTrace("step: core exception at pc=0x%016llx type=%d value=0x%016llx\n",
               static_cast<unsigned long long>(pcBefore),
               static_cast<int>(e.type()),
               static_cast<unsigned long long>(e.value()));

    // CoreException is not an architectural trap trace. Whisper may throw it for
    // stop/tohost/exit. There is no reliable per-instruction change log to report
    // here unless singleStep completed, so callbacks intentionally remain silent.
    if (e.type() == WdRiscv::CoreException::Stop ||
        e.type() == WdRiscv::CoreException::Exit)
      return RVM_STEP_FINISH;

    return RVM_STEP_EXCEPTION;
  } catch (...) {
    writeTrace("step: unknown C++ exception at pc=0x%016llx\n",
               static_cast<unsigned long long>(pcBefore));
    return RVM_STEP_EXCEPTION;
  }

  const uint64_t pcAfter = readPC();
  writeTrace("step: pc_after =0x%016llx\n",
             static_cast<unsigned long long>(pcAfter));

  const bool trapped = Hart->lastInstructionTrapped();
  if (trapped) {
    writeTrace("step: architectural trap at pc=0x%016llx cause=0x%016llx\n",
               static_cast<unsigned long long>(pcBefore),
               static_cast<unsigned long long>(Hart->lastTrapCause()));
  }

  // All snippy state callbacks are intentionally emitted only here, after a
  // completed singleStep. Explicit pokes from writeMem/setPC/set*Reg/setCSR are
  // initial-state/control operations and must not be reported as executed
  // instruction effects.
  if (Config.CallbackHandler) {
    if (Config.PCUpdateCallback) {
      writeDebug("notify: pc=0x%016llx\n",
                 static_cast<unsigned long long>(pcAfter));
      Config.PCUpdateCallback(Config.CallbackHandler, pcAfter);
    }

    if (Config.MemReadCallback && !trapped && di.isValid()) {
      if (di.isLoad() || di.isLr() || di.isAmo()) {
        // Scalar memory read callback. Whisper exposes the effective
        // address/size of the last load/store, but it does not expose a full
        // read data log. For scalar loads we re-read the bytes after the step.
        // This is correct for ordinary loads in our bare-metal use case; AMOs
        // are reported as both read and write, with read bytes sampled after
        // execution.
        uint64_t virtAddr = 0;
        uint64_t physAddr = 0;
        const unsigned size = Hart->lastLdStAddress(virtAddr, physAddr);
        if (size) {
          std::vector<char> data(size);
          bool ok = true;
          for (unsigned i = 0; i < size; ++i) {
            uint8_t byte = 0;
            if (!Hart->peekMemory(virtAddr + i, byte, /*usePma=*/false)) {
              ok = false;
              break;
            }
            data[i] = static_cast<char>(byte);
          }

          if (ok) {
            writeDebug("notify: mem-read addr=0x%016llx size=%u\n",
                       static_cast<unsigned long long>(virtAddr), size);
            Config.MemReadCallback(Config.CallbackHandler, virtAddr,
                                   data.data(), data.size());
          } else {
            writeDebug("notify: mem-read addr=0x%016llx size=%u skipped: peek "
                       "failed\n",
                       static_cast<unsigned long long>(virtAddr), size);
          }
        }
      } else if (di.isVectorLoad()) {
        // Vector memory read callback. Whisper exposes the effective
        // address/size/data of the last load/store for all vector elements.
        const auto &vLdStInfo = Hart->getLastVectorMemory();
        auto size = vLdStInfo.elemSize_;
        for (const auto &elem : vLdStInfo.elems_) {
          auto data = elem.data_;
          auto virtAddr = elem.va_;
          writeDebug("notify: mem-read addr=0x%016llx size=%u\n",
                     static_cast<unsigned long long>(virtAddr), size);
          Config.MemReadCallback(Config.CallbackHandler, virtAddr,
                                 reinterpret_cast<const char *>(&data), size);
        }
      }
    }

    if (Config.MemUpdateCallback && !trapped && di.isValid()) {
      if (di.isStore() || di.isSc() || di.isAmo()) {
        // Scalar memory write callback. Prefer the virtual address form so the
        // callback sees the same address space as snippy snippets.
        uint64_t virtAddr = 0;
        uint64_t physAddr1 = 0;
        uint64_t physAddr2 = 0;
        uint64_t value = 0;
        const unsigned size =
            Hart->lastStore(virtAddr, physAddr1, physAddr2, value);
        if (size) {
          std::vector<char> data;
          appendLittleEndianBytes(value, size, data);

          writeDebug(
              "notify: mem-write addr=0x%016llx size=%u value=0x%016llx\n",
              static_cast<unsigned long long>(virtAddr), size,
              static_cast<unsigned long long>(value));
          Config.MemUpdateCallback(Config.CallbackHandler, virtAddr,
                                   data.data(), data.size());
        }
      }
      if (di.isVectorStore()) {
        // Vector memory write callback. Whisper exposes the effective
        // address/size/data of the last load/store for all vector elements.
        const auto &vLdStInfo = Hart->getLastVectorMemory();
        auto size = vLdStInfo.elemSize_;
        for (const auto &elem : vLdStInfo.elems_) {
          auto data = elem.data_;
          auto virtAddr = elem.va_;
          writeDebug("notify: mem-write addr=0x%016llx size=%u\n",
                     static_cast<unsigned long long>(virtAddr), size);
          Config.MemUpdateCallback(Config.CallbackHandler, virtAddr,
                                   reinterpret_cast<const char *>(&data), size);
        }
      }
    }

    if (Config.XRegUpdateCallback && !trapped) {
      const int reg = Hart->lastIntReg();
      if (reg > 0) { // x0 is immutable and should not be reported.
        URV value = 0;
        if (Hart->peekIntReg(static_cast<unsigned>(reg), value)) {
          writeDebug("notify: xreg x%d=0x%016llx\n",
                     reg,
                     static_cast<unsigned long long>(value));
          Config.XRegUpdateCallback(Config.CallbackHandler,
                                    static_cast<RVMXReg>(reg),
                                    static_cast<RVMRegT>(value));
        }
      }
    }

    if (Config.FRegUpdateCallback && !trapped) {
      const int reg = Hart->lastFpReg();
      if (reg >= 0) {
        uint64_t value = 0;
        if (Hart->peekFpReg(static_cast<unsigned>(reg), value)) {
          writeDebug("notify: freg f%d=0x%016llx\n",
                     reg,
                     static_cast<unsigned long long>(value));
          Config.FRegUpdateCallback(Config.CallbackHandler,
                                    static_cast<RVMFReg>(reg),
                                    static_cast<RVMRegT>(value));
        }
      }
    }

    if (Config.VRegUpdateCallback && !trapped && di.isValid() && di.isVector()) {
      unsigned group = 1;
      const int firstReg = Hart->lastVecReg(di, group);
      if (firstReg >= 0) {
        const unsigned regCount = std::max(1u, group);
        const size_t vlenBytes = Hart->vecRegSize()
                                   ? Hart->vecRegSize()
                                   : static_cast<size_t>(Config.VLEN / 8);
        std::vector<uint8_t> bytes;
        std::vector<char> data(vlenBytes);

        for (unsigned offset = 0; offset < regCount; ++offset) {
          const unsigned reg = static_cast<unsigned>(firstReg) + offset;
          if (reg >= 32)
            break;

          bytes.clear();
          if (!Hart->peekVecRegLsb(reg, bytes))
            continue;

          const size_t n = std::min(data.size(), bytes.size());
          std::fill(data.begin(), data.end(), 0);
          std::memcpy(data.data(), bytes.data(), n);

          writeDebug("notify: vreg v%u size=%zu group=%u\n",
                     reg,
                     data.size(),
                     group);
          Config.VRegUpdateCallback(Config.CallbackHandler,
                                    static_cast<RVMVReg>(reg),
                                    data.data(),
                                    data.size());
        }
      }
    }

    if (Config.CSRUpdateCallback) {
      std::vector<WdRiscv::CsrNumber> csrs;
      Hart->lastCsr(csrs);
      for (auto csr : csrs) {
        const auto value = Hart->lastCsrValue(csr);
        const auto csrNumber = static_cast<unsigned>(csr);
        writeDebug("notify: csr 0x%03x=0x%016llx\n",
                   csrNumber,
                   static_cast<unsigned long long>(value));
        Config.CSRUpdateCallback(Config.CallbackHandler,
                                 static_cast<RVMCSR>(csrNumber),
                                 static_cast<RVMRegT>(value));
      }
    }
  }

  if (trapped)
    return RVM_STEP_EXCEPTION;

  if (StopMode == RVM_STOP_BY_PC && pcAfter == StopPC)
    return RVM_STEP_FINISH;

  return RVM_STEP_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::readMem(uint64_t addr, size_t count, char *data) const {
  if (!data && count != 0) {
    setErrorContext("readMem: Data is null for addr=0x%016llx size=%zu",
                    static_cast<unsigned long long>(addr), count);
    return RVM_ERRC_INVALID_ARGUMENT;
  }

  for (size_t i = 0; i < count; ++i) {
    uint8_t byte = 0;
    if (!Hart->peekMemory(addr + i, byte, /*usePma=*/false)) {
      setErrorContext("readMem: invalid address 0x%016llx",
                      static_cast<unsigned long long>(addr + i));
      return RVM_ERRC_INVALID_ADDRESS;
    }
    data[i] = static_cast<char>(byte);
  }

  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::writeMem(uint64_t addr, size_t count, const char *data) {
  if (!data && count != 0) {
    setErrorContext("writeMem: Data is null for addr=0x%016llx size=%zu",
                    static_cast<unsigned long long>(addr), count);
    return RVM_ERRC_INVALID_ARGUMENT;
  }

  for (size_t i = 0; i < count; ++i) {
    const auto byte = static_cast<uint8_t>(data[i]);
    if (!Hart->pokeMemory(addr + i, byte, /*usePma=*/false)) {
      setErrorContext("writeMem: invalid address 0x%016llx",
                      static_cast<unsigned long long>(addr + i));
      return RVM_ERRC_INVALID_ADDRESS;
    }
  }
#if 1
  writeDebug("writeMem: addr=0x%016llx size=%zu\n",
             static_cast<unsigned long long>(addr), count);
#endif
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
uint64_t Model<URV>::readPC() const {
  return Hart->pc();
}

template <typename URV>
RVMErrorCode Model<URV>::setPC(uint64_t pc) {
  if constexpr (sizeof(URV) == 4) {
    if (pc >> 32) {
      setErrorContext("setPC: value 0x%016llx is out of RV32 range",
                      static_cast<unsigned long long>(pc));
      return RVM_ERRC_INVALID_ADDRESS;
    }
  }

  Hart->pokePc(static_cast<URV>(pc));
#if 1
  writeDebug("setPC: 0x%016llx\n", static_cast<unsigned long long>(pc));
#endif
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::readXReg(RVMXReg reg, RVMRegT *value) const {
  if (!value) {
    setErrorContext("readXReg: Val is null");
    return RVM_ERRC_INVALID_ARGUMENT;
  }
  if (static_cast<unsigned>(reg) >= 32) {
    setErrorContext("readXReg: invalid register index %u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }

  URV v = 0;
  if (!Hart->peekIntReg(static_cast<unsigned>(reg), v)) {
    setErrorContext("readXReg: Whisper failed to read x%u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }
  *value = static_cast<RVMRegT>(v);
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::setXReg(RVMXReg reg, RVMRegT value) {
  if (static_cast<unsigned>(reg) >= 32) {
    setErrorContext("setXReg: invalid register index %u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }
  if constexpr (sizeof(URV) == 4) {
    if (value >> 32) {
      setErrorContext("setXReg: value 0x%016llx is out of RV32 range",
                      static_cast<unsigned long long>(value));
      return RVM_ERRC_INVALID_ARGUMENT;
    }
  }

  const unsigned idx = static_cast<unsigned>(reg);
  if (!Hart->pokeIntReg(idx, static_cast<URV>(value))) {
    setErrorContext("setXReg: Whisper failed to write x%u", idx);
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::readFReg(RVMFReg reg, RVMRegT *value) const {
  if (!value) {
    setErrorContext("readFReg: Val is null");
    return RVM_ERRC_INVALID_ARGUMENT;
  }
  if (static_cast<unsigned>(reg) >= 32) {
    setErrorContext("readFReg: invalid register index %u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }

  uint64_t v = 0;
  if (!Hart->peekFpReg(static_cast<unsigned>(reg), v)) {
    setErrorContext("readFReg: Whisper failed to read f%u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }
  *value = static_cast<RVMRegT>(v);
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::setFReg(RVMFReg reg, RVMRegT value) {
  if (static_cast<unsigned>(reg) >= 32) {
    setErrorContext("setFReg: invalid register index %u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }
  if (!Hart->pokeFpReg(static_cast<unsigned>(reg), value)) {
    setErrorContext("setFReg: Whisper failed to write f%u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::readCSR(unsigned csr, RVMRegT *value) const {
  if (!value) {
    setErrorContext("readCSR: Val is null for csr=0x%03x", csr);
    return RVM_ERRC_INVALID_ARGUMENT;
  }

  URV v = 0;
  if (!Hart->peekCsr(static_cast<WdRiscv::CsrNumber>(csr), v)) {
    setErrorContext("readCSR: unsupported csr=0x%03x", csr);
    return RVM_ERRC_INVALID_ADDRESS;
  }
  *value = static_cast<RVMRegT>(v);
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::setCSR(unsigned csr, RVMRegT value) {
  if constexpr (sizeof(URV) == 4) {
    if (value >> 32) {
      setErrorContext("setCSR: value 0x%016llx for csr=0x%03x is out of RV32 range",
                      static_cast<unsigned long long>(value), csr);
      return RVM_ERRC_VALUE_OUT_OF_RANGE;
    }
  }

  if (!Hart->pokeCsr(static_cast<WdRiscv::CsrNumber>(csr),
                     static_cast<URV>(value))) {
    setErrorContext("setCSR: unsupported csr=0x%03x", csr);
    return RVM_ERRC_INVALID_ADDRESS;
  }
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::readVReg(RVMVReg reg, char *data, size_t *maxSize) const {
  if (!maxSize) {
    setErrorContext("readVReg: MaxSize is null");
    return RVM_ERRC_INVALID_ARGUMENT;
  }
  if (static_cast<unsigned>(reg) >= 32) {
    setErrorContext("readVReg: invalid register index %u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }

  std::vector<uint8_t> bytes;
  if (!Hart->peekVecRegLsb(static_cast<unsigned>(reg), bytes)) {
    setErrorContext("readVReg: Whisper failed to read v%u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }

  const size_t required = bytes.size();
  if (!data) {
    *maxSize = required;
    clearErrorContext();
    return RVM_ERRC_SUCCESS;
  }

  const size_t n = std::min(*maxSize, required);
  std::memcpy(data, bytes.data(), n);
  *maxSize = required;
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::setVReg(RVMVReg reg, const char *data, size_t *dataSize) {
  if (!dataSize) {
    setErrorContext("setVReg: DataSize is null");
    return RVM_ERRC_INVALID_ARGUMENT;
  }
  if (!data) {
    *dataSize = Hart->vecRegSize() ? Hart->vecRegSize() : static_cast<size_t>(Config.VLEN / 8);
    setErrorContext("setVReg: Data is null");
    return RVM_ERRC_INVALID_ARGUMENT;
  }
  if (static_cast<unsigned>(reg) >= 32) {
    setErrorContext("setVReg: invalid register index %u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }

  std::vector<uint8_t> bytes(*dataSize);
  std::memcpy(bytes.data(), data, *dataSize);

  if (!Hart->pokeVecRegLsb(static_cast<unsigned>(reg), bytes)) {
    setErrorContext("setVReg: Whisper failed to write v%u", static_cast<unsigned>(reg));
    return RVM_ERRC_IDX_OUT_OF_RANGE;
  }

  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::raiseInterrupt(RVMRegT value) {
#if 1
  writeDebug("raiseInterrupt: value=0x%016llx ignored by MVP adapter\n",
             static_cast<unsigned long long>(value));
#endif
  (void)value;
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
RVMErrorCode Model<URV>::clearInterrupt(RVMRegT value) {
#if 1
  writeDebug("clearInterrupt: value=0x%016llx ignored by MVP adapter\n",
             static_cast<unsigned long long>(value));
#endif
  (void)value;
  clearErrorContext();
  return RVM_ERRC_SUCCESS;
}

template <typename URV>
void Model<URV>::logMessage(const char *message) const {
  if (!message)
    return;

  if (TraceFile) {
    std::fprintf(TraceFile, "%s\n", message);
    std::fflush(TraceFile);
  } else {
    std::fprintf(stderr, "%s\n", message);
  }
}

template <typename URV>
void Model<URV>::invokePCUpdate(uint64_t pc) {
  if (Config.PCUpdateCallback && Config.CallbackHandler)
    Config.PCUpdateCallback(Config.CallbackHandler, pc);
}

template <typename URV>
void Model<URV>::invokeXRegUpdate(unsigned reg, URV value) {
  if (Config.XRegUpdateCallback && Config.CallbackHandler)
    Config.XRegUpdateCallback(Config.CallbackHandler,
                              static_cast<RVMXReg>(reg),
                              static_cast<RVMRegT>(value));
}

template <typename URV>
void Model<URV>::invokeMemRead(uint64_t addr,
                               const char *data,
                               size_t size) const {
  if (Config.MemReadCallback && Config.CallbackHandler)
    Config.MemReadCallback(Config.CallbackHandler, addr, data, size);
}

template <typename URV>
void Model<URV>::invokeMemUpdate(uint64_t addr,
                                 const char *data,
                                 size_t size) {
  if (Config.MemUpdateCallback && Config.CallbackHandler)
    Config.MemUpdateCallback(Config.CallbackHandler, addr, data, size);
}

template class Model<uint32_t>;
template class Model<uint64_t>;

} // namespace snippy_whisper
