#pragma once

#include <cstdint>

namespace tt {

// ---------------------------------------------------------------------------
// 32-Bit Aligned RAM Bytecode Instruction Format
//   bits[7:0]   : op  (uint8_t)
//   bits[15:8]  : a   (uint8_t)
//   bits[31:16] : bc  (uint16_t unsigned, or int16_t signed jump/int offset)
// ---------------------------------------------------------------------------
enum Op : uint8_t {
  OP_NOP = 0,

  // Constants & Special Pseudo-Variables (pushed onto evaluation stack)
  OP_PUSH_NIL,         // push nil
  OP_PUSH_ME,          // push me (receiver)
  OP_PUSH_UP,          // push up (super marker)
  OP_PUSH_FRAME,       // push active Frame object (for custom macros)
  OP_PUSH_INT16,       // push makeInt30((int16_t)bc)
  OP_PUSH_CONST,       // push method->consts[bc]

  // Stack Manipulation
  OP_POP,              // pop 1 value
  OP_DUP,              // duplicate top of stack

  // Variable Load & Store (Store leaves assigned value on stack!)
  OP_GET_LOCAL,        // push locals[bc]
  OP_SET_LOCAL,        // locals[bc] = top (peek)
  OP_GET_INST,         // push me->instVars[bc]
  OP_SET_INST,         // me->instVars[bc] = top (peek)
  OP_GET_GLOBAL,       // push globals[symId = bc] (throws if undefined)

  // Destructuring Helper
  // Expects List on top of stack; checks len >= bc; pushes elements 0..bc-1
  // in reverse order so subsequent OP_SET_LOCAL / OP_SET_INST pop & store them,
  // while preserving original List below as expression result!
  OP_UNPACK_LIST,      // bc = required element count
  OP_POP_KEEP_UNDER,   // pops top, or used after unpack; see compiler

  // Collections & String Interpolation
  OP_MAKE_LIST,        // pops bc elements, pushes new List
  OP_MAKE_DICT,        // pops bc 2-element Lists, pushes new Dict
  OP_MAKE_BLK,         // push new Blk from sub-method consts[bc] + current Frame
  OP_STR_JAM,          // pops bc items, concatenates their `str` into a new Str

  // Fast-Path Binary Operators (fallback to OP_SEND when non-Num!)
  // a = callSiteIdx (for fallback cache), bc = selector SymId
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_MOD,
  OP_BIT_AND,
  OP_BIT_OR,
  OP_BIT_XOR,
  OP_EQ,
  OP_NE,
  OP_LT,
  OP_LE,
  OP_GT,
  OP_GE,

  // General Message Dispatch
  // a = numArgs, bc = callSiteIdx (method->sendSites[bc] holds symId + inline cache)
  OP_SEND,
  OP_SUPER_SEND,       // a = numArgs, bc = selector SymId

  // Control Flow & Inline Macros
  OP_JMP,              // pc += sbc
  OP_JMP_FALSE,        // pops cond; if !truth(cond) pc += sbc
  OP_JMP_TRUE,         // pops cond; if truth(cond)  pc += sbc
  OP_JMP_FALSE_PEEK,   // if !truth(top) pc += sbc (for short-circuit AND)
  OP_JMP_TRUE_PEEK,    // if truth(top)  pc += sbc (for short-circuit OR)
  OP_JMP_NOT_NIL_PEEK, // if top != nil  pc += sbc (for MAYBE..NULL)
  OP_CASE_MATCH,       // pops key; if equals(target_on_top, key) { pop target; pc += sbc; }

  // Zero-Allocation FOR Loop Iteration State (stored in 3 hidden temp locals at localBase = a)
  // locals[a+0] = normalized collection (Int n, List, or Dict pairs List)
  // locals[a+1] = Int length
  // locals[a+2] = Int current index (starts at 0)
  OP_FOR_PREP,         // a = tempBase; pops coll, initializes locals[a..a+2]
  OP_FOR_STEP_0,       // a = tempBase, sbc = exit offset; increments index (0 loop vars)
  OP_FOR_STEP_1,       // a = tempBase, sbc = exit offset; pushes val (1 loop var)
  OP_FOR_STEP_2,       // a = tempBase, sbc = exit offset; pushes key, then val (2 loop vars)

  // Exception Handling (TRY .. CATCH)
  OP_TRY_PUSH,         // pushes catch handler jumping to pc + sbc
  OP_TRY_POP,          // pops active catch handler

  // Return top of stack from method/block
  OP_RET,
};

static inline uint32_t encodeIns(Op op, uint8_t a, uint16_t bc) {
  return static_cast<uint32_t>(op) |
         (static_cast<uint32_t>(a) << 8) |
         (static_cast<uint32_t>(bc) << 16);
}

static inline uint32_t encodeInsSigned(Op op, uint8_t a, int16_t sbc) {
  return encodeIns(op, a, static_cast<uint16_t>(sbc));
}

static inline Op       insOp(uint32_t ins)  { return static_cast<Op>(ins & 0xFFu); }
static inline uint8_t  insA(uint32_t ins)   { return static_cast<uint8_t>((ins >> 8) & 0xFFu); }
static inline uint16_t insBC(uint32_t ins)  { return static_cast<uint16_t>(ins >> 16); }
static inline int16_t  insSBC(uint32_t ins) { return static_cast<int16_t>(ins >> 16); }

}  // namespace tt
