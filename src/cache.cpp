#include "cache.h"

#include <fstream>
#include <sstream>

bool CAllocatorCacheTy::exists() const {
  std::ifstream infile(file);
  return infile.good();
}

// Encoding

std::string CAllocatorCacheTy::encodeFunction(const Function* f) {
  if (!f) {
    myassert("Cannot encode NULL function");
    return "";
  }
  return f->getName().str();
}

std::string CAllocatorCacheTy::encodeArgInfos(const ArgInfosVectorTy* argInfos) {
  if (!argInfos) {
    myassert("Cannot encode NULL argInfos");
    return "";
  }
  std::ostringstream oss;
  for (size_t i = 0; i < argInfos->size(); ++i) {
    if (i > 0) oss << argDelimiter;
    const ArgInfoTy* a = (*argInfos)[i];
    if (a && a->isSymbol()) {
      oss << "S:" << static_cast<const SymbolArgInfoTy*>(a)->symbolName;
    } else if (a && a->isVector()) {
      oss << "V";
    } else {
      oss << "?";
    }
  }
  return oss.str();
}

std::string CAllocatorCacheTy::encodeCalledFunction(const CalledFunctionTy *cf) {
  std::string result = encodeFunction(cf->fun);
  std::string args = encodeArgInfos(cf->argInfo);
  if (!args.empty()) {
    result += argDelimiter;
    result += args;
  }
  return result;
}

std::string CAllocatorCacheTy::encodeCallInst(const Value *callSite) {
  auto *ci = dyn_cast<CallInst>(callSite);
  if (!ci) {
    myassert("CallSite should be a CallInst");
    return "";
  }

  const Function *f = ci->getParent()->getParent();
  unsigned bbIdx = 0, instIdx = 0;

  for (auto &bb : *f) {
    if (&bb == ci->getParent()) break;
    bbIdx++;
  }
  for (auto &i : *ci->getParent()) {
    if (&i == ci) break;
    instIdx++;
  }

  return encodeFunction(f) + callInstDelimiter
       + std::to_string(bbIdx) + callInstDelimiter
       + std::to_string(instIdx);
}

// Serialization

bool CAllocatorCacheTy::serialize(CalledModuleTy *cm) {
  std::ofstream out(file);
  if (!out) {
    errs() << "[CACHE] Error opening cache file for writing: " << file << "\n";
    return false;
  }

  const CalledModuleTy::CAllocatorSeedDataTy *seed = cm->getCAllocatorSeedData(); // triggers computeCalledAllocators
  if (!seed) {
    errs() << "[CACHE] Missing seed data for serialization\n";
    return false;
  }

  for (const CalledFunctionTy* cf : seed->getDoneCallers()) {
    out << "DONE" << delimiter << encodeCalledFunction(cf) << "\n";
  }
  for (const auto& e : seed->getCallEdges()) {
    out << "CALLS" << delimiter << encodeCalledFunction(e.first) << delimiter << encodeCalledFunction(e.second) << "\n";
  }
  for (const auto& e : seed->getWrapEdges()) {
    out << "WRAPS" << delimiter << encodeCalledFunction(e.first) << delimiter << encodeCalledFunction(e.second) << "\n";
  }

  for (const auto& [callInst, calledFunctionSet] : *cm->getCallSiteTargets()) {
    out << "CSTARGET" << delimiter << encodeCallInst(callInst);
    for (const CalledFunctionTy* cf : calledFunctionSet) {
      out << delimiter << encodeCalledFunction(cf);
    }
    out << "\n";
  }

  return true;
}

// Decoding

Function* CAllocatorCacheTy::decodeFunction(StringRef encoded, Module* m) {
  return m->getFunction(encoded);
}

const ArgInfoTy* CAllocatorCacheTy::decodeArgInfo(StringRef encoded) {
  if (encoded.starts_with("S:")) {
    return SymbolArgInfoTy::create(encoded.substr(2).str());
  } else if (encoded == "V") {
    return VectorArgInfoTy::get();
  } else {
    return nullptr;
  }
}

bool CAllocatorCacheTy::decodeCalledFunction(StringRef encoded, Module* m, Function*& outFn, ArgInfosVectorTy& outArgInfos) {
  // encoded = "funcName,arg1,arg2,...,argN"
  SmallVector<StringRef, 16> parts;
  encoded.split(parts, argDelimiter);
  if (parts.empty()) return false;

  outFn = decodeFunction(parts[0], m);
  if (!outFn) {
    errs() << "[CACHE] Function not found: " << parts[0] << "\n";
    return false;
  }

  // args are parts[1..N], skip empty trailing parts
  for (size_t i = 1; i < parts.size(); ++i) {
    StringRef part = parts[i];
    if (part.empty()) continue;
    outArgInfos.push_back(decodeArgInfo(part));
  }
  return true;
}


