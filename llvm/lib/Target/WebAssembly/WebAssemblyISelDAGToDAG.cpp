//- WebAssemblyISelDAGToDAG.cpp - A dag to dag inst selector for WebAssembly -//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines an instruction selector for the WebAssembly target.
///
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/WebAssemblyMCTargetDesc.h"
#include "WebAssembly.h"
#include "WebAssemblyTargetMachine.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h" // To access function attributes.
#include "llvm/IR/IntrinsicsWebAssembly.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "wasm-isel"

//===--------------------------------------------------------------------===//
/// WebAssembly-specific code to select WebAssembly machine instructions for
/// SelectionDAG operations.
///
namespace {
class WebAssemblyDAGToDAGISel final : public SelectionDAGISel {
  /// Keep a pointer to the WebAssemblySubtarget around so that we can make the
  /// right decision when generating code for different targets.
  const WebAssemblySubtarget *Subtarget;

public:
  WebAssemblyDAGToDAGISel(WebAssemblyTargetMachine &TM,
                          CodeGenOpt::Level OptLevel)
      : SelectionDAGISel(TM, OptLevel), Subtarget(nullptr) {
  }

  StringRef getPassName() const override {
    return "WebAssembly Instruction Selection";
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    LLVM_DEBUG(dbgs() << "********** ISelDAGToDAG **********\n"
                         "********** Function: "
                      << MF.getName() << '\n');

    Subtarget = &MF.getSubtarget<WebAssemblySubtarget>();

    // Wasm64 is not fully supported right now (and is not specified)
    if (Subtarget->hasAddr64())
      report_fatal_error(
          "64-bit WebAssembly (wasm64) is not currently supported");

    return SelectionDAGISel::runOnMachineFunction(MF);
  }

  void Select(SDNode *Node) override;

  bool SelectInlineAsmMemoryOperand(const SDValue &Op, unsigned ConstraintID,
                                    std::vector<SDValue> &OutOps) override;

// Include the pieces autogenerated from the target description.
#include "WebAssemblyGenDAGISel.inc"

private:
  // add select functions here...
};
} // end anonymous namespace

