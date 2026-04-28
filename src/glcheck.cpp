/*
  This tool is to detect global variables/structures that may (accidentally)
  hold SEXPs, but possibly are not known as roots to the GC.
*/ 

#include "common.h"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <llvm/Support/raw_ostream.h>

#include <unordered_map>
#include <unordered_set>

#include "symbols.h"

using namespace llvm;

DICompositeType* resolveForwardDeclaration(DICompositeType* t, Module* m) {
  if (!t) return nullptr;

  // Cache names and actual composite types on first use for each module.
  static Module* cachedModule = nullptr;
  static std::unordered_map<std::string, DICompositeType*> cache;
  if (cachedModule != m) {
    cachedModule = m;
    cache.clear();

    DebugInfoFinder finder;
    finder.processModule(*m);

    for (auto *type : finder.types()) {
      if (auto *composite = dyn_cast<DICompositeType>(type)) {
        if (!composite->isForwardDecl()) {
          cache.emplace(composite->getName().str(), composite);
        }
      }
    }
  }

  auto it = cache.find(t->getName().str());
  return it != cache.end() ? it->second : nullptr;
}

// return true if there is SEXP somewhere within type t
typedef std::unordered_set<DIType*> DITypeSetTy;

bool containsSEXP(DIType *t, DITypeSetTy& visited, Module* m) {
  if (!t) return false;
  
  if (visited.find(t) != visited.end()) {
    // already under evaluation (recursive type)
    return false;
  }
  visited.insert(t);

  if (auto *derived = dyn_cast<DIDerivedType>(t)) {
    return containsSEXP(derived->getBaseType(), visited, m);
  }

  if (auto *composite = dyn_cast<DICompositeType>(t)) {
    if (composite->isForwardDecl()) {
      return containsSEXP(resolveForwardDeclaration(composite, m), visited, m);
    }

    if (composite->getTag() == dwarf::DW_TAG_array_type)
      return containsSEXP(composite->getBaseType(), visited, m);

    if (composite->getTag() == dwarf::DW_TAG_structure_type) {
      if (composite->getName() == "SEXPREC" || composite->getName() == "struct SEXPREC") {
        return true;
      }
  
      for (auto *element : composite->getElements()) {
        if (containsSEXP(cast<DIType>(element), visited, m)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool isStructureWithSEXPFields(GlobalVariable *gv, Module* m) {
  SmallVector<DIGlobalVariableExpression *> debugInfoVector;
  gv->getDebugInfo(debugInfoVector);

  for (auto debugInfo : debugInfoVector) {
    if (!debugInfo) continue;
    DIGlobalVariable *variable = debugInfo->getVariable();

    if (!variable) continue;
    DIType *type = variable->getType();

    DITypeSetTy visited;
    if (containsSEXP(type, visited, m)) {
      return true;
    }
  }

  return false;
}

int main(int argc, char* argv[])
{
  LLVMContext context;
  FunctionsOrderedSetTy functionsOfInterestSet;
  FunctionsVectorTy functionsOfInterestVector;
  
  Module *m = parseArgsReadIR(argc, argv, functionsOfInterestSet, functionsOfInterestVector, context);
    // NOTE: functionsOfInterest ignored but (re-)analyzing the R core is necessary
  
  SymbolsMapTy symbolsMap;
  findSymbols(m, &symbolsMap); // symbols are globals which hold SEXPs, but are safe
  
  for(Module::global_iterator gi = m->global_begin(), ge = m->global_end(); gi != ge ; ++gi) {
    GlobalVariable *gv = &*gi;
    
    if (isSEXP(gv)) {
      if (symbolsMap.find(gv) != symbolsMap.end()) {
        continue;
        }
    
      errs() << "non-symbol SEXP global variable " << gv->getName() << "  " << *gv << "\n";
      // many of these are OK, but it does not seem to be easily checkable
      continue;
    }
    
    if (isStructureWithSEXPFields(gv, m)) {
      errs() << "structure with SEXP fields " << gv->getName() << " " << *gv << "\n";
    }
  }
  
  delete m;
}