Value* CAllocatorCacheTy::decodeCallInst(StringRef encoded, Module* m) {
  // encoded = "funcName:bbIdx:instIdx"
  SmallVector<StringRef, 4> parts;
  encoded.split(parts, callInstDelimiter);
  if (parts.size() != 3) {
    errs() << "[CACHE] Invalid call instruction encoding: " << encoded << "\n";
    return nullptr;
  }

  Function* f = decodeFunction(parts[0], m);
  if (!f) {
    errs() << "[CACHE] Function not found for call inst: " << parts[0] << "\n";
    return nullptr;
  }

  unsigned bbIdx, instIdx;
  if (parts[1].getAsInteger(10, bbIdx) || parts[2].getAsInteger(10, instIdx)) {
    errs() << "[CACHE] Invalid indices in call inst: " << encoded << "\n";
    return nullptr;
  }

  // Navigate to the instruction via basic block
  unsigned curBB = 0;
  for (auto &bb : *f) {
    if (curBB == bbIdx) {
      unsigned curInst = 0;
      for (auto &inst : bb) {
        if (curInst == instIdx) return &inst;
        curInst++;
      }
      errs() << "[CACHE] Instruction index " << instIdx << " out of range in " << parts[0] << "\n";
      return nullptr;
    }
    curBB++;
  }

  errs() << "[CACHE] Basic block index " << bbIdx << " out of range in " << parts[0] << "\n";
  return nullptr;
}

// Deserialization

bool CAllocatorCacheTy::deserialize(CalledModuleTy *cm) {
  std::ifstream in(file);
  if (!in) {
    errs() << "[CACHE] Error opening cache file for reading: " << file << "\n";
    return false;
  }

  CalledModuleTy::CAllocatorSeedDataTy *seed = new CalledModuleTy::CAllocatorSeedDataTy(cm);

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;

    SmallVector<StringRef, 16> parts;
    StringRef(line).split(parts, delimiter);

    if (parts.size() < 2) {
      errs() << "[CACHE] Invalid cache line: " << line << "\n";
      continue;
    }

    StringRef type = parts[0];

    if (type == "DONE") {
      Function* callerFn;
      ArgInfosVectorTy callerArgs;
      if (!decodeCalledFunction(parts[1], cm->getModule(), callerFn, callerArgs)) continue;
      seed->addDone(callerFn, callerArgs);

    } else if (type == "CALLS") {
      if (parts.size() != 3) {
        errs() << "[CACHE] CALLS needs caller and callee: " << line << "\n";
        continue;
      }
      Function* callerFn;
      ArgInfosVectorTy callerArgs;
      Function* calleeFn;
      ArgInfosVectorTy calleeArgs;
      if (!decodeCalledFunction(parts[1], cm->getModule(), callerFn, callerArgs)) continue;
      if (!decodeCalledFunction(parts[2], cm->getModule(), calleeFn, calleeArgs)) continue;
      seed->addCallEdge(callerFn, callerArgs, calleeFn, calleeArgs);

    } else if (type == "WRAPS") {
      if (parts.size() != 3) {
        errs() << "[CACHE] WRAPS needs caller and callee: " << line << "\n";
        continue;
      }
      Function* callerFn;
      ArgInfosVectorTy callerArgs;
      Function* calleeFn;
      ArgInfosVectorTy calleeArgs;
      if (!decodeCalledFunction(parts[1], cm->getModule(), callerFn, callerArgs)) continue;
      if (!decodeCalledFunction(parts[2], cm->getModule(), calleeFn, calleeArgs)) continue;
      seed->addWrapEdge(callerFn, callerArgs, calleeFn, calleeArgs);

    } else if (type == "CSTARGET") {
      // CSTARGET;func:bbIdx:instIdx;calledFunc1,args...;calledFunc2,args...;...
      if (parts.size() < 3) {
        errs() << "[CACHE] CSTARGET needs at least a call site and one target: " << line << "\n";
        continue;
      }

      Value* callInst = decodeCallInst(parts[1], cm->getModule());
      if (!callInst) {
        errs() << "[CACHE] Failed to decode call instruction: " << parts[1] << "\n";
        continue;
      }

      // parts[2..N] are each an encoded CalledFunction
      for (size_t i = 2; i < parts.size(); ++i) {
        if (parts[i].empty()) continue;

        Function* targetFn;
        ArgInfosVectorTy argInfos;
        if (!decodeCalledFunction(parts[i], cm->getModule(), targetFn, argInfos)) continue;
        cm->addToCallSiteTarget(callInst, targetFn, argInfos);
      }

    } else {
      errs() << "[CACHE] Unknown cache entry type: " << type << "\n";
    }
  }

  cm->setCAllocatorSeedData(std::move(seed));
  return true;
}
