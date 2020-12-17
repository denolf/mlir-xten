// (c) Copyright 2019 Xilinx Inc. All Rights Reserved.

#include "ATenDialect.h"
#include "AIRDialect.h"
#include "AffineToAIRPass.h"


#include "mlir/Analysis/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/EDSC/Builders.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/StandardOps/EDSC/Builders.h"
#include "mlir/Dialect/StandardOps/EDSC/Intrinsics.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/SCF/EDSC/Builders.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/EDSC/Builders.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Parser.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <sstream>

using namespace mlir;
using namespace xilinx;

#define DEBUG_TYPE "affine-to-air"

namespace {

#include "AffineToAIR.cpp.inc"

class AffineCopyToAIRDMAConversion : public ConversionPattern {
public:
  explicit AffineCopyToAIRDMAConversion(MLIRContext *context) : ConversionPattern(10, MatchAnyOpTypeTag()) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value > operands,
                  ConversionPatternRewriter &rewriter) const override
  {
    auto afo = dyn_cast<AffineForOp>(op);
    if (!afo)
      return failure();

    mlir::AffineLoadOp load = nullptr;
    mlir::AffineStoreOp store = nullptr;

    for (auto &o : afo.getLoopBody().getOps()) {
      if (isa<AffineLoadOp>(o)) {
        load = cast<AffineLoadOp>(o);
      }
      else if (isa<AffineStoreOp>(o)) {
        store = cast<AffineStoreOp>(o);
      }
      else if (isa<AffineYieldOp>(o)) {
      }
      else {
        llvm::outs() << "FAIL!\n";
        op->print(llvm::outs());
        o.print(llvm::outs());
        return failure();
      }
    }

    llvm::outs() << "HERE!\n";
    op->print(llvm::outs());

    if (!load || !store)
      return failure();

    if (store.value() != load)
      return failure();

    auto srcTy = load.memref().getType().cast<mlir::MemRefType>();
    auto dstTy = store.memref().getType().cast<mlir::MemRefType>();

    if (srcTy.getMemorySpace() == 0 && dstTy.getMemorySpace() == 1) {
      // ext -> L2
      // #map7 = affine_map<()[s0] -> (s0 + 32)>
      // affine.for %arg5 = %arg3 to #map7()[%arg3] {
      //   %0 = affine.load %arg1[%arg4, %arg5] : memref<256x256xf32>
      //   affine.store %0, %arg2[-%arg0 + %arg4, -%arg3 + %arg5] : memref<32x32xf32, 1>
      // }
      //air.shim_dma_memcpy(%src,  %dst,  %src_d1, %src_d0, %dst_d1,        %dst_d0, %num)
      //air.shim_dma_memcpy(%arg1, %arg2, %arg4,   %arg3,   -%arg0 + %arg4, 0,       32)
      llvm::outs() << "L3 to L2!\n";

      mlir::AffineMap lbm = afo.getLowerBoundMap();
      mlir::AffineMap ubm = afo.getUpperBoundMap();

      auto int32Ty = mlir::IntegerType::get(32, op->getContext());
      auto attr = mlir::IntegerAttr::get(int32Ty, 0);
      SmallVector<Attribute, 1> attrs{attr};
      SmallVector<Attribute, 2> ints;
      lbm.constantFold(attrs, ints);
      ubm.constantFold(attrs, ints);
      int64_t lower_bound = ints[0].cast<mlir::IntegerAttr>().getInt();
      int64_t upper_bound = ints[1].cast<mlir::IntegerAttr>().getInt();

      llvm::outs() << "LB: " << lower_bound << " UB: " << upper_bound << "\n";
      auto loc = op->getLoc();
      auto zero_const = rewriter.create<ConstantIndexOp>(loc, 0);
      auto upper_bound_const = rewriter.create<ConstantIndexOp>(loc, upper_bound);
      auto shim_dma_memcpy = rewriter.create<xilinx::air::ShimDmaMemcpy>(loc, load.memref(), store.memref(),
                                                                         load.indices()[0], afo.getLowerBoundOperands()[0],
                                                                         store.indices()[0], zero_const, upper_bound_const);
      // rewriter.eraseOp(load);
      // rewriter.eraseOp(store);
      rewriter.eraseOp(op);

      shim_dma_memcpy.getParentOfType<FuncOp>().dump();
      return success();
    }
    else if (srcTy.getMemorySpace() == 1 || dstTy.getMemorySpace() == 0) {
      // L2 -> ext
    }
    else {
      return failure();
    }
    return failure();
  }
};

