#include "callocators.h"

#include <fstream>

class CAllocatorCacheTy {
  const std::string file;
  // cache file format:
  // VERSION;<versionNumber>
  // DONE;<func>
  // CALLS;<func1>;<func2>
  // WRAPS;<func1>;<func2>
  // CSTARGET;<func>:<bbIdx>:<instIdx>;<func1>;<func2>;<func3>;...

  static constexpr unsigned formatVersion = 3;
  
  static constexpr char delimiter = ';';
  static constexpr char argDelimiter = ',';
  static constexpr char callInstDelimiter = ':';
  static constexpr char functionFingerprintDelimiter = '#';
  std::string encodeFunction(const Function* f);
  std::string encodeArgInfos(const ArgInfosVectorTy* argInfos);
  std::string encodeCalledFunction(const CalledFunctionTy *cf);
  std::string encodeCallInst(const Value *callInst);

  Function* decodeFunction(StringRef encoded, Module* m);
  const ArgInfoTy* decodeArgInfo(StringRef encoded);
  bool decodeCalledFunction(StringRef encoded, Module* m, Function*& outFn, ArgInfosVectorTy& outArgInfos);
  Value* decodeCallInst(StringRef encoded, Module* m);

public:

  CAllocatorCacheTy(std::string file): file(file) {};
  
  // checks whether the cache file can be opened for writing
  bool writeable() const {
    std::ofstream out(file);
    return out.good();
  }
  bool serialize(CalledModuleTy *cm);
  bool deserialize(CalledModuleTy *cm);
};

bool createRCacheFile(std::string baseIRFile, std::string cacheName);
