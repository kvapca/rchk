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

// return true if there is SEXP somewhere within type t
typedef std::unordered_set<Type*> TypeSetTy;

bool containsSEXP(Type *t, TypeSetTy& visited) {

  if (visited.find(t) != visited.end()) {
    // already under evaluation (recursive type)
    return false;
  }
  visited.insert(t);
  
  if (ArrayType *at = dyn_cast<ArrayType>(t)) {
    return containsSEXP(at->getElementType(), visited);
  }

  if (PointerType *pt = dyn_cast<PointerType>(t)) {
    return containsSEXP(pt->getPointerElementType(), visited);
  }
  
  if (VectorType *vt = dyn_cast<VectorType>(t)) {
    return containsSEXP(vt->getPointerElementType(), visited);
  }

  if (StructType *st = dyn_cast<StructType>(t)) {

    if (st->hasName() && st->getName() == "struct.SEXPREC") {
      return true;
    }
    unsigned nelems = st->getNumElements();
    for(unsigned i = 0; i < nelems; i++) {
      if (containsSEXP(st->getElementType(i), visited)) {
        return true;
      }
    }
  }
  return false;
}

DICompositeType* resolveForwardDeclaration(DICompositeType* t, Module* m) {
  if (!t) return nullptr;

  // cache name and actual composite type on first run
  static Module* cachedModule = nullptr;
  static std::unordered_map<std::string, DICompositeType*> cache;
  if (cachedModule == m) {
    cachedModule = m;
    cache.clear();

    DebugInfoFinder finder;
    finder.processModule(*m);

    for (auto *type : finder.types()) {
      if (auto *composite = dyn_cast<DICompositeType>(type)) {
        if (!composite->isForwardDecl()) {
          if (composite->getName() == t->getName()) {
            return composite;
          }
          cache.emplace(composite->getName().str(), composite);
        }
      }
    }
  }

  // retrieve from cache
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
  // TODO: remove after testing
  TypeSetTy visited_old;
  bool original = containsSEXP(gv->getType(), visited_old);
  bool current = false;

  DITypeSetTy visited;

  SmallVector<DIGlobalVariableExpression *> debugInfoVector;
  gv->getDebugInfo(debugInfoVector);

  for (auto debugInfo : debugInfoVector) {
    if (!debugInfo) continue;
    DIGlobalVariable *variable = debugInfo->getVariable();

    if (!variable) continue;
    DIType *type = variable->getType();

    if (containsSEXP(type, visited, m)) {
      current = true;
      break;
    }
  }

  assert(current == original);
  return current;
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