struct AffineToAIRPass : public PassWrapper<AffineToAIRPass,
                                               OperationPass<ModuleOp>> {


  void lower_dma_to_function(StringRef callee, CallOp dma_callOp)
  {
    auto module = getOperation();
    auto funcOp = module.lookupSymbol<mlir::FuncOp>(callee);
    auto ctx = funcOp.getContext();
    auto loc = dma_callOp.getLoc();

    // for now it's all very much hard coded
    if ( callee.equals("acap_L2_dma_copy") ) {
      auto arg_iter = dma_callOp.arg_operand_begin();
      // input and output here are relative to the copy
      auto dim1_idx = *(arg_iter);
      auto input_operand = *(++arg_iter);
      auto output_operand = *(++arg_iter);
      auto dim0_idx = *(++arg_iter);
      std::string dmafn_name = "acap_L2_dma_copy_arg0";
      FuncOp dmafn = module.lookupSymbol<FuncOp>(dmafn_name);
      if (!dmafn) {
        SmallVector<Type, 4> tys{input_operand.getType(),
                                 output_operand.getType(),
                                 dim1_idx.getType(),
                                 dim0_idx.getType()};
        SmallVector<Type, 1> retTy{};
        auto fnTy = FunctionType::get(tys, retTy, ctx);
        dmafn = FuncOp::create(loc, dmafn_name, fnTy);
        module.push_back(dmafn);
      } 
      OpBuilder builder(dma_callOp);
      SmallVector<Value,4> opers{input_operand, output_operand, dim1_idx, dim0_idx};
      SmallVector<Type, 1> retTy;
      builder.create<CallOp>(loc, retTy, builder.getSymbolRefAttr(dmafn_name), opers);
      dma_callOp.erase();
      //acap_L2_dma_copy_arg1(&weights);
    }
    else if (callee.equals("acap_L2_dma_copy_1")) {
      auto arg_iter = dma_callOp.arg_operand_begin();
      auto dim_idx = *(arg_iter);
      // input and output here are relative to the copy
      auto dim1_idx = *(arg_iter);
      auto input_operand = *(++arg_iter);
      auto dim0_idx = *(++arg_iter);
      auto output_operand = *(++arg_iter);
      std::string dmafn_name = "acap_L2_dma_copy_arg1";
      FuncOp dmafn = module.lookupSymbol<FuncOp>(dmafn_name);
      if (!dmafn) {
        SmallVector<Type, 4> tys{input_operand.getType(),
                                 output_operand.getType(),
                                 dim1_idx.getType(),
                                 dim0_idx.getType()};
        SmallVector<Type, 1> retTy{};
        auto fnTy = FunctionType::get(tys, retTy, ctx);
        dmafn = FuncOp::create(loc, dmafn_name, fnTy);
        module.push_back(dmafn);
      } 
      OpBuilder builder(dma_callOp);
      SmallVector<Value,4> opers{input_operand, output_operand, dim1_idx, dim0_idx};
      SmallVector<Type, 1> retTy;
      builder.create<CallOp>(loc, retTy, builder.getSymbolRefAttr(dmafn_name), opers);
      dma_callOp.erase();
    }
  }

  void lowerDma(StringRef callee, CallOp dma_callOp) {
    //return lowerDma_pad(callee, dma_callOp);
    return lower_dma_to_function(callee, dma_callOp);
  }

  void getDependentDialects(::mlir::DialectRegistry &registry) const override {
     registry.insert<xilinx::air::AIRDialect>();
  }

  void runOnOperation() override {
    auto module = getOperation();
    auto context = module.getContext();


    LLVM_DEBUG(llvm::outs() << "input\n");
    LLVM_DEBUG(module.print(llvm::outs()));

    // check that a function called "graph" exists
    auto graph = module.lookupSymbol<mlir::FuncOp>("graph");
    if (!graph) {
      emitError(mlir::UnknownLoc::get(module.getContext()),
                "OpReportPass failed: can't find a graph function\n");
      signalPassFailure();
      return;
    }

    graph.walk([&](Operation *o) {
      if (auto co = dyn_cast<CallOp>(o)) {
        if (co.getCallee().startswith("acap_L2_dma_copy")) {
          lowerDma(co.getCallee(), co);
        }
        if (co.getCallee().startswith("acap_L1_dma_copy")) {
          lowerDma(co.getCallee(), co);
        }
      }
    });

    LLVM_DEBUG(llvm::outs() << "output\n");
    LLVM_DEBUG(module.print(llvm::outs()));

    // tablegen patterns
    // OwningRewritePatternList patterns;
    // patterns.insert<AffineCopyToAIRDMAConversion>(context);

    // populateWithGenerated(context, patterns);

    // // Perform aten specific Fusion.
    // ConversionTarget target(*context);

    // target.addLegalDialect<LLVM::LLVMDialect,
    //                        StandardOpsDialect, scf::SCFDialect>();
    // target.addLegalOp<xilinx::air::ShimDmaMemcpy>();
    // if (failed(applyPartialConversion(module, target, std::move(patterns)))) {
    //   emitError(UnknownLoc::get(context), "error\n");
    //   signalPassFailure();
    //   assert(0);
    // }

  }
};

}// namespace


namespace xilinx {
namespace air {

std::unique_ptr<mlir::Pass> createAffineToAIRPass() {
  return std::make_unique<AffineToAIRPass>();
}

} // namespace air
} // namespace xilinx

void xilinx::air::registerAffineToAIRPass() {
    PassRegistration<AffineToAIRPass>(
      "affine-to-air",
      "Lift affine loops to AIR dialect");
}
