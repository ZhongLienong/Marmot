#pragma once

#include "Compiler/MidoriIROptimizer/MidoriIROptimizer.h"

// Sparse conditional constant propagation: a value is a constant when every
// path the function can take makes it one, and a branch on a constant goes one
// way. A global defined by a constant other than Text reads as that constant.
// Replaces ConstantFolding, LocalConstantPropagation and
// ConstantBranchElimination.
bool RunSccp(MidoriIRModule& module);
inline constexpr MidoriIRPass SccpPass{ "Sccp", &RunSccp };

// Drops what nothing uses and nothing needs, block parameters included, then
// simplifies the graph: a branch whose edges agree is a jump, a block that only
// jumps on is skipped, a block with one predecessor that jumps to it joins it,
// and blocks nothing reaches go. Replaces DeadCodeElimination and
// CanonicalizationCleanup.
bool RunDeadCodeElimination(MidoriIRModule& module);
inline constexpr MidoriIRPass DeadCodeEliminationPass{ "DeadCodeElimination", &RunDeadCodeElimination };

// A tail call of the function itself becomes a jump back to a loop header,
// whether it names the function, the global that holds it, or the capture a
// local function reaches itself through. Every other tail call stays one.
// Replaces TailCallOptimization.
bool RunSelfTailCall(MidoriIRModule& module);
inline constexpr MidoriIRPass SelfTailCallPass{ "SelfTailCall", &RunSelfTailCall };

// Resolves the closures a call is known to call, drops the captures nothing
// reads, and turns the captures of a local function whose closure is only ever
// called into parameters, so it is called directly and made nowhere. Replaces
// ClosureLifting.
bool RunClosureConversion(MidoriIRModule& module);
inline constexpr MidoriIRPass ClosureConversionPass{ "ClosureConversion", &RunClosureConversion };

// A local function only ever tail called, and only from one function, becomes
// blocks of that function, and each tail call a jump to them.
bool RunContification(MidoriIRModule& module);
inline constexpr MidoriIRPass ContificationPass{ "Contification", &RunContification };

// Copies a directly called function's body into its caller when it is small or
// called nowhere else, callees before callers. A call in tail position stays a
// tail call: the callee's returns become the caller's, and its tail calls stay
// tail calls. Specializations of generics are inlined like any function.
// Replaces FunctionInlining.
bool RunInlining(MidoriIRModule& module);
inline constexpr MidoriIRPass InliningPass{ "Inlining", &RunInlining };

// Algebraic identities with a constant operand: x + 0, x * 1, x * 0, x * 2^n,
// and the like, only where they hold for every value, NaN and -0.0 included.
// Replaces StrengthReduction.
bool RunStrengthReduction(MidoriIRModule& module);
inline constexpr MidoriIRPass StrengthReductionPass{ "StrengthReduction", &RunStrengthReduction };

// An edge that passes a union into a small block that only branches on its
// tag goes straight to the successor that tag picks. The edge carries the
// block's few pure instructions with it, and that successor takes the block's
// values it reads as parameters, so what the union was made with can be read
// from it where it is taken apart.
bool RunKnownConstructorThreading(MidoriIRModule& module);
inline constexpr MidoriIRPass KnownConstructorThreadingPass{ "KnownConstructorThreading", &RunKnownConstructorThreading };

// A block parameter that is always passed a struct or tuple made for it, or
// itself around a loop, and is only ever taken apart, becomes a parameter for
// each part read, so the whole need not be made.
bool RunParameterUnboxing(MidoriIRModule& module);
inline constexpr MidoriIRPass ParameterUnboxingPass{ "ParameterUnboxing", &RunParameterUnboxing };

// A tuple, struct or union made and only taken apart again is never made:
// each part read from it is the value it was made with.
bool RunScalarReplacement(MidoriIRModule& module);
inline constexpr MidoriIRPass ScalarReplacementPass{ "ScalarReplacement", &RunScalarReplacement };

// An instruction that computes what a dominating one already did uses its
// value instead. Only instructions whose value depends on nothing but their
// operands take part: never an allocation, whose identity is its own.
bool RunGlobalValueNumbering(MidoriIRModule& module);
inline constexpr MidoriIRPass GlobalValueNumberingPass{ "GlobalValueNumbering", &RunGlobalValueNumbering };

// An instruction in a loop whose operands the loop does not change, and which
// can neither fail nor allocate, moves to the block before the loop.
bool RunLoopInvariantCodeMotion(MidoriIRModule& module);
inline constexpr MidoriIRPass LoopInvariantCodeMotionPass{ "LoopInvariantCodeMotion", &RunLoopInvariantCodeMotion };