void WebAssemblyDAGToDAGISel::Select(SDNode *Node) {
  // If we have a custom node, we already have selected!
  if (Node->isMachineOpcode()) {
    LLVM_DEBUG(errs() << "== "; Node->dump(CurDAG); errs() << "\n");
    Node->setNodeId(-1);
    return;
  }

  // Few custom selection stuff.
  SDLoc DL(Node);
  MachineFunction &MF = CurDAG->getMachineFunction();
  switch (Node->getOpcode()) {
  case ISD::ATOMIC_FENCE: {
    if (!MF.getSubtarget<WebAssemblySubtarget>().hasAtomics())
      break;

    uint64_t SyncScopeID =
        cast<ConstantSDNode>(Node->getOperand(2).getNode())->getZExtValue();
    MachineSDNode *Fence = nullptr;
    switch (SyncScopeID) {
    case SyncScope::SingleThread:
      // We lower a single-thread fence to a pseudo compiler barrier instruction
      // preventing instruction reordering. This will not be emitted in final
      // binary.
      Fence = CurDAG->getMachineNode(WebAssembly::COMPILER_FENCE,
                                     DL,                 // debug loc
                                     MVT::Other,         // outchain type
                                     Node->getOperand(0) // inchain
      );
      break;
    case SyncScope::System:
      // Currently wasm only supports sequentially consistent atomics, so we
      // always set the order to 0 (sequentially consistent).
      Fence = CurDAG->getMachineNode(
          WebAssembly::ATOMIC_FENCE,
          DL,                                         // debug loc
          MVT::Other,                                 // outchain type
          CurDAG->getTargetConstant(0, DL, MVT::i32), // order
          Node->getOperand(0)                         // inchain
      );
      break;
    default:
      llvm_unreachable("Unknown scope!");
    }

    ReplaceNode(Node, Fence);
    CurDAG->RemoveDeadNode(Node);
    return;
  }

  case ISD::GlobalTLSAddress: {
    const auto *GA = cast<GlobalAddressSDNode>(Node);

    if (!MF.getSubtarget<WebAssemblySubtarget>().hasBulkMemory())
      report_fatal_error("cannot use thread-local storage without bulk memory",
                         false);

    // Currently Emscripten does not support dynamic linking with threads.
    // Therefore, if we have thread-local storage, only the local-exec model
    // is possible.
    // TODO: remove this and implement proper TLS models once Emscripten
    // supports dynamic linking with threads.
    if (GA->getGlobal()->getThreadLocalMode() !=
            GlobalValue::LocalExecTLSModel &&
        !Subtarget->getTargetTriple().isOSEmscripten()) {
      report_fatal_error("only -ftls-model=local-exec is supported for now on "
                         "non-Emscripten OSes: variable " +
                             GA->getGlobal()->getName(),
                         false);
    }

    MVT PtrVT = TLI->getPointerTy(CurDAG->getDataLayout());
    assert(PtrVT == MVT::i32 && "only wasm32 is supported for now");

    SDValue TLSBaseSym = CurDAG->getTargetExternalSymbol("__tls_base", PtrVT);
    SDValue TLSOffsetSym = CurDAG->getTargetGlobalAddress(
        GA->getGlobal(), DL, PtrVT, GA->getOffset(), 0);

    MachineSDNode *TLSBase = CurDAG->getMachineNode(WebAssembly::GLOBAL_GET_I32,
                                                    DL, MVT::i32, TLSBaseSym);
    MachineSDNode *TLSOffset = CurDAG->getMachineNode(
        WebAssembly::CONST_I32, DL, MVT::i32, TLSOffsetSym);
    MachineSDNode *TLSAddress =
        CurDAG->getMachineNode(WebAssembly::ADD_I32, DL, MVT::i32,
                               SDValue(TLSBase, 0), SDValue(TLSOffset, 0));
    ReplaceNode(Node, TLSAddress);
    return;
  }

  case ISD::INTRINSIC_WO_CHAIN: {
    unsigned IntNo = cast<ConstantSDNode>(Node->getOperand(0))->getZExtValue();
    switch (IntNo) {
    case Intrinsic::wasm_tls_size: {
      MVT PtrVT = TLI->getPointerTy(CurDAG->getDataLayout());
      assert(PtrVT == MVT::i32 && "only wasm32 is supported for now");

      MachineSDNode *TLSSize = CurDAG->getMachineNode(
          WebAssembly::GLOBAL_GET_I32, DL, PtrVT,
          CurDAG->getTargetExternalSymbol("__tls_size", MVT::i32));
      ReplaceNode(Node, TLSSize);
      return;
    }
    case Intrinsic::wasm_tls_align: {
      MVT PtrVT = TLI->getPointerTy(CurDAG->getDataLayout());
      assert(PtrVT == MVT::i32 && "only wasm32 is supported for now");

      MachineSDNode *TLSAlign = CurDAG->getMachineNode(
          WebAssembly::GLOBAL_GET_I32, DL, PtrVT,
          CurDAG->getTargetExternalSymbol("__tls_align", MVT::i32));
      ReplaceNode(Node, TLSAlign);
      return;
    }
    }
    break;
  }
  case ISD::INTRINSIC_W_CHAIN: {
    unsigned IntNo = cast<ConstantSDNode>(Node->getOperand(1))->getZExtValue();
    switch (IntNo) {
    case Intrinsic::wasm_tls_base: {
      MVT PtrVT = TLI->getPointerTy(CurDAG->getDataLayout());
      assert(PtrVT == MVT::i32 && "only wasm32 is supported for now");

      MachineSDNode *TLSBase = CurDAG->getMachineNode(
          WebAssembly::GLOBAL_GET_I32, DL, MVT::i32, MVT::Other,
          CurDAG->getTargetExternalSymbol("__tls_base", PtrVT),
          Node->getOperand(0));
      ReplaceNode(Node, TLSBase);
      return;
    }
    }
    break;
  }
  case WebAssemblyISD::CALL: {
    // CALL has both variable operands and variable results, but ISel only
    // supports one or the other. Split calls into two nodes glued together, one
    // for the operands and one for the results. These two nodes will be
    // recombined in a custom inserter hook into a single MachineInstr.
    SmallVector<SDValue, 16> Ops;
    for (size_t i = 1; i < Node->getNumOperands(); ++i) {
      SDValue Op = Node->getOperand(i);
      if (Op->getOpcode() == WebAssemblyISD::Wrapper)
        Op = Op->getOperand(0);
      Ops.push_back(Op);
    }
    Ops.push_back(Node->getOperand(0));
    MachineSDNode *CallParams =
        CurDAG->getMachineNode(WebAssembly::CALL_PARAMS, DL, MVT::Glue, Ops);
    SDValue Link(CallParams, 0);
    MachineSDNode *CallResults = CurDAG->getMachineNode(
        WebAssembly::CALL_RESULTS, DL, Node->getVTList(), Link);
    ReplaceNode(Node, CallResults);
    return;
  }

  default:
    break;
  }

  // Select the default instruction.
  SelectCode(Node);
}

bool WebAssemblyDAGToDAGISel::SelectInlineAsmMemoryOperand(
    const SDValue &Op, unsigned ConstraintID, std::vector<SDValue> &OutOps) {
  switch (ConstraintID) {
  case InlineAsm::Constraint_m:
    // We just support simple memory operands that just have a single address
    // operand and need no special handling.
    OutOps.push_back(Op);
    return false;
  default:
    break;
  }

  return true;
}

/// This pass converts a legalized DAG into a WebAssembly-specific DAG, ready
/// for instruction scheduling.
FunctionPass *llvm::createWebAssemblyISelDag(WebAssemblyTargetMachine &TM,
                                             CodeGenOpt::Level OptLevel) {
  return new WebAssemblyDAGToDAGISel(TM, OptLevel);
}
