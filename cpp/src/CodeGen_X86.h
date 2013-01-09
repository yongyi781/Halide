#ifndef HALIDE_CODEGEN_X86_H
#define HALIDE_CODEGEN_X86_H

/** \file
 * Defines the code-generator for producing x86 machine code 
 */

#include "CodeGen.h"

namespace Halide { 
namespace Internal {

/** A code generator that emits x86 code from a given Halide stmt. */
class CodeGen_X86 : public CodeGen {
public:

    /** Create an x86 code generator. Processor features can be
     * enabled using the appropriate arguments */
    CodeGen_X86(bool use_sse_41 = true, bool use_avx = true);
        
    /** Compile to an internally-held llvm module. Takes a halide
     * statement, the name of the function produced, and the arguments
     * to the function produced. After calling this, call
     * CodeGen::compile_to_file or
     * CodeGen::compile_to_function_pointer to get at the x86 machine
     * code. */
    void compile(Stmt stmt, std::string name, const std::vector<Argument> &args);

    static void test();

protected:
    /** Should the emitted code make use of sse 4.1 */
    bool use_sse_41;

    /** Should the emitted code use avx 1 operations */
    bool use_avx;

    /** Some useful llvm types */
    // @{
    llvm::Type *i32x4, *i32x8;
    // @}

    /** Some wildcard variables used for peephole optimizations  */
    // @{
    Expr wild_i8x16, wild_i16x8, wild_i16x16, wild_i32x4, wild_i32x8;
    Expr wild_u8x16, wild_u16x8, wild_u32x4;
    Expr wild_f32x4, wild_f32x8, wild_f64x2;
    // @}

    /** Generate a call to an sse or avx intrinsic */
    // @{
    llvm::Value *call_intrin(Type t, const std::string &name, std::vector<Expr>);    
    llvm::Value *call_intrin(Type t, const std::string &name, std::vector<llvm::Value *>);    
    // @}

    /** Nodes for which we want to emit specific sse/avx intrinsics */
    // @{
    void visit(const Cast *);
    void visit(const Div *);
    void visit(const Min *);
    void visit(const Max *);
    // @}

    /** X86 implementation of Allocate. Small constant-sized ones go
     * on the stack. The rest go on the heap by calling "fast_malloc"
     * and "fast_free" in the standard library */
    void visit(const Allocate *);        

    /** The heap allocations currently in scope */
    std::stack<llvm::Value *> heap_allocations;

    /** Free all heap allocations in scope */
    void prepare_for_early_exit();

    std::string mcpu() const;
    std::string mattrs() const;
};

}}

#endif