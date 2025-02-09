// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2022 Second State INC

#include "validator/formchecker.h"

#include "common/errinfo.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>

namespace WasmEdge {
namespace Validator {

namespace {

// Helper function for printing error log of index out of range.
auto logOutOfRange(ErrCode Code, ErrInfo::IndexCategory Cate, uint32_t Idx,
                   uint32_t Bound) {
  spdlog::error(Code);
  spdlog::error(ErrInfo::InfoForbidIndex(Cate, Idx, Bound));
  return Unexpect(Code);
}

} // namespace

void FormChecker::reset(bool CleanGlobal) {
  ValStack.clear();
  CtrlStack.clear();
  Locals.clear();
  Returns.clear();

  if (CleanGlobal) {
    Types.clear();
    Funcs.clear();
    Tables.clear();
    Mems = 0;
    Globals.clear();
    Datas.clear();
    Elems.clear();
    Refs.clear();
    NumImportFuncs = 0;
    NumImportGlobals = 0;
  }
}

Expect<void> FormChecker::validate(AST::InstrView Instrs,
                                   Span<const ValType> RetVals) {
  for (const ValType &Val : RetVals) {
    Returns.push_back(Val);
  }
  return checkExpr(Instrs);
}

Expect<void> FormChecker::validate(const ValType &VT) const noexcept {
  // The value type should be validated for the type index case.
  if (VT.isRefType() && VT.getHeapTypeCode() == TypeCode::TypeIndex) {
    if (VT.getTypeIndex() >= Types.size()) {
      spdlog::error(ErrCode::Value::InvalidFuncTypeIdx);
      spdlog::error(ErrInfo::InfoForbidIndex(
          ErrInfo::IndexCategory::FunctionType, VT.getTypeIndex(),
          static_cast<uint32_t>(Types.size())));
      return Unexpect(ErrCode::Value::InvalidFuncTypeIdx);
    }
  }
  return {};
}

void FormChecker::addType(const AST::FunctionType &Func) {
  std::vector<ValType> Param, Ret;
  Param.reserve(Func.getParamTypes().size());
  Ret.reserve(Func.getReturnTypes().size());
  for (auto Val : Func.getParamTypes()) {
    Param.push_back(Val);
  }
  for (auto Val : Func.getReturnTypes()) {
    Ret.push_back(Val);
  }
  Types.emplace_back(std::move(Param), std::move(Ret));
}

void FormChecker::addFunc(const uint32_t TypeIdx, const bool IsImport) {
  if (Types.size() > TypeIdx) {
    Funcs.emplace_back(TypeIdx);
  }
  if (IsImport) {
    NumImportFuncs++;
  }
}

void FormChecker::addTable(const AST::TableType &Tab) {
  Tables.push_back(Tab.getRefType());
}

void FormChecker::addMemory(const AST::MemoryType &) { Mems++; }

void FormChecker::addGlobal(const AST::GlobalType &Glob, const bool IsImport) {
  // Type in global is confirmed in loading phase.
  Globals.emplace_back(Glob.getValType(), Glob.getValMut());
  if (IsImport) {
    NumImportGlobals++;
  }
}

void FormChecker::addData(const AST::DataSegment &) {
  Datas.emplace_back(static_cast<uint32_t>(Datas.size()));
}

void FormChecker::addElem(const AST::ElementSegment &Elem) {
  Elems.emplace_back(Elem.getRefType());
}

void FormChecker::addRef(const uint32_t FuncIdx) { Refs.emplace(FuncIdx); }

void FormChecker::addLocal(const ValType &V, bool Initialized) {
  Locals.emplace_back(V);
  if (Initialized || V.isDefaultable()) {
    LocalInits.push_back(static_cast<uint32_t>(Locals.size() - 1));
    Locals.back().IsInit = true;
  }
}

ValType FormChecker::VTypeToAST(const VType &V) {
  if (!V) {
    return TypeCode::I32;
  }
  return *V;
}

bool FormChecker::matchType(const ValType &Exp,
                            const ValType &Got) const noexcept {
  if (!Exp.isRefType() && !Got.isRefType() && Exp.getCode() == Got.getCode()) {
    // Match for the non-reference type case.
    return true;
  }
  if (Exp.isRefType() && Got.isRefType()) {
    // Nullable matching.
    if (!Exp.isNullableRefType() && Got.isNullableRefType()) {
      return false;
    }

    // Match the heap type.
    if (Exp.getHeapTypeCode() == Got.getHeapTypeCode() &&
        Exp.getHeapTypeCode() != TypeCode::TypeIndex) {
      // Abs heap types are the same.
      return true;
    }
    if (Exp.getHeapTypeCode() == TypeCode::FuncRef &&
        Got.getHeapTypeCode() == TypeCode::TypeIndex) {
      // Match type index to any funcref.
      return true;
    }
    if (Exp.getHeapTypeCode() == TypeCode::TypeIndex &&
        Got.getHeapTypeCode() == TypeCode::TypeIndex) {
      // Match got type index to expected type index.
      if (matchTypes(Types[Exp.getTypeIndex()].first,
                     Types[Got.getTypeIndex()].first) &&
          matchTypes(Types[Exp.getTypeIndex()].second,
                     Types[Got.getTypeIndex()].second)) {
        // Note: In future versions of WebAssembly, subtyping on function types
        // may be relaxed to support co- and contra-variance.
        // Due to passing the validation of type section, this will not cause
        // infinite recursion.
        return true;
      }
    }
  }
  return false;
}

bool FormChecker::matchTypes(Span<const ValType> Exp,
                             Span<const ValType> Got) const noexcept {
  if (Exp.size() != Got.size()) {
    return false;
  }
  for (uint32_t I = 0; I < Exp.size(); I++) {
    if (!matchType(Exp[I], Got[I])) {
      return false;
    }
  }
  return true;
}

Expect<void> FormChecker::checkExpr(AST::InstrView Instrs) {
  if (Instrs.size() > 0) {
    // Push ctrl frame ([] -> [Returns])
    pushCtrl({}, Returns, &*Instrs.rbegin());
    return checkInstrs(Instrs);
  }
  return {};
}

Expect<void> FormChecker::checkInstrs(AST::InstrView Instrs) {
  // Validate instructions
  for (auto &Instr : Instrs) {
    if (auto Res = checkInstr(Instr); !Res) {
      spdlog::error(
          ErrInfo::InfoInstruction(Instr.getOpCode(), Instr.getOffset()));
      return Unexpect(Res);
    }
  }
  return {};
}
Expect<void> FormChecker::checkInstr(const AST::Instruction &Instr) {
  // Note: The instructions and their immediates have passed proposal
  // configuration checking in loader phase.

  // Helper lambda for checking and resolve the block type.
  auto checkBlockType = [this](std::vector<ValType> &Buffer,
                               const BlockType &BType)
      -> Expect<std::pair<Span<const ValType>, Span<const ValType>>> {
    using ReturnType = std::pair<Span<const ValType>, Span<const ValType>>;
    if (BType.isEmpty()) {
      // Empty case. t2* = none
      return ReturnType{{}, {}};
    } else if (BType.isValType()) {
      // ValType case. t2* = valtype
      if (auto Res = validate(BType.getValType()); !Res) {
        return Unexpect(Res);
      }
      Buffer[0] = BType.getValType();
      return ReturnType{{}, Buffer};
    } else {
      // Type index case. t2* = type[index].returns
      const uint32_t TypeIdx = BType.getTypeIndex();
      if (TypeIdx >= Types.size()) {
        return logOutOfRange(ErrCode::Value::InvalidFuncTypeIdx,
                             ErrInfo::IndexCategory::FunctionType, TypeIdx,
                             static_cast<uint32_t>(Types.size()));
      }
      return ReturnType{Types[TypeIdx].first, Types[TypeIdx].second};
    }
  };

  // Helper lambda for checking control stack depth and return index.
  auto checkCtrlStackDepth = [this](uint32_t N) -> Expect<uint32_t> {
    // Check the control stack for at least N + 1 frames.
    if (N >= CtrlStack.size()) {
      // Branch out of stack.
      return logOutOfRange(ErrCode::Value::InvalidLabelIdx,
                           ErrInfo::IndexCategory::Label, N,
                           static_cast<uint32_t>(CtrlStack.size()));
    }
    // Return the index of the last N element.
    return static_cast<uint32_t>(CtrlStack.size()) - UINT32_C(1) - N;
  };

  // Helper lambda for checking memory index and perform transformation.
  auto checkMemAndTrans = [this,
                           &Instr](Span<const ValType> Take,
                                   Span<const ValType> Put) -> Expect<void> {
    if (Instr.getTargetIndex() >= Mems) {
      return logOutOfRange(ErrCode::Value::InvalidMemoryIdx,
                           ErrInfo::IndexCategory::Memory,
                           Instr.getTargetIndex(), Mems);
    }
    return StackTrans(Take, Put);
  };

  // Helper lambda for checking lane index and perform transformation.
  auto checkLaneAndTrans = [this,
                            &Instr](uint32_t N, Span<const ValType> Take,
                                    Span<const ValType> Put) -> Expect<void> {
    if (Instr.getMemoryLane() >= N) {
      return logOutOfRange(ErrCode::Value::InvalidLaneIdx,
                           ErrInfo::IndexCategory::Lane, Instr.getMemoryLane(),
                           N);
    }
    return StackTrans(Take, Put);
  };

  // Helper lambda for checking memory alignment and perform transformation.
  auto checkAlignAndTrans = [this, checkLaneAndTrans,
                             &Instr](uint32_t N, Span<const ValType> Take,
                                     Span<const ValType> Put,
                                     bool CheckLane = false) -> Expect<void> {
    if (Instr.getTargetIndex() >= Mems) {
      return logOutOfRange(ErrCode::Value::InvalidMemoryIdx,
                           ErrInfo::IndexCategory::Memory,
                           Instr.getTargetIndex(), Mems);
    }
    if (Instr.getMemoryAlign() > 31 ||
        (1UL << Instr.getMemoryAlign()) > (N >> 3UL)) {
      // 2 ^ align needs to <= N / 8
      spdlog::error(ErrCode::Value::InvalidAlignment);
      spdlog::error(ErrInfo::InfoMismatch(static_cast<uint8_t>(N >> 3),
                                          Instr.getMemoryAlign()));
      return Unexpect(ErrCode::Value::InvalidAlignment);
    }
    if (CheckLane) {
      return checkLaneAndTrans(128 / N, Take, Put);
    }
    return StackTrans(Take, Put);
  };

  // Helper lambda for checking value types matching.
  auto checkTypesMatching = [this](Span<const ValType> Exp,
                                   Span<const ValType> Got) -> Expect<void> {
    if (!matchTypes(Exp, Got)) {
      std::vector<ValType> ExpV, GotV;
      ExpV.reserve(Exp.size());
      for (auto &I : Exp) {
        ExpV.push_back(I);
      }
      GotV.reserve(Got.size());
      for (auto &I : Got) {
        GotV.push_back(I);
      }
      spdlog::error(ErrCode::Value::TypeCheckFailed);
      spdlog::error(ErrInfo::InfoMismatch(ExpV, GotV));
      return Unexpect(ErrCode::Value::TypeCheckFailed);
    }
    return {};
  };

  switch (Instr.getOpCode()) {
  // Control instructions.
  case OpCode::Unreachable:
    return unreachable();
  case OpCode::Nop:
    return {};

  case OpCode::If:
  case OpCode::Block:
  case OpCode::Loop: {
    // Get blocktype [t1*] -> [t2*] and check valtype first.
    std::vector<ValType> Buffer(1);
    Span<const ValType> T1, T2;
    if (auto Res = checkBlockType(Buffer, Instr.getBlockType())) {
      std::tie(T1, T2) = std::move(*Res);
    } else {
      return Unexpect(Res);
    }
    // For the if instruction, pop I32 first.
    if (Instr.getOpCode() == OpCode::If) {
      if (auto Res = popType(TypeCode::I32); !Res) {
        return Unexpect(Res);
      }
    }
    // Pop and check [t1*]
    if (auto Res = popTypes(T1); !Res) {
      return Unexpect(Res);
    }
    // Push ctrl frame ([t1*], [t2*])
    const AST::Instruction *From = Instr.getOpCode() == OpCode::Loop
                                       ? &Instr
                                       : &Instr + Instr.getJumpEnd();
    pushCtrl(T1, T2, From, Instr.getOpCode());
    if (Instr.getOpCode() == OpCode::If &&
        Instr.getJumpElse() == Instr.getJumpEnd()) {
      // No else case in if-else statement.
      if (auto Res = checkTypesMatching(T2, T1); !Res) {
        return Unexpect(Res);
      }
    }
    return {};
  }

  case OpCode::Else:
    if (auto Res = popCtrl()) {
      pushCtrl((*Res).StartTypes, (*Res).EndTypes, Res->Jump,
               Instr.getOpCode());
    } else {
      return Unexpect(Res);
    }
    return {};
  case OpCode::End:
    if (auto Res = popCtrl()) {
      pushTypes((*Res).EndTypes);
    } else {
      return Unexpect(Res);
    }
    return {};

  case OpCode::Br:
    if (auto D = checkCtrlStackDepth(Instr.getJump().TargetIndex); !D) {
      return Unexpect(D);
    } else {
      // D is the last D element of control stack.
      const auto NTypes = getLabelTypes(CtrlStack[*D]);
      if (auto Res = popTypes(NTypes); !Res) {
        return Unexpect(Res);
      }
      const uint32_t Remain =
          static_cast<uint32_t>(ValStack.size() - CtrlStack[*D].Height);
      const uint32_t Arity = static_cast<uint32_t>(NTypes.size());
      auto &Jump = const_cast<AST::Instruction &>(Instr).getJump();
      Jump.StackEraseBegin = Remain + Arity;
      Jump.StackEraseEnd = Arity;
      Jump.PCOffset = static_cast<int32_t>(CtrlStack[*D].Jump - &Instr);
      return unreachable();
    }
  case OpCode::Br_if:
    if (auto D = checkCtrlStackDepth(Instr.getJump().TargetIndex); !D) {
      return Unexpect(D);
    } else {
      // D is the last D element of control stack.
      if (auto Res = popType(TypeCode::I32); !Res) {
        return Unexpect(Res);
      }
      const auto NTypes = getLabelTypes(CtrlStack[*D]);
      if (auto Res = popTypes(NTypes); !Res) {
        return Unexpect(Res);
      }
      const uint32_t Remain =
          static_cast<uint32_t>(ValStack.size() - CtrlStack[*D].Height);
      const uint32_t Arity = static_cast<uint32_t>(NTypes.size());
      auto &Jump = const_cast<AST::Instruction &>(Instr).getJump();
      Jump.StackEraseBegin = Remain + Arity;
      Jump.StackEraseEnd = Arity;
      Jump.PCOffset = static_cast<int32_t>(CtrlStack[*D].Jump - &Instr);
      pushTypes(NTypes);
      return {};
    }
  case OpCode::Br_table: {
    if (auto Res = popType(TypeCode::I32); !Res) {
      return Unexpect(Res);
    }
    auto LabelTable = const_cast<AST::Instruction &>(Instr).getLabelList();
    const auto LabelTableSize = static_cast<uint32_t>(LabelTable.size() - 1);
    if (auto M = checkCtrlStackDepth(LabelTable[LabelTableSize].TargetIndex)) {
      // M is the last M element of control stack.
      auto MTypes = getLabelTypes(CtrlStack[*M]);
      for (uint32_t LabelIdx = 0; LabelIdx < LabelTableSize; ++LabelIdx) {
        const uint32_t L = LabelTable[LabelIdx].TargetIndex;
        if (auto N = checkCtrlStackDepth(L)) {
          // N is the last N element of control stack.
          const auto NTypes = getLabelTypes(CtrlStack[*N]);
          if (MTypes.size() != NTypes.size()) {
            return checkTypesMatching(MTypes, NTypes);
          }
          // Push the popped types.
          std::vector<VType> TypeBuf(NTypes.size());
          for (uint32_t IdxN = static_cast<uint32_t>(NTypes.size()); IdxN >= 1;
               --IdxN) {
            const uint32_t Idx = IdxN - 1;
            // Cannot use popTypes() here because we need the popped value.
            if (auto Res = popType(NTypes[Idx])) {
              // Have to check is `unreachableVType` occurred for the case of
              // `unreachable` instruction appeared before the `br_table`
              // instruction.
              if (CtrlStack.back().IsUnreachable) {
                TypeBuf[Idx] = unreachableVType();
              } else {
                TypeBuf[Idx] = *Res;
              }
            } else {
              return Unexpect(Res);
            }
          }
          const uint32_t Remain =
              static_cast<uint32_t>(ValStack.size() - CtrlStack[*N].Height);
          const uint32_t Arity = static_cast<uint32_t>(NTypes.size());
          LabelTable[LabelIdx].StackEraseBegin = Remain + Arity;
          LabelTable[LabelIdx].StackEraseEnd = Arity;
          LabelTable[LabelIdx].PCOffset =
              static_cast<int32_t>(CtrlStack[*N].Jump - &Instr);
          pushTypes(TypeBuf);
        } else {
          return Unexpect(N);
        }
      }
      const auto NTypes = getLabelTypes(CtrlStack[*M]);
      if (auto Res = popTypes(NTypes); !Res) {
        return Unexpect(Res);
      }
      const uint32_t Remain =
          static_cast<uint32_t>(ValStack.size() - CtrlStack[*M].Height);
      const uint32_t Arity = static_cast<uint32_t>(NTypes.size());
      LabelTable[LabelTableSize].StackEraseBegin = Remain + Arity;
      LabelTable[LabelTableSize].StackEraseEnd = Arity;
      LabelTable[LabelTableSize].PCOffset =
          static_cast<int32_t>(CtrlStack[*M].Jump - &Instr);
      return unreachable();
    } else {
      return Unexpect(M);
    }
  }

  case OpCode::Br_on_null:
    // D is the last D element of control stack.
    if (auto D = checkCtrlStackDepth(Instr.getTargetIndex())) {
      const auto NTypes = getLabelTypes(CtrlStack[*D]);
      if (auto ResT = popType()) {
        if (*ResT == unreachableVType()) {
          // will not reach here. Validation succeeds.
          return {};
        }
        if (!(*ResT)->isRefType()) {
          spdlog::error(ErrCode::Value::InvalidBrRefType);
          return Unexpect(ErrCode::ErrCode::Value::InvalidBrRefType);
        }
        if (auto Res = popTypes(NTypes); !Res) {
          return Unexpect(Res);
        }
        const uint32_t Remain =
            static_cast<uint32_t>(ValStack.size() - CtrlStack[*D].Height);
        const uint32_t Arity = static_cast<uint32_t>(NTypes.size());
        auto &Jump = const_cast<AST::Instruction &>(Instr).getJump();
        Jump.StackEraseBegin = Remain + Arity;
        Jump.StackEraseEnd = Arity;
        Jump.PCOffset = static_cast<int32_t>(CtrlStack[*D].Jump - &Instr);
        pushTypes(NTypes);
        pushType(ValType(TypeCode::Ref, (*ResT)->getHeapTypeCode(),
                         (*ResT)->getTypeIndex()));
        return {};
      } else {
        return Unexpect(ResT);
      }
    } else {
      return Unexpect(D);
    }
  case OpCode::Br_on_non_null:
    if (auto D = checkCtrlStackDepth(Instr.getTargetIndex())) {
      auto LabelTypes = getLabelTypes(CtrlStack[*D]);
      std::vector<ValType> NTypes(LabelTypes.begin(), LabelTypes.end());
      if (NTypes.empty()) {
        spdlog::error(ErrCode::Value::InvalidBrRefType);
        return Unexpect(ErrCode::Value::InvalidBrRefType);
      }
      ValType RType = NTypes.back();
      NTypes.pop_back();
      if (!RType.isRefType() || RType.isNullableRefType()) {
        spdlog::error(ErrCode::Value::InvalidBrRefType);
        return Unexpect(ErrCode::Value::InvalidBrRefType);
      }
      if (auto Res = popType(ValType(TypeCode::RefNull, RType.getHeapTypeCode(),
                                     RType.getTypeIndex()));
          !Res) {
        spdlog::error(ErrCode::Value::InvalidBrRefType);
        return Unexpect(ErrCode::Value::InvalidBrRefType);
      }
      if (auto Res = popTypes(NTypes); !Res) {
        spdlog::error(ErrCode::Value::InvalidBrRefType);
        return Unexpect(ErrCode::Value::InvalidBrRefType);
      }
      const uint32_t Remain =
          static_cast<uint32_t>(ValStack.size() - CtrlStack[*D].Height);
      const uint32_t Arity = static_cast<uint32_t>(NTypes.size() + 1);
      // We plus 1 here because we did `pop_back` on `NTypes`
      auto &Jump = const_cast<AST::Instruction &>(Instr).getJump();
      Jump.StackEraseBegin = Remain + Arity;
      Jump.StackEraseEnd = Arity;
      Jump.PCOffset = static_cast<int32_t>(CtrlStack[*D].Jump - &Instr);
      pushTypes(NTypes);
      return {};
    } else {
      return Unexpect(D);
    }

  case OpCode::Return:
    if (auto Res = popTypes(Returns); !Res) {
      return Unexpect(Res);
    }
    return unreachable();

  case OpCode::Call: {
    auto N = Instr.getTargetIndex();
    if (N >= Funcs.size()) {
      return logOutOfRange(ErrCode::Value::InvalidFuncIdx,
                           ErrInfo::IndexCategory::Function, N,
                           static_cast<uint32_t>(Funcs.size()));
    }
    return StackTrans(Types[Funcs[N]].first, Types[Funcs[N]].second);
  }
  case OpCode::Call_indirect: {
    auto N = Instr.getTargetIndex();
    auto T = Instr.getSourceIndex();
    // Check source table index.
    if (T >= Tables.size()) {
      return logOutOfRange(ErrCode::Value::InvalidTableIdx,
                           ErrInfo::IndexCategory::Table, T,
                           static_cast<uint32_t>(Tables.size()));
    }
    if (!Tables[T].isFuncRefType()) {
      spdlog::error(ErrCode::Value::InvalidTableIdx);
      return Unexpect(ErrCode::Value::InvalidTableIdx);
    }
    // Check target function type index.
    if (N >= Types.size()) {
      return logOutOfRange(ErrCode::Value::InvalidFuncTypeIdx,
                           ErrInfo::IndexCategory::FunctionType, N,
                           static_cast<uint32_t>(Types.size()));
    }
    if (auto Res = popType(TypeCode::I32); !Res) {
      return Unexpect(Res);
    }
    return StackTrans(Types[N].first, Types[N].second);
  }
  case OpCode::Return_call: {
    auto N = Instr.getTargetIndex();
    if (Funcs.size() <= N) {
      // Call function index out of range
      spdlog::error(ErrCode::Value::InvalidFuncIdx);
      spdlog::error(
          ErrInfo::InfoForbidIndex(ErrInfo::IndexCategory::Function, N,
                                   static_cast<uint32_t>(Funcs.size())));
      return Unexpect(ErrCode::Value::InvalidFuncIdx);
    }
    if (!matchTypes(Returns, Types[Funcs[N]].second)) {
      spdlog::error(ErrCode::Value::TypeCheckFailed);
      spdlog::error(ErrInfo::InfoMismatch(Returns, Types[Funcs[N]].second));
      return Unexpect(ErrCode::Value::TypeCheckFailed);
    }
    if (auto Res = popTypes(Types[Funcs[N]].first); !Res) {
      return Unexpect(Res);
    }
    return unreachable();
  }
  case OpCode::Return_call_indirect: {
    auto N = Instr.getTargetIndex();
    auto T = Instr.getSourceIndex();
    // Check source table index.
    if (Tables.size() <= T) {
      spdlog::error(ErrCode::Value::InvalidTableIdx);
      spdlog::error(
          ErrInfo::InfoForbidIndex(ErrInfo::IndexCategory::Table, T,
                                   static_cast<uint32_t>(Tables.size())));
      return Unexpect(ErrCode::Value::InvalidTableIdx);
    }
    if (!Tables[T].isFuncRefType()) {
      spdlog::error(ErrCode::Value::InvalidTableIdx);
      return Unexpect(ErrCode::Value::InvalidTableIdx);
    }
    // Check target function type index.
    if (Types.size() <= N) {
      spdlog::error(ErrCode::Value::InvalidFuncTypeIdx);
      spdlog::error(
          ErrInfo::InfoForbidIndex(ErrInfo::IndexCategory::FunctionType, N,
                                   static_cast<uint32_t>(Types.size())));
      return Unexpect(ErrCode::Value::InvalidFuncTypeIdx);
    }
    if (!matchTypes(Returns, Types[N].second)) {
      spdlog::error(ErrCode::Value::TypeCheckFailed);
      spdlog::error(ErrInfo::InfoMismatch(Returns, Types[N].second));
      return Unexpect(ErrCode::Value::TypeCheckFailed);
    }
    if (auto Res = popType(TypeCode::I32); !Res) {
      return Unexpect(Res);
    }
    if (auto Res = popTypes(Types[N].first); !Res) {
      return Unexpect(Res);
    }
    return unreachable();
  }
  case OpCode::Call_ref: {
    auto TypeIdx = Instr.getTargetIndex();
    if (TypeIdx >= static_cast<uint32_t>(Types.size())) {
      return logOutOfRange(ErrCode::Value::InvalidFuncTypeIdx,
                           ErrInfo::IndexCategory::FunctionType, TypeIdx,
                           static_cast<uint32_t>(Types.size()));
    }
    std::vector<ValType> Input = Types[TypeIdx].first;
    Input.push_back(ValType(TypeCode::RefNull, TypeIdx));
    return StackTrans(Input, Types[TypeIdx].second);
  }
  case OpCode::Return_call_ref: {
    auto TypeIdx = Instr.getTargetIndex();
    if (TypeIdx >= Types.size()) {
      // Call function index out of range
      spdlog::error(ErrCode::Value::InvalidFuncIdx);
      spdlog::error(ErrInfo::InfoForbidIndex(
          ErrInfo::IndexCategory::FunctionType, TypeIdx,
          static_cast<uint32_t>(Types.size())));
      return Unexpect(ErrCode::Value::InvalidFuncIdx);
    }
    if (!matchTypes(Returns, Types[TypeIdx].second)) {
      spdlog::error(ErrCode::Value::TypeCheckFailed);
      spdlog::error(ErrInfo::InfoMismatch(Returns, Types[TypeIdx].second));
      return Unexpect(ErrCode::Value::TypeCheckFailed);
    }
    std::vector<ValType> Input = Types[TypeIdx].first;
    Input.push_back(ValType(TypeCode::RefNull, TypeIdx));
    if (auto Res = popTypes(Input); !Res) {
      return Unexpect(Res);
    }
    return unreachable();
  }

  // Reference Instructions.
  case OpCode::Ref__null:
    if (auto Res = validate(Instr.getValType())) {
      return StackTrans({}, {Instr.getValType()});
    } else {
      return Unexpect(Res);
    }
  case OpCode::Ref__is_null:
    if (auto Res = popType()) {
      if (!isRefType(*Res)) {
        spdlog::error(ErrCode::Value::TypeCheckFailed);
        spdlog::error(
            ErrInfo::InfoMismatch(TypeCode::FuncRef, VTypeToAST(*Res)));
        return Unexpect(ErrCode::Value::TypeCheckFailed);
      }
    } else {
      return Unexpect(Res);
    }
    return StackTrans({}, {ValType(TypeCode::I32)});
  case OpCode::Ref__func: {
    auto FuncIdx = Instr.getTargetIndex();
    if (Refs.find(FuncIdx) == Refs.cend()) {
      // Undeclared function reference.
      spdlog::error(ErrCode::Value::InvalidRefIdx);
      return Unexpect(ErrCode::Value::InvalidRefIdx);
    }
    assuming(FuncIdx < Funcs.size());
    auto TypeIdx = Funcs[FuncIdx];
    assuming(TypeIdx < Types.size());
    return StackTrans({}, {ValType(TypeCode::Ref, TypeIdx)});
  }
  case OpCode::Ref__as_non_null: {
    if (auto Res = popType()) {
      if (*Res == unreachableVType()) {
        pushType(unreachableVType());
        return {};
      }
      if (!(*Res)->isRefType()) {
        spdlog::error(ErrCode::Value::TypeCheckFailed);
        spdlog::error(ErrInfo::InfoMismatch(
            ValType(TypeCode::RefNull, TypeCode::FuncRef), VTypeToAST(*Res)));
        return Unexpect(ErrCode::Value::TypeCheckFailed);
      }
      return StackTrans({}, {ValType(TypeCode::Ref, (*Res)->getHeapTypeCode(),
                                     (*Res)->getTypeIndex())});
    } else {
      return Unexpect(Res);
    }
  }
  // Parametric Instructions.
  case OpCode::Drop:
    return StackPopAny();
  case OpCode::Select: {
    // Pop I32.
    if (auto Res = popType(TypeCode::I32); !Res) {
      return Unexpect(Res);
    }
    // Pop T1 and T2.
    VType T1, T2;
    if (auto Res = popType()) {
      T1 = *Res;
    } else {
      return Unexpect(Res);
    }
    if (auto Res = popType()) {
      T2 = *Res;
    } else {
      return Unexpect(Res);
    }
    // T1 and T2 should be number type.
    if (!isNumType(T1)) {
      spdlog::error(ErrCode::Value::TypeCheckFailed);
      spdlog::error(ErrInfo::InfoMismatch(TypeCode::I32, VTypeToAST(T1)));
      return Unexpect(ErrCode::Value::TypeCheckFailed);
    }
    if (!isNumType(T2)) {
      spdlog::error(ErrCode::Value::TypeCheckFailed);
      spdlog::error(ErrInfo::InfoMismatch(VTypeToAST(T1), VTypeToAST(T2)));
      return Unexpect(ErrCode::Value::TypeCheckFailed);
    }
    // Error if t1 != t2 && t1 =/= Unknown && t2 =/= Unknown
    if (T1 != T2 && T1 != unreachableVType() && T2 != unreachableVType()) {
      spdlog::error(ErrCode::Value::TypeCheckFailed);
      spdlog::error(ErrInfo::InfoMismatch(VTypeToAST(T1), VTypeToAST(T2)));
      return Unexpect(ErrCode::Value::TypeCheckFailed);
    }
    // Push value.
    if (T1 == unreachableVType()) {
      pushType(T2);
    } else {
      pushType(T1);
    }
    return {};
  }
  case OpCode::Select_t: {
    // Note: There may be multiple values choice in the future.
    if (Instr.getValTypeList().size() != 1) {
      spdlog::error(ErrCode::Value::InvalidResultArity);
      return Unexpect(ErrCode::Value::InvalidResultArity);
    }
    ValType ExpT = Instr.getValTypeList()[0];
    if (auto Res = validate(ExpT); !Res) {
      return Unexpect(Res);
    }
    if (auto Res = popTypes({ExpT, ExpT, ValType(TypeCode::I32)}); !Res) {
      return Unexpect(Res);
    }
    pushType(ExpT);
    return {};
  }

  // Variable Instructions.
  case OpCode::Local__get:
  case OpCode::Local__set:
  case OpCode::Local__tee: {
    if (Instr.getTargetIndex() >= Locals.size()) {
      return logOutOfRange(
          ErrCode::Value::InvalidLocalIdx, ErrInfo::IndexCategory::Local,
          Instr.getTargetIndex(), static_cast<uint32_t>(Locals.size()));
    }
    auto &TExpect = Locals[Instr.getTargetIndex()];
    const_cast<AST::Instruction &>(Instr).getStackOffset() =
        static_cast<uint32_t>(ValStack.size() +
                              (Locals.size() - Instr.getTargetIndex()));
    if (Instr.getOpCode() == OpCode::Local__get) {
      if (!TExpect.IsInit) {
        spdlog::error(ErrCode::Value::InvalidUninitLocal);
        return Unexpect(ErrCode::Value::InvalidUninitLocal);
      }
      return StackTrans({}, {TExpect.VType});
    } else if (Instr.getOpCode() == OpCode::Local__set) {
      if (!TExpect.IsInit) {
        TExpect.IsInit = true;
        LocalInits.push_back(Instr.getTargetIndex());
      }
      return StackTrans({TExpect.VType}, {});
    } else if (Instr.getOpCode() == OpCode::Local__tee) {
      if (!TExpect.IsInit) {
        TExpect.IsInit = true;
        LocalInits.push_back(Instr.getTargetIndex());
      }
      return StackTrans({TExpect.VType}, {TExpect.VType});
    } else {
      assumingUnreachable();
    }
  }
  case OpCode::Global__set:
    // Global case, check mutation.
    if (Instr.getTargetIndex() < Globals.size() &&
        Globals[Instr.getTargetIndex()].second != ValMut::Var) {
      // Global is immutable
      spdlog::error(ErrCode::Value::ImmutableGlobal);
      return Unexpect(ErrCode::Value::ImmutableGlobal);
    }
    [[fallthrough]];
  case OpCode::Global__get: {
    if (Instr.getTargetIndex() >= Globals.size()) {
      return logOutOfRange(
          ErrCode::Value::InvalidGlobalIdx, ErrInfo::IndexCategory::Global,
          Instr.getTargetIndex(), static_cast<uint32_t>(Globals.size()));
    }
    ValType ExpT = Globals[Instr.getTargetIndex()].first;
    if (Instr.getOpCode() == OpCode::Global__set) {
      return StackTrans({ExpT}, {});
    } else {
      return StackTrans({}, {ExpT});
    }
  }

  // Table Instructions.
  case OpCode::Table__get:
  case OpCode::Table__set:
  case OpCode::Table__grow:
  case OpCode::Table__size:
  case OpCode::Table__fill:
  case OpCode::Table__init:
  case OpCode::Table__copy: {
    // Check target table index to perform.
    if (Instr.getTargetIndex() >= Tables.size()) {
      return logOutOfRange(
          ErrCode::Value::InvalidTableIdx, ErrInfo::IndexCategory::Table,
          Instr.getTargetIndex(), static_cast<uint32_t>(Tables.size()));
    }
    ValType ExpT = Tables[Instr.getTargetIndex()];
    if (Instr.getOpCode() == OpCode::Table__get) {
      return StackTrans({ValType(TypeCode::I32)}, {ExpT});
    } else if (Instr.getOpCode() == OpCode::Table__set) {
      return StackTrans({ValType(TypeCode::I32), ExpT}, {});
    } else if (Instr.getOpCode() == OpCode::Table__grow) {
      return StackTrans({ExpT, ValType(TypeCode::I32)},
                        {ValType(TypeCode::I32)});
    } else if (Instr.getOpCode() == OpCode::Table__size) {
      return StackTrans({}, {ValType(TypeCode::I32)});
    } else if (Instr.getOpCode() == OpCode::Table__fill) {
      return StackTrans({ValType(TypeCode::I32), ExpT, ValType(TypeCode::I32)},
                        {});
    } else if (Instr.getOpCode() == OpCode::Table__init) {
      // Check source element index for initialization.
      if (Instr.getSourceIndex() >= Elems.size()) {
        return logOutOfRange(
            ErrCode::Value::InvalidElemIdx, ErrInfo::IndexCategory::Element,
            Instr.getSourceIndex(), static_cast<uint32_t>(Elems.size()));
      }
      // Check is the reference types matched.
      if (!matchType(Tables[Instr.getTargetIndex()],
                     Elems[Instr.getSourceIndex()])) {
        spdlog::error(ErrCode::Value::TypeCheckFailed);
        spdlog::error(ErrInfo::InfoMismatch(Tables[Instr.getTargetIndex()],
                                            Elems[Instr.getSourceIndex()]));
        return Unexpect(ErrCode::Value::TypeCheckFailed);
      }
      return StackTrans({ValType(TypeCode::I32), ValType(TypeCode::I32),
                         ValType(TypeCode::I32)},
                        {});
    } else if (Instr.getOpCode() == OpCode::Table__copy) {
      // Check source table index for copying.
      if (Instr.getSourceIndex() >= Tables.size()) {
        return logOutOfRange(
            ErrCode::Value::InvalidTableIdx, ErrInfo::IndexCategory::Table,
            Instr.getSourceIndex(), static_cast<uint32_t>(Tables.size()));
      }
      // Check is the reference types matched.
      if (!matchType(Tables[Instr.getTargetIndex()],
                     Tables[Instr.getSourceIndex()])) {
        spdlog::error(ErrCode::Value::TypeCheckFailed);
        spdlog::error(ErrInfo::InfoMismatch(Tables[Instr.getTargetIndex()],
                                            Tables[Instr.getSourceIndex()]));
        return Unexpect(ErrCode::Value::TypeCheckFailed);
      }
      return StackTrans({ValType(TypeCode::I32), ValType(TypeCode::I32),
                         ValType(TypeCode::I32)},
                        {});
    } else {
      assumingUnreachable();
    }
  }
  case OpCode::Elem__drop:
    // Check target element index to drop.
    if (Instr.getTargetIndex() >= Elems.size()) {
      return logOutOfRange(
          ErrCode::Value::InvalidElemIdx, ErrInfo::IndexCategory::Element,
          Instr.getTargetIndex(), static_cast<uint32_t>(Elems.size()));
    }
    return {};

  // Memory Instructions.
  case OpCode::I32__load:
    return checkAlignAndTrans(32, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::I32)});
  case OpCode::I64__load:
    return checkAlignAndTrans(64, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::I64)});
  case OpCode::F32__load:
    return checkAlignAndTrans(32, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::F32)});
  case OpCode::F64__load:
    return checkAlignAndTrans(64, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::F64)});
  case OpCode::I32__load8_s:
  case OpCode::I32__load8_u:
    return checkAlignAndTrans(8, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::I32)});
  case OpCode::I32__load16_s:
  case OpCode::I32__load16_u:
    return checkAlignAndTrans(16, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::I32)});
  case OpCode::I64__load8_s:
  case OpCode::I64__load8_u:
    return checkAlignAndTrans(8, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::I64)});
  case OpCode::I64__load16_s:
  case OpCode::I64__load16_u:
    return checkAlignAndTrans(16, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::I64)});
  case OpCode::I64__load32_s:
  case OpCode::I64__load32_u:
    return checkAlignAndTrans(32, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::I64)});
  case OpCode::I32__store:
    return checkAlignAndTrans(
        32, {ValType(TypeCode::I32), ValType(TypeCode::I32)}, {});
  case OpCode::I64__store:
    return checkAlignAndTrans(
        64, {ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  case OpCode::F32__store:
    return checkAlignAndTrans(
        32, {ValType(TypeCode::I32), ValType(TypeCode::F32)}, {});
  case OpCode::F64__store:
    return checkAlignAndTrans(
        64, {ValType(TypeCode::I32), ValType(TypeCode::F64)}, {});
  case OpCode::I32__store8:
    return checkAlignAndTrans(
        8, {ValType(TypeCode::I32), ValType(TypeCode::I32)}, {});
  case OpCode::I32__store16:
    return checkAlignAndTrans(
        16, {ValType(TypeCode::I32), ValType(TypeCode::I32)}, {});
  case OpCode::I64__store8:
    return checkAlignAndTrans(
        8, {ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  case OpCode::I64__store16:
    return checkAlignAndTrans(
        16, {ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  case OpCode::I64__store32:
    return checkAlignAndTrans(
        32, {ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  case OpCode::Memory__size:
    return checkMemAndTrans({}, {ValType(TypeCode::I32)});
  case OpCode::Memory__grow:
    return checkMemAndTrans({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  case OpCode::Memory__init:
    // Check the target memory index. Memory index should be checked first.
    if (Instr.getTargetIndex() >= Mems) {
      return logOutOfRange(ErrCode::Value::InvalidMemoryIdx,
                           ErrInfo::IndexCategory::Memory,
                           Instr.getTargetIndex(), Mems);
    }
    // Check the source data index.
    if (Instr.getSourceIndex() >= Datas.size()) {
      return logOutOfRange(ErrCode::Value::InvalidDataIdx,
                           ErrInfo::IndexCategory::Data, Instr.getSourceIndex(),
                           static_cast<uint32_t>(Datas.size()));
    }
    return StackTrans({ValType(TypeCode::I32), ValType(TypeCode::I32),
                       ValType(TypeCode::I32)},
                      {});
  case OpCode::Memory__copy:
    /// Check the source memory index.
    if (Instr.getSourceIndex() >= Mems) {
      return logOutOfRange(ErrCode::Value::InvalidMemoryIdx,
                           ErrInfo::IndexCategory::Memory,
                           Instr.getSourceIndex(), Mems);
    }
    [[fallthrough]];
  case OpCode::Memory__fill:
    return checkMemAndTrans({ValType(TypeCode::I32), ValType(TypeCode::I32),
                             ValType(TypeCode::I32)},
                            {});
  case OpCode::Data__drop:
    // Check the target data index.
    if (Instr.getTargetIndex() >= Datas.size()) {
      return logOutOfRange(ErrCode::Value::InvalidDataIdx,
                           ErrInfo::IndexCategory::Data, Instr.getTargetIndex(),
                           static_cast<uint32_t>(Datas.size()));
    }
    return {};

  // Const Instructions.
  case OpCode::I32__const:
    return StackTrans({}, {ValType(TypeCode::I32)});
  case OpCode::I64__const:
    return StackTrans({}, {ValType(TypeCode::I64)});
  case OpCode::F32__const:
    return StackTrans({}, {ValType(TypeCode::F32)});
  case OpCode::F64__const:
    return StackTrans({}, {ValType(TypeCode::F64)});

  // Unary Numeric Instructions.
  case OpCode::I32__eqz:
    return StackTrans({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  case OpCode::I64__eqz:
    return StackTrans({ValType(TypeCode::I64)}, {ValType(TypeCode::I32)});
  case OpCode::I32__clz:
  case OpCode::I32__ctz:
  case OpCode::I32__popcnt:
    return StackTrans({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  case OpCode::I64__clz:
  case OpCode::I64__ctz:
  case OpCode::I64__popcnt:
    return StackTrans({ValType(TypeCode::I64)}, {ValType(TypeCode::I64)});
  case OpCode::F32__abs:
  case OpCode::F32__neg:
  case OpCode::F32__ceil:
  case OpCode::F32__floor:
  case OpCode::F32__trunc:
  case OpCode::F32__nearest:
  case OpCode::F32__sqrt:
    return StackTrans({ValType(TypeCode::F32)}, {ValType(TypeCode::F32)});
  case OpCode::F64__abs:
  case OpCode::F64__neg:
  case OpCode::F64__ceil:
  case OpCode::F64__floor:
  case OpCode::F64__trunc:
  case OpCode::F64__nearest:
  case OpCode::F64__sqrt:
    return StackTrans({ValType(TypeCode::F64)}, {ValType(TypeCode::F64)});
  case OpCode::I32__wrap_i64:
    return StackTrans({ValType(TypeCode::I64)}, {ValType(TypeCode::I32)});
  case OpCode::I32__trunc_f32_s:
  case OpCode::I32__trunc_f32_u:
    return StackTrans({ValType(TypeCode::F32)}, {ValType(TypeCode::I32)});
  case OpCode::I32__trunc_f64_s:
  case OpCode::I32__trunc_f64_u:
    return StackTrans({ValType(TypeCode::F64)}, {ValType(TypeCode::I32)});
  case OpCode::I64__extend_i32_s:
  case OpCode::I64__extend_i32_u:
    return StackTrans({ValType(TypeCode::I32)}, {ValType(TypeCode::I64)});
  case OpCode::I64__trunc_f32_s:
  case OpCode::I64__trunc_f32_u:
    return StackTrans({ValType(TypeCode::F32)}, {ValType(TypeCode::I64)});
  case OpCode::I64__trunc_f64_s:
  case OpCode::I64__trunc_f64_u:
    return StackTrans({ValType(TypeCode::F64)}, {ValType(TypeCode::I64)});
  case OpCode::F32__convert_i32_s:
  case OpCode::F32__convert_i32_u:
    return StackTrans({ValType(TypeCode::I32)}, {ValType(TypeCode::F32)});
  case OpCode::F32__convert_i64_s:
  case OpCode::F32__convert_i64_u:
    return StackTrans({ValType(TypeCode::I64)}, {ValType(TypeCode::F32)});
  case OpCode::F32__demote_f64:
    return StackTrans({ValType(TypeCode::F64)}, {ValType(TypeCode::F32)});
  case OpCode::F64__convert_i32_s:
  case OpCode::F64__convert_i32_u:
    return StackTrans({ValType(TypeCode::I32)}, {ValType(TypeCode::F64)});
  case OpCode::F64__convert_i64_s:
  case OpCode::F64__convert_i64_u:
    return StackTrans({ValType(TypeCode::I64)}, {ValType(TypeCode::F64)});
  case OpCode::F64__promote_f32:
    return StackTrans({ValType(TypeCode::F32)}, {ValType(TypeCode::F64)});
  case OpCode::I32__reinterpret_f32:
    return StackTrans({ValType(TypeCode::F32)}, {ValType(TypeCode::I32)});
  case OpCode::I64__reinterpret_f64:
    return StackTrans({ValType(TypeCode::F64)}, {ValType(TypeCode::I64)});
  case OpCode::F32__reinterpret_i32:
    return StackTrans({ValType(TypeCode::I32)}, {ValType(TypeCode::F32)});
  case OpCode::F64__reinterpret_i64:
    return StackTrans({ValType(TypeCode::I64)}, {ValType(TypeCode::F64)});
  case OpCode::I32__extend8_s:
  case OpCode::I32__extend16_s:
    return StackTrans({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  case OpCode::I64__extend8_s:
  case OpCode::I64__extend16_s:
  case OpCode::I64__extend32_s:
    return StackTrans({ValType(TypeCode::I64)}, {ValType(TypeCode::I64)});
  case OpCode::I32__trunc_sat_f32_s:
  case OpCode::I32__trunc_sat_f32_u:
    return StackTrans({ValType(TypeCode::F32)}, {ValType(TypeCode::I32)});
  case OpCode::I32__trunc_sat_f64_s:
  case OpCode::I32__trunc_sat_f64_u:
    return StackTrans({ValType(TypeCode::F64)}, {ValType(TypeCode::I32)});
  case OpCode::I64__trunc_sat_f32_s:
  case OpCode::I64__trunc_sat_f32_u:
    return StackTrans({ValType(TypeCode::F32)}, {ValType(TypeCode::I64)});
  case OpCode::I64__trunc_sat_f64_s:
  case OpCode::I64__trunc_sat_f64_u:
    return StackTrans({ValType(TypeCode::F64)}, {ValType(TypeCode::I64)});

  // Binary Numeric Instructions.
  case OpCode::I32__eq:
  case OpCode::I32__ne:
  case OpCode::I32__lt_s:
  case OpCode::I32__lt_u:
  case OpCode::I32__gt_s:
  case OpCode::I32__gt_u:
  case OpCode::I32__le_s:
  case OpCode::I32__le_u:
  case OpCode::I32__ge_s:
  case OpCode::I32__ge_u:
    return StackTrans({ValType(TypeCode::I32), ValType(TypeCode::I32)},
                      {ValType(TypeCode::I32)});
  case OpCode::I64__eq:
  case OpCode::I64__ne:
  case OpCode::I64__lt_s:
  case OpCode::I64__lt_u:
  case OpCode::I64__gt_s:
  case OpCode::I64__gt_u:
  case OpCode::I64__le_s:
  case OpCode::I64__le_u:
  case OpCode::I64__ge_s:
  case OpCode::I64__ge_u:
    return StackTrans({ValType(TypeCode::I64), ValType(TypeCode::I64)},
                      {ValType(TypeCode::I32)});
  case OpCode::F32__eq:
  case OpCode::F32__ne:
  case OpCode::F32__lt:
  case OpCode::F32__gt:
  case OpCode::F32__le:
  case OpCode::F32__ge:
    return StackTrans({ValType(TypeCode::F32), ValType(TypeCode::F32)},
                      {ValType(TypeCode::I32)});
  case OpCode::F64__eq:
  case OpCode::F64__ne:
  case OpCode::F64__lt:
  case OpCode::F64__gt:
  case OpCode::F64__le:
  case OpCode::F64__ge:
    return StackTrans({ValType(TypeCode::F64), ValType(TypeCode::F64)},
                      {ValType(TypeCode::I32)});
  case OpCode::I32__add:
  case OpCode::I32__sub:
  case OpCode::I32__mul:
  case OpCode::I32__div_s:
  case OpCode::I32__div_u:
  case OpCode::I32__rem_s:
  case OpCode::I32__rem_u:
  case OpCode::I32__and:
  case OpCode::I32__or:
  case OpCode::I32__xor:
  case OpCode::I32__shl:
  case OpCode::I32__shr_s:
  case OpCode::I32__shr_u:
  case OpCode::I32__rotl:
  case OpCode::I32__rotr:
    return StackTrans({ValType(TypeCode::I32), ValType(TypeCode::I32)},
                      {ValType(TypeCode::I32)});
  case OpCode::I64__add:
  case OpCode::I64__sub:
  case OpCode::I64__mul:
  case OpCode::I64__div_s:
  case OpCode::I64__div_u:
  case OpCode::I64__rem_s:
  case OpCode::I64__rem_u:
  case OpCode::I64__and:
  case OpCode::I64__or:
  case OpCode::I64__xor:
  case OpCode::I64__shl:
  case OpCode::I64__shr_s:
  case OpCode::I64__shr_u:
  case OpCode::I64__rotl:
  case OpCode::I64__rotr:
    return StackTrans({ValType(TypeCode::I64), ValType(TypeCode::I64)},
                      {ValType(TypeCode::I64)});
  case OpCode::F32__add:
  case OpCode::F32__sub:
  case OpCode::F32__mul:
  case OpCode::F32__div:
  case OpCode::F32__min:
  case OpCode::F32__max:
  case OpCode::F32__copysign:
    return StackTrans({ValType(TypeCode::F32), ValType(TypeCode::F32)},
                      {ValType(TypeCode::F32)});
  case OpCode::F64__add:
  case OpCode::F64__sub:
  case OpCode::F64__mul:
  case OpCode::F64__div:
  case OpCode::F64__min:
  case OpCode::F64__max:
  case OpCode::F64__copysign:
    return StackTrans({ValType(TypeCode::F64), ValType(TypeCode::F64)},
                      {ValType(TypeCode::F64)});

  // SIMD Memory Instruction.
  case OpCode::V128__load:
    return checkAlignAndTrans(128, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::V128)});
  case OpCode::V128__load8x8_s:
  case OpCode::V128__load8x8_u:
  case OpCode::V128__load16x4_s:
  case OpCode::V128__load16x4_u:
  case OpCode::V128__load32x2_s:
  case OpCode::V128__load32x2_u:
  case OpCode::V128__load64_splat:
  case OpCode::V128__load64_zero:
    return checkAlignAndTrans(64, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::V128)});
  case OpCode::V128__load8_splat:
    return checkAlignAndTrans(8, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::V128)});
  case OpCode::V128__load16_splat:
    return checkAlignAndTrans(16, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::V128)});
  case OpCode::V128__load32_splat:
  case OpCode::V128__load32_zero:
    return checkAlignAndTrans(32, {ValType(TypeCode::I32)},
                              {ValType(TypeCode::V128)});
  case OpCode::V128__store:
    return checkAlignAndTrans(
        128, {ValType(TypeCode::I32), ValType(TypeCode::V128)}, {});
  case OpCode::V128__load8_lane:
    return checkAlignAndTrans(8,
                              {ValType(TypeCode::I32), ValType(TypeCode::V128)},
                              {ValType(TypeCode::V128)}, true);
  case OpCode::V128__load16_lane:
    return checkAlignAndTrans(16,
                              {ValType(TypeCode::I32), ValType(TypeCode::V128)},
                              {ValType(TypeCode::V128)}, true);
  case OpCode::V128__load32_lane:
    return checkAlignAndTrans(32,
                              {ValType(TypeCode::I32), ValType(TypeCode::V128)},
                              {ValType(TypeCode::V128)}, true);
  case OpCode::V128__load64_lane:
    return checkAlignAndTrans(64,
                              {ValType(TypeCode::I32), ValType(TypeCode::V128)},
                              {ValType(TypeCode::V128)}, true);
  case OpCode::V128__store8_lane:
    return checkAlignAndTrans(
        8, {ValType(TypeCode::I32), ValType(TypeCode::V128)}, {}, true);
  case OpCode::V128__store16_lane:
    return checkAlignAndTrans(
        16, {ValType(TypeCode::I32), ValType(TypeCode::V128)}, {}, true);
  case OpCode::V128__store32_lane:
    return checkAlignAndTrans(
        32, {ValType(TypeCode::I32), ValType(TypeCode::V128)}, {}, true);
  case OpCode::V128__store64_lane:
    return checkAlignAndTrans(
        64, {ValType(TypeCode::I32), ValType(TypeCode::V128)}, {}, true);

  // SIMD Const Instruction.
  case OpCode::V128__const:
    return StackTrans({}, {ValType(TypeCode::V128)});

  // SIMD Shuffle Instruction.
  case OpCode::I8x16__shuffle: {
    // Check all lane index < 32 by masking
    const uint128_t Mask = (uint128_t(0xe0e0e0e0e0e0e0e0U) << 64U) |
                           uint128_t(0xe0e0e0e0e0e0e0e0U);
    const uint128_t Result = Instr.getNum().get<uint128_t>() & Mask;
    if (Result) {
      spdlog::error(ErrCode::Value::InvalidLaneIdx);
      return Unexpect(ErrCode::Value::InvalidLaneIdx);
    }
    return StackTrans({ValType(TypeCode::V128), ValType(TypeCode::V128)},
                      {ValType(TypeCode::V128)});
  }

  // SIMD Lane Instructions.
  case OpCode::I8x16__extract_lane_s:
  case OpCode::I8x16__extract_lane_u:
    return checkLaneAndTrans(16, {ValType(TypeCode::V128)},
                             {ValType(TypeCode::I32)});
  case OpCode::I8x16__replace_lane:
    return checkLaneAndTrans(16,
                             {ValType(TypeCode::V128), ValType(TypeCode::I32)},
                             {ValType(TypeCode::V128)});
  case OpCode::I16x8__extract_lane_s:
  case OpCode::I16x8__extract_lane_u:
    return checkLaneAndTrans(8, {ValType(TypeCode::V128)},
                             {ValType(TypeCode::I32)});
  case OpCode::I16x8__replace_lane:
    return checkLaneAndTrans(8,
                             {ValType(TypeCode::V128), ValType(TypeCode::I32)},
                             {ValType(TypeCode::V128)});
  case OpCode::I32x4__extract_lane:
    return checkLaneAndTrans(4, {ValType(TypeCode::V128)},
                             {ValType(TypeCode::I32)});
  case OpCode::I32x4__replace_lane:
    return checkLaneAndTrans(4,
                             {ValType(TypeCode::V128), ValType(TypeCode::I32)},
                             {ValType(TypeCode::V128)});
  case OpCode::I64x2__extract_lane:
    return checkLaneAndTrans(2, {ValType(TypeCode::V128)},
                             {ValType(TypeCode::I64)});
  case OpCode::I64x2__replace_lane:
    return checkLaneAndTrans(2,
                             {ValType(TypeCode::V128), ValType(TypeCode::I64)},
                             {ValType(TypeCode::V128)});
  case OpCode::F32x4__extract_lane:
    return checkLaneAndTrans(4, {ValType(TypeCode::V128)},
                             {ValType(TypeCode::F32)});
  case OpCode::F32x4__replace_lane:
    return checkLaneAndTrans(4,
                             {ValType(TypeCode::V128), ValType(TypeCode::F32)},
                             {ValType(TypeCode::V128)});
  case OpCode::F64x2__extract_lane:
    return checkLaneAndTrans(2, {ValType(TypeCode::V128)},
                             {ValType(TypeCode::F64)});
  case OpCode::F64x2__replace_lane:
    return checkLaneAndTrans(2,
                             {ValType(TypeCode::V128), ValType(TypeCode::F64)},
                             {ValType(TypeCode::V128)});

  // SIMD Numeric Instructions.
  case OpCode::I8x16__splat:
  case OpCode::I16x8__splat:
  case OpCode::I32x4__splat:
    return StackTrans({ValType(TypeCode::I32)}, {ValType(TypeCode::V128)});
  case OpCode::I64x2__splat:
    return StackTrans({ValType(TypeCode::I64)}, {ValType(TypeCode::V128)});
  case OpCode::F32x4__splat:
    return StackTrans({ValType(TypeCode::F32)}, {ValType(TypeCode::V128)});
  case OpCode::F64x2__splat:
    return StackTrans({ValType(TypeCode::F64)}, {ValType(TypeCode::V128)});
  case OpCode::V128__not:
  case OpCode::I8x16__abs:
  case OpCode::I8x16__neg:
  case OpCode::I8x16__popcnt:
  case OpCode::I16x8__abs:
  case OpCode::I16x8__neg:
  case OpCode::I16x8__extend_low_i8x16_s:
  case OpCode::I16x8__extend_high_i8x16_s:
  case OpCode::I16x8__extend_low_i8x16_u:
  case OpCode::I16x8__extend_high_i8x16_u:
  case OpCode::I16x8__extadd_pairwise_i8x16_s:
  case OpCode::I16x8__extadd_pairwise_i8x16_u:
  case OpCode::I32x4__abs:
  case OpCode::I32x4__neg:
  case OpCode::I32x4__extend_low_i16x8_s:
  case OpCode::I32x4__extend_high_i16x8_s:
  case OpCode::I32x4__extend_low_i16x8_u:
  case OpCode::I32x4__extend_high_i16x8_u:
  case OpCode::I32x4__extadd_pairwise_i16x8_s:
  case OpCode::I32x4__extadd_pairwise_i16x8_u:
  case OpCode::I64x2__abs:
  case OpCode::I64x2__neg:
  case OpCode::I64x2__extend_low_i32x4_s:
  case OpCode::I64x2__extend_high_i32x4_s:
  case OpCode::I64x2__extend_low_i32x4_u:
  case OpCode::I64x2__extend_high_i32x4_u:
  case OpCode::F32x4__abs:
  case OpCode::F32x4__neg:
  case OpCode::F32x4__sqrt:
  case OpCode::F64x2__abs:
  case OpCode::F64x2__neg:
  case OpCode::F64x2__sqrt:
  case OpCode::I32x4__trunc_sat_f32x4_s:
  case OpCode::I32x4__trunc_sat_f32x4_u:
  case OpCode::F32x4__convert_i32x4_s:
  case OpCode::F32x4__convert_i32x4_u:
  case OpCode::I32x4__trunc_sat_f64x2_s_zero:
  case OpCode::I32x4__trunc_sat_f64x2_u_zero:
  case OpCode::F64x2__convert_low_i32x4_s:
  case OpCode::F64x2__convert_low_i32x4_u:
  case OpCode::F32x4__demote_f64x2_zero:
  case OpCode::F64x2__promote_low_f32x4:
  case OpCode::F32x4__ceil:
  case OpCode::F32x4__floor:
  case OpCode::F32x4__trunc:
  case OpCode::F32x4__nearest:
  case OpCode::F64x2__ceil:
  case OpCode::F64x2__floor:
  case OpCode::F64x2__trunc:
  case OpCode::F64x2__nearest:
    return StackTrans({ValType(TypeCode::V128)}, {ValType(TypeCode::V128)});
  case OpCode::I8x16__swizzle:
  case OpCode::I8x16__eq:
  case OpCode::I8x16__ne:
  case OpCode::I8x16__lt_s:
  case OpCode::I8x16__lt_u:
  case OpCode::I8x16__gt_s:
  case OpCode::I8x16__gt_u:
  case OpCode::I8x16__le_s:
  case OpCode::I8x16__le_u:
  case OpCode::I8x16__ge_s:
  case OpCode::I8x16__ge_u:
  case OpCode::I16x8__eq:
  case OpCode::I16x8__ne:
  case OpCode::I16x8__lt_s:
  case OpCode::I16x8__lt_u:
  case OpCode::I16x8__gt_s:
  case OpCode::I16x8__gt_u:
  case OpCode::I16x8__le_s:
  case OpCode::I16x8__le_u:
  case OpCode::I16x8__ge_s:
  case OpCode::I16x8__ge_u:
  case OpCode::I32x4__eq:
  case OpCode::I32x4__ne:
  case OpCode::I32x4__lt_s:
  case OpCode::I32x4__lt_u:
  case OpCode::I32x4__gt_s:
  case OpCode::I32x4__gt_u:
  case OpCode::I32x4__le_s:
  case OpCode::I32x4__le_u:
  case OpCode::I32x4__ge_s:
  case OpCode::I32x4__ge_u:
  case OpCode::I64x2__eq:
  case OpCode::I64x2__ne:
  case OpCode::I64x2__lt_s:
  case OpCode::I64x2__gt_s:
  case OpCode::I64x2__le_s:
  case OpCode::I64x2__ge_s:
  case OpCode::F32x4__eq:
  case OpCode::F32x4__ne:
  case OpCode::F32x4__lt:
  case OpCode::F32x4__gt:
  case OpCode::F32x4__le:
  case OpCode::F32x4__ge:
  case OpCode::F64x2__eq:
  case OpCode::F64x2__ne:
  case OpCode::F64x2__lt:
  case OpCode::F64x2__gt:
  case OpCode::F64x2__le:
  case OpCode::F64x2__ge:
  case OpCode::V128__and:
  case OpCode::V128__andnot:
  case OpCode::V128__or:
  case OpCode::V128__xor:
  case OpCode::I8x16__narrow_i16x8_s:
  case OpCode::I8x16__narrow_i16x8_u:
  case OpCode::I8x16__add:
  case OpCode::I8x16__add_sat_s:
  case OpCode::I8x16__add_sat_u:
  case OpCode::I8x16__sub:
  case OpCode::I8x16__sub_sat_s:
  case OpCode::I8x16__sub_sat_u:
  case OpCode::I8x16__min_s:
  case OpCode::I8x16__min_u:
  case OpCode::I8x16__max_s:
  case OpCode::I8x16__max_u:
  case OpCode::I8x16__avgr_u:
  case OpCode::I16x8__narrow_i32x4_s:
  case OpCode::I16x8__narrow_i32x4_u:
  case OpCode::I16x8__add:
  case OpCode::I16x8__add_sat_s:
  case OpCode::I16x8__add_sat_u:
  case OpCode::I16x8__sub:
  case OpCode::I16x8__sub_sat_s:
  case OpCode::I16x8__sub_sat_u:
  case OpCode::I16x8__mul:
  case OpCode::I16x8__min_s:
  case OpCode::I16x8__min_u:
  case OpCode::I16x8__max_s:
  case OpCode::I16x8__max_u:
  case OpCode::I16x8__avgr_u:
  case OpCode::I16x8__extmul_low_i8x16_s:
  case OpCode::I16x8__extmul_high_i8x16_s:
  case OpCode::I16x8__extmul_low_i8x16_u:
  case OpCode::I16x8__extmul_high_i8x16_u:
  case OpCode::I16x8__q15mulr_sat_s:
  case OpCode::I32x4__add:
  case OpCode::I32x4__sub:
  case OpCode::I32x4__mul:
  case OpCode::I32x4__min_s:
  case OpCode::I32x4__min_u:
  case OpCode::I32x4__max_s:
  case OpCode::I32x4__max_u:
  case OpCode::I32x4__extmul_low_i16x8_s:
  case OpCode::I32x4__extmul_high_i16x8_s:
  case OpCode::I32x4__extmul_low_i16x8_u:
  case OpCode::I32x4__extmul_high_i16x8_u:
  case OpCode::I64x2__add:
  case OpCode::I64x2__sub:
  case OpCode::I64x2__mul:
  case OpCode::I64x2__extmul_low_i32x4_s:
  case OpCode::I64x2__extmul_high_i32x4_s:
  case OpCode::I64x2__extmul_low_i32x4_u:
  case OpCode::I64x2__extmul_high_i32x4_u:
  case OpCode::F32x4__add:
  case OpCode::F32x4__sub:
  case OpCode::F32x4__mul:
  case OpCode::F32x4__div:
  case OpCode::F32x4__min:
  case OpCode::F32x4__max:
  case OpCode::F32x4__pmin:
  case OpCode::F32x4__pmax:
  case OpCode::F64x2__add:
  case OpCode::F64x2__sub:
  case OpCode::F64x2__mul:
  case OpCode::F64x2__div:
  case OpCode::F64x2__min:
  case OpCode::F64x2__max:
  case OpCode::F64x2__pmin:
  case OpCode::F64x2__pmax:
  case OpCode::I32x4__dot_i16x8_s:
    return StackTrans({ValType(TypeCode::V128), ValType(TypeCode::V128)},
                      {ValType(TypeCode::V128)});
  case OpCode::V128__bitselect:
    return StackTrans({ValType(TypeCode::V128), ValType(TypeCode::V128),
                       ValType(TypeCode::V128)},
                      {ValType(TypeCode::V128)});
  case OpCode::V128__any_true:
  case OpCode::I8x16__all_true:
  case OpCode::I8x16__bitmask:
  case OpCode::I16x8__all_true:
  case OpCode::I16x8__bitmask:
  case OpCode::I32x4__all_true:
  case OpCode::I32x4__bitmask:
  case OpCode::I64x2__all_true:
  case OpCode::I64x2__bitmask:
    return StackTrans({ValType(TypeCode::V128)}, {ValType(TypeCode::I32)});
  case OpCode::I8x16__shl:
  case OpCode::I8x16__shr_s:
  case OpCode::I8x16__shr_u:
  case OpCode::I16x8__shl:
  case OpCode::I16x8__shr_s:
  case OpCode::I16x8__shr_u:
  case OpCode::I32x4__shl:
  case OpCode::I32x4__shr_s:
  case OpCode::I32x4__shr_u:
  case OpCode::I64x2__shl:
  case OpCode::I64x2__shr_s:
  case OpCode::I64x2__shr_u:
    return StackTrans({ValType(TypeCode::V128), ValType(TypeCode::I32)},
                      {ValType(TypeCode::V128)});

  case OpCode::Atomic__fence:
    return {};

  case OpCode::Memory__atomic__notify:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::Memory__atomic__wait32:
    return checkAlignAndTrans(32,
                              std::array{ValType(TypeCode::I32),
                                         ValType(TypeCode::I32),
                                         ValType(TypeCode::I64)},
                              std::array{ValType(TypeCode::I32)});
  case OpCode::Memory__atomic__wait64:
    return checkAlignAndTrans(64,
                              std::array{ValType(TypeCode::I32),
                                         ValType(TypeCode::I64),
                                         ValType(TypeCode::I64)},
                              std::array{ValType(TypeCode::I32)});

  case OpCode::I32__atomic__load:
    return checkAlignAndTrans(32, std::array{ValType(TypeCode::I32)},
                              std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__load:
    return checkAlignAndTrans(64, std::array{ValType(TypeCode::I32)},
                              std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__load8_u:
    return checkAlignAndTrans(8, std::array{ValType(TypeCode::I32)},
                              std::array{ValType(TypeCode::I32)});
  case OpCode::I32__atomic__load16_u:
    return checkAlignAndTrans(16, std::array{ValType(TypeCode::I32)},
                              std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__load8_u:
    return checkAlignAndTrans(8, std::array{ValType(TypeCode::I32)},
                              std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__load16_u:
    return checkAlignAndTrans(16, std::array{ValType(TypeCode::I32)},
                              std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__load32_u:
    return checkAlignAndTrans(32, std::array{ValType(TypeCode::I32)},
                              std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__store:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)}, {});
  case OpCode::I64__atomic__store:
    return checkAlignAndTrans(
        64, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  case OpCode::I32__atomic__store8:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)}, {});
  case OpCode::I32__atomic__store16:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)}, {});
  case OpCode::I64__atomic__store8:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  case OpCode::I64__atomic__store16:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  case OpCode::I64__atomic__store32:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  case OpCode::I32__atomic__rmw__add:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw__add:
    return checkAlignAndTrans(
        64, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw8__add_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I32__atomic__rmw16__add_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw8__add_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw16__add_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw32__add_u:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw__sub:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw__sub:
    return checkAlignAndTrans(
        64, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw8__sub_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I32__atomic__rmw16__sub_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw8__sub_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw16__sub_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw32__sub_u:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw__and:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw__and:
    return checkAlignAndTrans(
        64, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw8__and_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I32__atomic__rmw16__and_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw8__and_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw16__and_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw32__and_u:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw__or:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw__or:
    return checkAlignAndTrans(
        64, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw8__or_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I32__atomic__rmw16__or_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw8__or_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw16__or_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw32__or_u:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw__xor:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw__xor:
    return checkAlignAndTrans(
        64, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw8__xor_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I32__atomic__rmw16__xor_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw8__xor_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw16__xor_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw32__xor_u:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw__xchg:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw__xchg:
    return checkAlignAndTrans(
        64, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw8__xchg_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I32__atomic__rmw16__xchg_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I32)},
        std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw8__xchg_u:
    return checkAlignAndTrans(
        8, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw16__xchg_u:
    return checkAlignAndTrans(
        16, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw32__xchg_u:
    return checkAlignAndTrans(
        32, std::array{ValType(TypeCode::I32), ValType(TypeCode::I64)},
        std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw__cmpxchg:
    return checkAlignAndTrans(32,
                              std::array{ValType(TypeCode::I32),
                                         ValType(TypeCode::I32),
                                         ValType(TypeCode::I32)},
                              std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw__cmpxchg:
    return checkAlignAndTrans(64,
                              std::array{ValType(TypeCode::I32),
                                         ValType(TypeCode::I64),
                                         ValType(TypeCode::I64)},
                              std::array{ValType(TypeCode::I64)});
  case OpCode::I32__atomic__rmw8__cmpxchg_u:
    return checkAlignAndTrans(8,
                              std::array{ValType(TypeCode::I32),
                                         ValType(TypeCode::I32),
                                         ValType(TypeCode::I32)},
                              std::array{ValType(TypeCode::I32)});
  case OpCode::I32__atomic__rmw16__cmpxchg_u:
    return checkAlignAndTrans(16,
                              std::array{ValType(TypeCode::I32),
                                         ValType(TypeCode::I32),
                                         ValType(TypeCode::I32)},
                              std::array{ValType(TypeCode::I32)});
  case OpCode::I64__atomic__rmw8__cmpxchg_u:
    return checkAlignAndTrans(8,
                              std::array{ValType(TypeCode::I32),
                                         ValType(TypeCode::I64),
                                         ValType(TypeCode::I64)},
                              std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw16__cmpxchg_u:
    return checkAlignAndTrans(16,
                              std::array{ValType(TypeCode::I32),
                                         ValType(TypeCode::I64),
                                         ValType(TypeCode::I64)},
                              std::array{ValType(TypeCode::I64)});
  case OpCode::I64__atomic__rmw32__cmpxchg_u:
    return checkAlignAndTrans(32,
                              std::array{ValType(TypeCode::I32),
                                         ValType(TypeCode::I64),
                                         ValType(TypeCode::I64)},
                              std::array{ValType(TypeCode::I64)});

  default:
    assumingUnreachable();
  }
}

void FormChecker::pushType(VType V) { ValStack.emplace_back(V); }

void FormChecker::pushTypes(Span<const VType> Input) {
  for (auto Val : Input) {
    pushType(Val);
  }
}

void FormChecker::pushTypes(Span<const ValType> Input) {
  for (auto Val : Input) {
    pushType(Val);
  }
}

Expect<VType> FormChecker::popType() {
  if (ValStack.size() == CtrlStack.back().Height) {
    if (CtrlStack.back().IsUnreachable) {
      return unreachableVType();
    }
    // Value stack underflow
    spdlog::error(ErrCode::Value::TypeCheckFailed);
    spdlog::error("    Value stack underflow.");
    return Unexpect(ErrCode::Value::TypeCheckFailed);
  }
  auto Res = ValStack.back();
  ValStack.pop_back();
  return Res;
}

Expect<VType> FormChecker::popType(ValType E) {
  auto Res = popType();
  if (!Res) {
    return Unexpect(Res);
  }
  if (*Res == unreachableVType()) {
    return E;
  }

  if (!matchType(E, **Res)) {
    // Expect value on value stack is not matched
    spdlog::error(ErrCode::Value::TypeCheckFailed);
    spdlog::error(ErrInfo::InfoMismatch(VTypeToAST(E), VTypeToAST(*Res)));
    return Unexpect(ErrCode::Value::TypeCheckFailed);
  }
  return *Res;
}

Expect<void> FormChecker::popTypes(Span<const ValType> Input) {
  for (auto Val = Input.rbegin(); Val != Input.rend(); ++Val) {
    if (auto Res = popType(*Val); !Res) {
      return Unexpect(Res);
    }
  }
  return {};
}

void FormChecker::pushCtrl(Span<const ValType> In, Span<const ValType> Out,
                           const AST::Instruction *Jump, OpCode Code) {
  CtrlStack.emplace_back(In, Out, Jump, ValStack.size(), LocalInits.size(),
                         Code);
  pushTypes(In);
}

Expect<FormChecker::CtrlFrame> FormChecker::popCtrl() {
  if (CtrlStack.empty()) {
    // Ctrl stack is empty when popping.
    spdlog::error(ErrCode::Value::TypeCheckFailed);
    spdlog::error("    Control stack underflow.");
    return Unexpect(ErrCode::Value::TypeCheckFailed);
  }
  if (auto Res = popTypes(CtrlStack.back().EndTypes); !Res) {
    return Unexpect(Res);
  }
  if (ValStack.size() != CtrlStack.back().Height) {
    // Value stack size not matched.
    spdlog::error(ErrCode::Value::TypeCheckFailed);
    spdlog::error("    Value stack underflow.");
    return Unexpect(ErrCode::Value::TypeCheckFailed);
  }
  // When popping a frame, reset the inited locals during this frame.
  for (size_t I = CtrlStack.back().InitedLocal; I < LocalInits.size(); I++) {
    Locals[LocalInits[I]].IsInit = false;
  }
  LocalInits.erase(LocalInits.begin() +
                       static_cast<uint32_t>(CtrlStack.back().InitedLocal),
                   LocalInits.end());
  auto Head = std::move(CtrlStack.back());
  CtrlStack.pop_back();
  return Head;
}

Span<const ValType>
FormChecker::getLabelTypes(const FormChecker::CtrlFrame &F) {
  if (F.Code == OpCode::Loop) {
    return F.StartTypes;
  }
  return F.EndTypes;
}

Expect<void> FormChecker::unreachable() {
  while (ValStack.size() > CtrlStack.back().Height) {
    if (auto Res = popType(); !Res) {
      return Unexpect(Res);
    }
  }
  CtrlStack.back().IsUnreachable = true;
  return {};
}

Expect<void> FormChecker::StackTrans(Span<const ValType> Take,
                                     Span<const ValType> Put) {
  if (auto Res = popTypes(Take); !Res) {
    return Unexpect(Res);
  }
  pushTypes(Put);
  return {};
}

Expect<void> FormChecker::StackPopAny() {
  if (auto Res = popType(); !Res) {
    return Unexpect(Res);
  }
  return {};
}

} // namespace Validator
} // namespace WasmEdge
