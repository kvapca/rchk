#include "cache.h"

#include "common.h"

#include <algorithm>
#include <functional>
#include <vector>

// This program tests whether CAllocatorCacheTy correctly serializes and deserializes objects from CalledModuleTy
// NOTE: As this test recomputes entire analysis, clearCommonCaches() must be called
//       before each test to clear results from static caches

std::string cacheName = "./src/main/R.bin.cache";
std::string baseIRFile = "./src/main/R.bin.bc";
std::string survivalIRFile = "./library/survival/libs/survival.so.bc";
std::string execName = "../src/cachecheck";

// There are multiple interning tables involved in CalledModuleTy and ArgInfoTy:
//  - CalledFunctionsOrderedSetTy interned via osTable
//  - CalledFunctionTy interned via calledFunctionsTable
//  - ArgInfosVectorTy interned via argInfoVectorsTable
//  - SymbolArgInfoTy interned via SymbolArgInfoTy::table
//
// In order check whether deserialization correctly restores the sets of CalledFunctionTy,
// we need to compare them without relying on pointers of interned elements.

struct CalledFunctionCmp {
  // uses fun and argInfo for comparison, ignore module and idx as they differ between runs
  bool operator()(const CalledFunctionTy* a, const CalledFunctionTy* b) const {
    return a->getName() < b->getName();
  }
};

// Helper functions
template<typename T, typename Comparator>
std::vector<T> toSortedVector(const std::unordered_set<T>* set, Comparator comparator) {
  std::vector<T> vec(set->begin(), set->end());
  std::sort(vec.begin(), vec.end(), comparator);
  return vec;
}

template<typename SetTy, typename Comparator = std::less<typename SetTy::value_type>>
bool compareSets(const SetTy *original, const SetTy *restored, Comparator cmp = Comparator{}) {
  if (original->size() != restored->size()) {
    errs() << "Set sizes do not match: " << original->size() << " vs " << restored->size() << "\n";
    return false;
  }

  auto originalVec = toSortedVector(original, cmp);
  auto restoredVec = toSortedVector(restored, cmp);

  using ElemTy = typename SetTy::value_type;
  return std::equal(originalVec.begin(), originalVec.end(), restoredVec.begin(),
    [&cmp](ElemTy a, ElemTy b) {
      return !cmp(a, b) && !cmp(b, a);
    });
}

bool compareCalledFunctionsMaps(const CallSiteTargetsTy *original, const CallSiteTargetsTy *restored) {
  if (original->size() != restored->size()) {
    errs() << "Set sizes do not match: " << original->size() << " vs " << restored->size() << "\n";
    return false;
  }

  for (auto const& [key, val] : *original) {
    if (restored->find(key) == restored->end()) {
      return false;
    }
    if (!compareSets(&val, &restored->at(key), CalledFunctionCmp{})) {
      return false;
    }
  }
  return true;
}

bool compareCalledModules(CalledModuleTy *original, CalledModuleTy *restored) {
  bool ok = true;
  if (!compareSets(original->getPossibleCAllocators(), restored->getPossibleCAllocators(), CalledFunctionCmp{})) {
    errs() << "Possible CAllocators do not match\n";
    ok = false;
  }
  if (!compareSets(original->getAllocatingCFunctions(), restored->getAllocatingCFunctions(), CalledFunctionCmp{})) {
    errs() << "Allocating CFunctions do not match\n";
    ok = false;
  }
  if (!compareSets(original->getContextSensitivePossibleAllocators(), restored->getContextSensitivePossibleAllocators())) {
    errs() << "ContextSensitivePossibleAllocators do not match\n";
    ok = false;
  }
  if (!compareSets(original->getContextSensitiveAllocatingFunctions(), restored->getContextSensitiveAllocatingFunctions())) {
    errs() << "ContextSensitiveAllocatingFunctions do not match\n";
    ok = false;
  }
  if (!compareCalledFunctionsMaps(original->getCallSiteTargets(), restored->getCallSiteTargets())) {
    errs() << "CallSiteTargets do not match\n";
    ok = false;
  }
  if (ok) {
    errs() << "All checks passed\n";
  }
  return ok;
}

void runCheck(int argc, char* argv[]) {
  LLVMContext context;
  FunctionsOrderedSetTy functionsOfInterestSet;
  FunctionsVectorTy functionsOfInterestVector;
  std::string cacheFile;
  Module *m = parseArgsReadIR(argc, argv, functionsOfInterestSet, functionsOfInterestVector, context, &cacheFile);
  
  // create original CalledModuleTy
  CalledModuleTy *original = CalledModuleTy::create(m);
  original->getCallSiteTargets(); // triggers computeCalledAllocators

  // restore CalledModuleTy from cache
  CAllocatorCacheTy cache(cacheFile);
  CalledModuleTy *restored = CalledModuleTy::create(m);
  cache.deserialize(restored);

  errs() << "\n";
  compareCalledModules(original, restored);
}

// checks whether serializing and deserializing R cache works
void checkR() {
  clearCommonCaches();
  const char* argv[] = {execName.c_str(), "--cache", cacheName.c_str(), baseIRFile.c_str()};
  int argc = sizeof(argv) / sizeof(argv[0]);
  errs() << "\nComparing original and restored R cache\n";
  runCheck(argc, const_cast<char**>(argv));
}

// checks whether serializing and deserializing R cache works with survival package
void checkRWithPackage(std::string packageIRFile) {
  clearCommonCaches();
  const char* argv[] = {execName.c_str(), "--cache", cacheName.c_str(), baseIRFile.c_str(), packageIRFile.c_str()};
  int argc = sizeof(argv) / sizeof(argv[0]);
  errs() << "\nComparing original and restored R cache with survival package\n";
  runCheck(argc, const_cast<char**>(argv));
}

void malformedArgsCheck() {
  clearCommonCaches();
  const char* argv[] = {execName.c_str(), "--cache", cacheName.c_str(), baseIRFile.c_str()};
  int argc = sizeof(argv) / sizeof(argv[0]);
  errs() << "\nTesting malformed arguments\n";
  runCheck(argc, const_cast<char**>(argv));
}

int main() {
  createRCacheFile(baseIRFile, cacheName);

  checkR();
  checkRWithPackage(survivalIRFile);
  malformedArgsCheck();
  return 0;
}
