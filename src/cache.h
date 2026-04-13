#include "callocators.h"

#include <fstream>

class CAllocatorCacheTy {
  const std::string file;
  // cache file format:
  // DONE;callerFunc,arg1,...,argN
  // CALLS;callerFunc,arg1,...,argN;calleeFunc,arg1,...,argN
  // WRAPS;callerFunc,arg1,...,argN;calleeFunc,arg1,...,argN
  // CSTARGET;func:bbIdx:instIdx;calledFunc1,args;calledFunc2,args;...

  char delimiter = ';';
  char argDelimiter = ',';
  char callInstDelimiter = ':';

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
