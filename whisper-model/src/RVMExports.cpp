#include "SnippyWhisperModel.h"
#include "RISCVModel/VTable.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

using snippy_whisper::Model;
using snippy_whisper::ModelBase;

namespace {

ModelBase *asModel(RVMState *state) {
  return reinterpret_cast<ModelBase *>(state);
}

const ModelBase *asConstModel(const RVMState *state) {
  return reinterpret_cast<const ModelBase *>(state);
}

static void writeErr(RVMErrorCode code,
                     const char *message,
                     RVMErrorCode *err,
                     char *errBuf,
                     size_t errBufSize) {
  if (err)
    *err = code;

  if (!errBuf || errBufSize == 0)
    return;

  assert(message);
  const char *text = message;
  const size_t n = std::min(errBufSize - 1, std::strlen(text));
  std::memcpy(errBuf, text, n);
  errBuf[n] = '\0';
}

} // namespace

extern "C" {

uint32_t RVMInterfaceVersion = RVMAPI_CURRENT_INTERFACE_VERSION;

RVMState *rvm_modelCreate(const RVMConfig *config,
                          RVMErrorCode *err,
                          char *errBuf,
                          size_t errBufSize) {
  try {
    if (!config) {
      writeErr(RVM_ERRC_INVALID_ARGUMENT, "modelCreate: config is null", err, errBuf, errBufSize);
      return nullptr;
    }

    if (config->Extensions.ZExtSize != sizeof(config->Extensions.ZExt) ||
        config->Extensions.XExtSize != sizeof(config->Extensions.XExt)) {
      writeErr(RVM_ERRC_INCOMPATIBLE,
               "modelCreate: RVMExtDescriptor ZExtSize/XExtSize does not match this ABI",
               err,
               errBuf,
               errBufSize);
      return nullptr;
    }

    if (config->MemoryRegionCount && !config->MemoryRegions) {
      writeErr(RVM_ERRC_INVALID_ARGUMENT,
               "modelCreate: MemoryRegions is null while MemoryRegionCount is non-zero",
               err,
               errBuf,
               errBufSize);
      return nullptr;
    }

    std::unique_ptr<ModelBase> model;
    if (config->RV64)
      model = std::make_unique<Model<uint64_t>>(config);
    else
      model = std::make_unique<Model<uint32_t>>(config);

    writeErr(RVM_ERRC_SUCCESS, "Success", err, errBuf, errBufSize);
    return reinterpret_cast<RVMState *>(model.release());
  } catch (const std::exception &e) {
    writeErr(RVM_ERRC_UNRECOVERABLE_ERROR, e.what(), err, errBuf, errBufSize);
    return nullptr;
  } catch (...) {
    writeErr(RVM_ERRC_UNRECOVERABLE_ERROR, "modelCreate: unknown exception", err, errBuf, errBufSize);
    return nullptr;
  }
}

void rvm_modelDestroy(RVMState *state) {
  delete asModel(state);
}

void rvm_modelReset(RVMState *state) {
  if (state)
    asModel(state)->reset();
}

const RVMConfig *rvm_getModelConfig(const RVMState *state) {
  return state ? asConstModel(state)->config() : nullptr;
}

RVMSimExecStatus rvm_executeInstr(RVMState *state) {
  if (!state)
    return RVM_STEP_EXCEPTION;

  try {
    return asModel(state)->executeInstr();
  } catch (...) {
    return RVM_STEP_EXCEPTION;
  }
}

RVMErrorCode rvm_readMem(const RVMState *state, uint64_t addr, size_t count, char *data) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asConstModel(state)->readMem(addr, count, data);
}

RVMErrorCode rvm_writeMem(RVMState *state, uint64_t addr, size_t count, const char *data) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asModel(state)->writeMem(addr, count, data);
}

void rvm_setStopMode(RVMState *state, RVMStopMode mode) {
  if (state)
    asModel(state)->setStopMode(mode);
}

RVMErrorCode rvm_setStopPC(RVMState *state, uint64_t addr) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asModel(state)->setStopPC(addr);
}

uint64_t rvm_readPC(const RVMState *state) {
  return state ? asConstModel(state)->readPC() : 0;
}

RVMErrorCode rvm_setPC(RVMState *state, uint64_t pc) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asModel(state)->setPC(pc);
}

RVMErrorCode rvm_readXReg(const RVMState *state, RVMXReg reg, RVMRegT *value) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asConstModel(state)->readXReg(reg, value);
}

RVMErrorCode rvm_setXReg(RVMState *state, RVMXReg reg, RVMRegT value) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asModel(state)->setXReg(reg, value);
}

RVMErrorCode rvm_readFReg(const RVMState *state, RVMFReg reg, RVMRegT *value) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asConstModel(state)->readFReg(reg, value);
}

RVMErrorCode rvm_setFReg(RVMState *state, RVMFReg reg, RVMRegT value) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asModel(state)->setFReg(reg, value);
}

RVMErrorCode rvm_readCSR(const RVMState *state, unsigned csr, RVMRegT *value) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asConstModel(state)->readCSR(csr, value);
}

RVMErrorCode rvm_setCSR(RVMState *state, unsigned csr, RVMRegT value) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asModel(state)->setCSR(csr, value);
}

RVMErrorCode rvm_readVReg(const RVMState *state, RVMVReg reg, char *data, size_t *maxSize) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asConstModel(state)->readVReg(reg, data, maxSize);
}

RVMErrorCode rvm_setVReg(RVMState *state, RVMVReg reg, const char *data, size_t *dataSize) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asModel(state)->setVReg(reg, data, dataSize);
}

RVMErrorCode rvm_raiseInterrupt(RVMState *state, RVMRegT value) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asModel(state)->raiseInterrupt(value);
}

RVMErrorCode rvm_clearInterrupt(RVMState *state, RVMRegT value) {
  if (!state)
    return RVM_ERRC_INVALID_ARGUMENT;
  return asModel(state)->clearInterrupt(value);
}

void rvm_logMessage(const RVMState *state, const char *message) {
  if (state)
    asConstModel(state)->logMessage(message);
}

int rvm_queryCallbackSupportPresent(const RVMState *) {
  return 1;
}

void rvm_getErrorContext(const RVMState *state, char *buf, size_t *bufSize) {
  if (!bufSize)
    return;
  if (!state) {
    static constexpr const char *msg = "RVMState is null";
    const size_t required = std::strlen(msg) + 1;
    if (!buf) {
      *bufSize = required;
      return;
    }
    if (*bufSize) {
      const size_t n = std::min(*bufSize - 1, required - 1);
      std::memcpy(buf, msg, n);
      buf[n] = '\0';
    }
    *bufSize = required;
    return;
  }
  asConstModel(state)->getErrorContext(buf, bufSize);
}

rvm::RVM_FunctionPointers RVMVTable = {
    rvm_modelCreate,
    rvm_modelDestroy,
    rvm_modelReset,
    rvm_getModelConfig,
    rvm_executeInstr,
    rvm_readMem,
    rvm_writeMem,
    rvm_setStopMode,
    rvm_setStopPC,
    rvm_readPC,
    rvm_setPC,
    rvm_readXReg,
    rvm_setXReg,
    rvm_readFReg,
    rvm_setFReg,
    rvm_readCSR,
    rvm_setCSR,
    rvm_readVReg,
    rvm_setVReg,
    rvm_raiseInterrupt,
    rvm_clearInterrupt,
    rvm_logMessage,
    rvm_queryCallbackSupportPresent,
    rvm_getErrorContext,
};

} // extern "C"
