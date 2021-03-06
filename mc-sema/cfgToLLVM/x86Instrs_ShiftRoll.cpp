/*
Copyright (c) 2014, Trail of Bits
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

  Redistributions in binary form must reproduce the above copyright notice, this  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

  Neither the name of Trail of Bits nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "InstructionDispatch.h"
#include "toLLVM.h"
#include "X86.h"
#include "raiseX86.h"
#include "x86Helpers.h"
#include "x86Instrs_flagops.h"
#include "x86Instrs_ShiftRoll.h"

using namespace llvm;

template <int width> 
static Value *getBit(BasicBlock *b, Value *val, int which)
{
    TASSERT(which < width, "Bit width too big for getBit!");
    uint64_t mask_value = 1 << which;
    Value *mask = CONST_V<width>(b, mask_value);
    Value *sigbyte = BinaryOperator::CreateAnd(val, mask, "", b);
    Value *is_set = new ICmpInst(*b, CmpInst::ICMP_NE,
                                sigbyte, CONST_V<width>(b, 0));
    return is_set;
}

template <int width, Instruction::BinaryOps shift_op>
static Value *doShiftOp(InstPtr ip, 
        BasicBlock *b, 
        Value *src, 
        Value *count) 
{

    // get the masked count variable
    Value   *tempCOUNT = 
        BinaryOperator::CreateAnd(count, CONST_V<width>(b, 0x1F), "", b);

    // first time we'll shift count -1
    // so we can get the lsb/msb
    Value   *count_minus = 
        BinaryOperator::CreateSub(count, CONST_V<width>(b, 1), "", b);
    
    Value   *count_not_zero = new ICmpInst(*b,
                                CmpInst::ICMP_NE,
                                tempCOUNT,
                                CONST_V<width>(b, 0));
    // how much to shift the first time
    Value   *whichShift1 =
        SelectInst::Create( count_not_zero, 
                            count_minus,
                            CONST_V<width>(b, 0),
                            "",
                            b);
    // how much to shift a second time 
    // this is either 1 or 0 bits
    Value   *whichShift2 = 
        SelectInst::Create( count_not_zero, 
                            CONST_V<width>(b, 1),
                            CONST_V<width>(b, 0),
                            "",
                            b);

    // shift to count -1 bytes so we can extract
    // lsb or msb before its shifted out
    Value *shift_first = BinaryOperator::Create(
        shift_op,
        src,
        whichShift1,
        "",
        b);


    int mask_value = 1;
    switch(shift_op) {
        case Instruction::LShr:
        case Instruction::AShr:
            mask_value = 1;
            break;
        case Instruction::Shl:
            mask_value = 1 << (width-1);
            break;
        default:
            // assert;
            TASSERT(false, "Unknown operation given to doShiftOp");
            break;
    }

    // extract lsb or msb
    Value *mask = CONST_V<width>(b, mask_value);
    Value *sigbyte = BinaryOperator::CreateAnd(shift_first, mask, "", b);

    // if sigbyte is nonzero, then LSB or MSB is 1
    // else it is zero
    Value *maybe_CF = new ICmpInst(*b, 
            CmpInst::ICMP_NE,
            sigbyte, 
            CONST_V<width>(b, 0));
    Value *old_CF = F_READ(b, CF);
    Value *new_CF = SelectInst::Create(count_not_zero, 
            maybe_CF,
            old_CF,"", b);

    // shift out lsb or msb to complete the shift op
    Value *shift_second = BinaryOperator::Create(
        shift_op,
        shift_first,
        whichShift2,
        "",
        b);


    // OF RULES
    // COUNT == 1 and LEFT: OF = MSB(result) XOR CF
    // COUNT == 1 and SAR:  OF = 0
    // COUNT == 1 and SHR:  OF = MSB(OriginalDEST)
    // COUNT == 0:          UNCHANGED
    // COUNT  > 1:          OF = UNDEFINED
    //
    Value   *count_is_one = new ICmpInst(*b,
                                CmpInst::ICMP_EQ,
                                tempCOUNT,
                                CONST_V<width>(b, 1));
    Value *new_OF = nullptr;
    Value *old_OF = F_READ(b, OF);
    switch(shift_op) {
        case Instruction::Shl:
            {
                Value *v1 = getBit<width>(b, shift_second, width-1);
                Value *maybeOF = BinaryOperator::CreateXor(v1, new_CF, "", b);
                new_OF = SelectInst::Create(count_is_one, maybeOF, old_OF,"", b);
            }
            break;
        case Instruction::AShr:
            {
                Value *maybeOF = CONST_V<1>(b, 0);
                new_OF = SelectInst::Create(count_is_one, maybeOF, old_OF,"", b);
            }
            break;
        case Instruction::LShr:
            {
                Value *maybeOF = getBit<width>(b, src, width-1);
                new_OF = SelectInst::Create(count_is_one, maybeOF, old_OF,"", b);
            }
            break;
        default:
            // assert;
            TASSERT(false, "Unknown operation given to doShiftOp");
            break;
    }

    if(new_OF != nullptr) {
        F_WRITE(b, OF, new_OF);
    }


    F_WRITE(b, CF, new_CF);

    Value *old_ZF = F_READ(b, ZF);
    Value *maybe_ZF = new ICmpInst(*b, CmpInst::ICMP_EQ, shift_second, CONST_V<width>(b, 0));
    Value *new_ZF = SelectInst::Create(count_not_zero,
            maybe_ZF,
            old_ZF,"", b);
    F_WRITE(b, ZF, new_ZF);

    Value *old_SF = F_READ(b, SF);
    Value *maybe_SF = new ICmpInst(*b,
            ICmpInst::ICMP_SLT,
            shift_second,
            CONST_V<width>(b, 0));
    Value *new_SF = SelectInst::Create(count_not_zero,
            maybe_SF,
            old_SF,"", b);
    F_WRITE(b, SF, new_SF);

    Value *old_PF = F_READ(b, PF);
    WritePF<width>(b, shift_second);
    Value *maybe_PF = F_READ(b, PF);
    Value *new_PF = SelectInst::Create(count_not_zero,
            maybe_PF,
            old_PF,"", b);
    F_WRITE(b, PF, new_PF);

    return shift_second;
}

Value *doShrVV32(BasicBlock *&b, Value *src, Value *count) 
{
    return doShiftOp<32, Instruction::LShr>(InstPtr ((Inst*)(nullptr)), b, src, count);
}

template <int width>
static Value *doShldVV(InstPtr ip,
        BasicBlock *&b,
        unsigned dstReg,
        unsigned srcReg1,
        //unsigned shiftBy)
        Value* shiftBy)
{
    Type* widthTy = Type::getIntNTy(b->getContext(), width); 
    Type* doubleTy = Type::getIntNTy(b->getContext(), width*2); 

    // read left part
    Value   *from_left = R_READ<width>(b, dstReg);
    // and extend it to double width
    Value   *le = new ZExtInst(from_left, doubleTy, "", b);

    // read right part
    Value   *from_right = R_READ<width>(b, srcReg1);
    // and extend it to double width
    Value   *re = new ZExtInst(from_right, doubleTy, "", b);

    // put the left part on the left of a double-width int
    Value *v1 = BinaryOperator::CreateShl(
            le, 
            CONST_V<width*2>(b, width), 
            "", 
            b);

    // or the right part and left part to create
    // a complete double width thing to shift
    Value *from = BinaryOperator::CreateOr(v1, re, "", b);

    // read "how much to shift by"
    //Value   *imm = CONST_V<width*2>(b, shiftBy);
    Value   *imm = shiftBy;


    // do the shift
    Value *shift_res = doShiftOp<width*2, Instruction::Shl>(ip, b, from, imm);

    Value   *fix_it = BinaryOperator::CreateLShr(shift_res, CONST_V<width*2>(b, width), "", b);

    // truncate result to width type
    Value *reg_val = new TruncInst( fix_it, 
                                    widthTy,
                                    "", 
                                    b);

    // save result
    R_WRITE<width>(b, dstReg, reg_val);
    return reg_val;
}

template <int width>
static Value *doShrdVV(InstPtr ip,
        BasicBlock *&b,
        unsigned dstReg,
        unsigned srcReg1,
        //unsigned shiftBy)
        Value* shiftBy)
{
    Type* widthTy = Type::getIntNTy(b->getContext(), width); 
    Type* doubleTy = Type::getIntNTy(b->getContext(), width*2); 

    // read left part
    Value   *from_left = R_READ<width>(b, srcReg1);
    // and extend it to double width
    Value   *le = new ZExtInst(from_left, doubleTy, "", b);

    // read right part
    Value   *from_right = R_READ<width>(b, dstReg);
    // and extend it to double width
    Value   *re = new ZExtInst(from_right, doubleTy, "", b);

    // put the left part on the left of a double-width int
    Value *v1 = BinaryOperator::CreateShl(
            le, 
            CONST_V<width*2>(b, width), 
            "", 
            b);

    // or the right part and left part to create
    // a complete double width thing to shift
    Value *from = BinaryOperator::CreateOr(v1, re, "", b);

    // read "how much to shift by"
    //Value   *imm = CONST_V<width*2>(b, shiftBy);
    Value   *imm = shiftBy;


    // do the shift
    Value *shift_res = doShiftOp<width*2, Instruction::LShr>(ip, b, from, imm);

    // truncate result to width type
    Value *reg_val = new TruncInst( shift_res, 
                                    widthTy,
                                    "", 
                                    b);

    // save result
    R_WRITE<width>(b, dstReg, reg_val);
    return reg_val;
}

Value *ShrdVV32(BasicBlock *&b,
        unsigned dstReg,
        unsigned srcReg1,
        Value* shiftBy)
{

    return doShrdVV<32>(
            InstPtr((Inst*)(NULL)), 
            b, 
            dstReg, 
            srcReg1, 
            shiftBy);
}


template <int width>
static InstTransResult doShrdRI(InstPtr ip, BasicBlock *&b,
                        const MCOperand &dst,
                        const MCOperand &src1,
                        const MCOperand &src2)
{
    TASSERT(src2.isImm(), "");
    TASSERT(src1.isReg(), "");
    TASSERT(dst.isReg(), "");

    doShrdVV<width>(ip, b, 
            dst.getReg(), 
            src1.getReg(), 
            CONST_V<width*2>(b, src2.getImm()));
    
    return ContinueBlock;
}

template <int width>
static InstTransResult doShldRI(InstPtr ip, BasicBlock *&b,
                        const MCOperand &dst,
                        const MCOperand &src1,
                        const MCOperand &src2)
{
    TASSERT(src2.isImm(), "");
    TASSERT(src1.isReg(), "");
    TASSERT(dst.isReg(), "");

    doShldVV<width>(ip, b, 
            dst.getReg(), 
            src1.getReg(), 
            CONST_V<width*2>(b, src2.getImm()));
    
    return ContinueBlock;
}

template <int width>
static InstTransResult doShldRCL(InstPtr ip, BasicBlock *&b,
                        const MCOperand &dst,
                        const MCOperand &src1)
{
    TASSERT(src1.isReg(), "");
    TASSERT(dst.isReg(), "");

    Type* doubleTy = Type::getIntNTy(b->getContext(), width*2); 

    Value   *count = R_READ<width>(b, X86::CL);
    // ShrdVV needs a 64-bit count
    Value   *extCount = new ZExtInst(count, doubleTy, "", b);

    doShldVV<width>(ip, b, 
            dst.getReg(), 
            src1.getReg(), 
            extCount);
    
    return ContinueBlock;
}

template <int width>
static InstTransResult doShrdRCL(InstPtr ip, BasicBlock *&b,
                        const MCOperand &dst,
                        const MCOperand &src1)
{
    TASSERT(src1.isReg(), "");
    TASSERT(dst.isReg(), "");

    Type* doubleTy = Type::getIntNTy(b->getContext(), width*2); 

    Value   *count = R_READ<width>(b, X86::CL);
    // ShrdVV needs a 64-bit count
    Value   *extCount = new ZExtInst(count, doubleTy, "", b);

    doShrdVV<width>(ip, b, 
            dst.getReg(), 
            src1.getReg(), 
            extCount);
    
    return ContinueBlock;
}

template <int width>
static InstTransResult doShrRI(InstPtr ip, BasicBlock *&b,
                        const MCOperand &src1,
                        const MCOperand &src2,
                        const MCOperand &dst)
{
    TASSERT(src1.isReg(), "");
    TASSERT(src2.isImm(), "");
    TASSERT(dst.isReg(), "");

    Value   *fromReg = R_READ<width>(b, src1.getReg());
    Value   *imm = CONST_V<width>(b, src2.getImm());

    Value *res = doShiftOp<width, Instruction::LShr>(ip, b, fromReg, imm);

    R_WRITE<width>(b, dst.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShrR1(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg)
{
    TASSERT(reg.isReg(), "");

    Value   *fromReg = R_READ<width>(b, reg.getReg());
    Value   *count = CONST_V<width>(b, 1);

    Value *res = doShiftOp<width, Instruction::LShr>(ip, b, fromReg, count);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShrRCL(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg)
{
    TASSERT(reg.isReg(), "");

    Value   *fromReg = R_READ<width>(b, reg.getReg());
    Value   *count = R_READ<width>(b, X86::CL);

    Value *res = doShiftOp<width, Instruction::LShr>(ip, b, fromReg, count);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShrMI(InstPtr ip, BasicBlock *&b,
                            Value   *addr,
                            const MCOperand &imm)
{
    TASSERT(addr != NULL, "");
    TASSERT(imm.isImm(), "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *count = CONST_V<width>(b, imm.getImm());

    Value *res = doShiftOp<width, Instruction::LShr>(ip, b, dst, count);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShrMV(InstPtr ip, BasicBlock *&b,
                            Value   *addr,
                            Value   *rhs)
{
    TASSERT(addr != NULL, "");
    TASSERT(rhs != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);

    Value *res = doShiftOp<width, Instruction::LShr>(ip, b, dst, rhs);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShrM1(InstPtr ip, BasicBlock *&b,
                        Value       *addr)
{
    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *count = CONST_V<width>(b, 1);

    Value *res = doShiftOp<width, Instruction::LShr>(ip, b, dst, count);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShrMCL(InstPtr ip, BasicBlock *&b,
                            Value   *addr)
{
    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *count = R_READ<width>(b, X86::CL);

    Value *res = doShiftOp<width, Instruction::LShr>(ip, b, dst, count);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShlRI(InstPtr ip, BasicBlock *&b,
                        const MCOperand &src1,
                        const MCOperand &src2,
                        const MCOperand &dst)
{
    TASSERT(src1.isReg(), "");
    TASSERT(src2.isImm(), "");
    TASSERT(dst.isReg(), "");

    Value   *fromReg = R_READ<width>(b, src1.getReg());
    Value   *imm = CONST_V<width>(b, src2.getImm());

    Value *res = doShiftOp<width, Instruction::Shl>(ip, b, fromReg, imm);

    R_WRITE<width>(b, dst.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShlR1(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg)
{
    TASSERT(reg.isReg(), "");

    Value   *fromReg = R_READ<width>(b, reg.getReg());
    Value   *count = CONST_V<width>(b, 1);

    Value *res = doShiftOp<width, Instruction::Shl>(ip, b, fromReg, count);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShlRCL(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg)
{
    TASSERT(reg.isReg(), "");

    Value   *fromReg = R_READ<width>(b, reg.getReg());
    Value   *count = R_READ<width>(b, X86::CL);

    Value *res = doShiftOp<width, Instruction::Shl>(ip, b, fromReg, count);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShlMI(InstPtr ip, BasicBlock *&b,
                            Value   *addr,
                            const MCOperand &imm)
{
    TASSERT(addr != NULL, "");
    TASSERT(imm.isImm(), "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *count = CONST_V<width>(b, imm.getImm());

    Value *res = doShiftOp<width, Instruction::Shl>(ip, b, dst, count);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShlMV(InstPtr ip, BasicBlock *&b,
                            Value   *addr,
                            Value   *rhs)
{
    TASSERT(addr != NULL, "");
    TASSERT(rhs != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);

    Value *res = doShiftOp<width, Instruction::Shl>(ip, b, dst, rhs);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShlM1(InstPtr ip, BasicBlock *&b,
                        Value       *addr)
{
    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *count = CONST_V<width>(b, 1);

    Value *res = doShiftOp<width, Instruction::Shl>(ip, b, dst, count);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doShlMCL(InstPtr ip, BasicBlock *&b,
                            Value   *addr)
{
    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *count = R_READ<width>(b, X86::CL);

    Value *res = doShiftOp<width, Instruction::Shl>(ip, b, dst, count);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doSarRI(InstPtr ip, BasicBlock *&b,
                        const MCOperand &src1,
                        const MCOperand &src2,
                        const MCOperand &dst)
{
    TASSERT(src1.isReg(), "");
    TASSERT(src2.isImm(), "");
    TASSERT(dst.isReg(), "");

    Value   *fromReg = R_READ<width>(b, src1.getReg());
    Value   *imm = CONST_V<width>(b, src2.getImm());

    Value *res = doShiftOp<width, Instruction::AShr>(ip, b, fromReg, imm);

    R_WRITE<width>(b, dst.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doSarR1(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg)
{
    TASSERT(reg.isReg(), "");

    Value   *fromReg = R_READ<width>(b, reg.getReg());
    Value   *count = CONST_V<width>(b, 1);

    Value *res = doShiftOp<width, Instruction::AShr>(ip, b, fromReg, count);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doSarRCL(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg)
{
    TASSERT(reg.isReg(), "");

    Value   *fromReg = R_READ<width>(b, reg.getReg());
    Value   *count = R_READ<width>(b, X86::CL);

    Value *res = doShiftOp<width, Instruction::AShr>(ip, b, fromReg, count);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doSarMI(InstPtr ip, BasicBlock *&b,
                            Value   *addr,
                            const MCOperand &imm)
{
    TASSERT(addr != NULL, "");
    TASSERT(imm.isImm(), "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *count = CONST_V<width>(b, imm.getImm());

    Value *res = doShiftOp<width, Instruction::AShr>(ip, b, dst, count);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doSarMV(InstPtr ip, BasicBlock *&b,
                            Value   *addr,
                            Value   *rhs)
{
    TASSERT(addr != NULL, "");
    TASSERT(rhs != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);

    Value *res = doShiftOp<width, Instruction::AShr>(ip, b, dst, rhs);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doSarM1(InstPtr ip, BasicBlock *&b,
                        Value       *addr)
{
    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *count = CONST_V<width>(b, 1);

    Value *res = doShiftOp<width, Instruction::AShr>(ip, b, dst, count);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doSarMCL(InstPtr ip, BasicBlock *&b,
                            Value   *addr)
{
    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *count = R_READ<width>(b, X86::CL);

    Value *res = doShiftOp<width, Instruction::AShr>(ip, b, dst, count);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static Value *doRclVV(InstPtr ip, BasicBlock *&b, Value *dst, Value *count) {
    Function    *F = b->getParent();

    //create basic blocks to define the branching behavior 
    BasicBlock  *loopHeader = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *whileBody = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *afterWhile = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *singleBit = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *nonSingleBit = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *rest = BasicBlock::Create(F->getContext(), "", F);

    Type    *t;

    switch(width) {
        case 8:
            t = Type::getInt8Ty(b->getContext());
            break;
        case 16:
            t = Type::getInt16Ty(b->getContext());
            break;
        case 32:
            t = Type::getInt32Ty(b->getContext());
            break;
        default:
            throw TErr(__LINE__, __FILE__, "Width not supported");
    }

    Value   *tempCount;

    switch(width) {
        case 8:
            tempCount = BinaryOperator::Create( Instruction::SRem, 
                                                BinaryOperator::CreateAnd(count, CONST_V<width>(b, 0x1F), "", b), 
                                                CONST_V<width>(b, 9), 
                                                "", 
                                                b);
            break;
        case 16:
            tempCount = BinaryOperator::Create( Instruction::SRem, 
                                                BinaryOperator::CreateAnd(count, CONST_V<width>(b, 0x1F), "", b), 
                                                CONST_V<width>(b, 17), 
                                                "", 
                                                b);
            break;
        case 32:
            tempCount = BinaryOperator::CreateAnd(count, CONST_V<width>(b, 0x1F), "", b);
            break;
        case 64:
            tempCount = BinaryOperator::CreateAnd(count, CONST_V<width>(b, 0x3F), "", b);
            break;
        default:
            break;
    }

    BranchInst::Create(loopHeader, b);

    //while header
    // create PHI values
    PHINode   *dst_phi = PHINode::Create(Type::getIntNTy(b->getContext(), width),
                                    2,
                                    "",
                                    loopHeader);

    PHINode     *tempCount_phi = PHINode::Create(  Type::getIntNTy(b->getContext(), width),
                                            2,
                                            "",
                                            loopHeader);
    
    // set initial PHI values
    dst_phi->addIncoming(dst, b);
    tempCount_phi->addIncoming(tempCount, b);
    //check if tempCount == 0
    Value   *cmpRes = new ICmpInst(*loopHeader, CmpInst::ICMP_EQ, tempCount_phi, CONST_V<width>(b, 0));
    BranchInst::Create(afterWhile, whileBody, cmpRes, loopHeader);

    //while body

    //tempCF = MSB(dst)
    Value   *tempCF = BinaryOperator::CreateLShr(dst_phi, CONST_V<width>(b, width-1), "", whileBody);

    //dst = (dst*2) + tempCF
    Value   *tempDst = BinaryOperator::Create(Instruction::Mul, dst_phi, CONST_V<width>(b, 2), "", whileBody);
    Value   *cf_bit = F_READ(whileBody, CF);
    Value   *cf_zx = new ZExtInst(cf_bit, t, "", whileBody);
    Value   *newDst = BinaryOperator::CreateAdd(tempDst, cf_zx, "", whileBody);

    //CF = tempCF
    Value   *tempCF_trunc = new TruncInst(tempCF, Type::getInt1Ty(b->getContext()), "", whileBody);
    F_WRITE(whileBody, CF, tempCF_trunc);

    //tempCount -= 1
    Value   *newCount = BinaryOperator::CreateSub(tempCount_phi, CONST_V<width>(b, 1), "", whileBody);

    // update PHI values
    dst_phi->addIncoming(newDst, whileBody);
    tempCount_phi->addIncoming(newCount, whileBody);

    //branch back to the loopheader to check tempCount
    BranchInst::Create(loopHeader, whileBody);

    Value   *rotateType = new ICmpInst(*afterWhile, CmpInst::ICMP_EQ, count, CONST_V<width>(b, 1));
    BranchInst::Create(singleBit, nonSingleBit, rotateType, afterWhile);

    //if it is single bit, set OF to MSB(dst) XOR CF
    Value   *msb = new TruncInst(   BinaryOperator::CreateLShr(dst_phi, CONST_V<width>(b, width-1), "", singleBit), 
                                    Type::getInt1Ty(b->getContext()), 
                                    "", 
                                    singleBit);
    Value   *cf = F_READ(singleBit, CF);
    Value   *xorRes = BinaryOperator::CreateXor(msb, cf, "", singleBit);
    F_WRITE(singleBit, OF, xorRes);
    BranchInst::Create(rest, singleBit);

    //if it is not single bit, zap OF
    //F_ZAP(nonSingleBit, OF);
    F_SET(nonSingleBit, OF); //OF Set to match testSemantics
    BranchInst::Create(rest, nonSingleBit);

    b = rest;

    return dst_phi;
}

template <int width>
static InstTransResult doRclM1(InstPtr ip, BasicBlock *&b,
                        Value           *addr) 
{

    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *imm = CONST_V<width>(b, 1);

    Value   *res = doRclVV<width>(ip, b, dst, imm);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRclMI(InstPtr ip, BasicBlock *&b,
                        Value           *addr,
                        const MCOperand &count) 
{

    TASSERT(addr != NULL, "");
    TASSERT(count.isImm(), "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *imm = CONST_V<width>(b, count.getImm());

    Value   *res = doRclVV<width>(ip, b, dst, imm);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRclMV(InstPtr ip, BasicBlock *&b,
                        Value           *addr,
                        Value           *rhs)
{

    TASSERT(addr != NULL, "");
    TASSERT(rhs != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);

    Value   *res = doRclVV<width>(ip, b, dst, rhs);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRclMCL(InstPtr ip, BasicBlock *&b,
                        Value           *addr) 
{

    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *cl = R_READ<width>(b, X86::CL);

    Value   *res = doRclVV<width>(ip, b, dst, cl);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRclR1(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg) 
{

    TASSERT(reg.isReg(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *imm = CONST_V<width>(b, 1);

    Value   *res = doRclVV<width>(ip, b, dst, imm);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRclRI(InstPtr ip, BasicBlock *&b,
                        const MCOperand &dst1,
                        const MCOperand &reg,
                        const MCOperand &count) 
{

    TASSERT(dst1.isReg(), "");
    TASSERT(reg.isReg(), "");
    TASSERT(count.isImm(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *imm = CONST_V<width>(b, count.getImm());

    Value   *res = doRclVV<width>(ip, b, dst, imm);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRclRCL(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg) 
{

    TASSERT(reg.isReg(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *cl = R_READ<width>(b, X86::CL);

    Value   *res = doRclVV<width>(ip, b, dst, cl);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static Value *doRcrVV(InstPtr ip, BasicBlock *&b, Value *dst, Value *count) {
    Function    *F = b->getParent();

    //create basic blocks to define the branching behavior 
    BasicBlock  *loopHeader = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *preHeader = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *whileBody = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *afterWhile = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *singleBit = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *nonSingleBit = BasicBlock::Create(F->getContext(), "", F);

    Type    *t;

    switch(width) {
        case 8:
            t = Type::getInt8Ty(b->getContext());
            break;
        case 16:
            t = Type::getInt16Ty(b->getContext());
            break;
        case 32:
            t = Type::getInt32Ty(b->getContext());
            break;
        default:
            throw TErr(__LINE__, __FILE__, "Width not supported");
    }

    Value   *rotateType = new ICmpInst(*b, CmpInst::ICMP_EQ, count, CONST_V<width>(b, 1));
    BranchInst::Create(singleBit, nonSingleBit, rotateType, b);

    //single rotate condition
    Value   *msb = new TruncInst(   BinaryOperator::CreateLShr(dst, CONST_V<width>(b, width-1), "", singleBit), 
                                    Type::getInt1Ty(b->getContext()), 
                                    "", 
                                    singleBit);
    Value   *cf = F_READ(singleBit, CF);
    F_WRITE(singleBit, OF, BinaryOperator::CreateXor(msb, cf, "", singleBit));
    BranchInst::Create(preHeader, singleBit);

    //non-single rotate condition
    //F_ZAP(nonSingleBit, OF);
    F_SET(nonSingleBit, OF); //OF Set to match testSemantics
    BranchInst::Create(preHeader, nonSingleBit);

    Value   *tempCount;

    switch(width) {
        case 8:
            tempCount = BinaryOperator::Create( Instruction::SRem, 
                                                BinaryOperator::CreateAnd(count, CONST_V<width>(b, 0x1F), "", preHeader), 
                                                CONST_V<width>(b, 9), 
                                                "", 
                                                preHeader);
            break;
        case 16:
            tempCount = BinaryOperator::Create( Instruction::SRem, 
                                                BinaryOperator::CreateAnd(count, CONST_V<width>(b, 0x1F), "", preHeader), 
                                                CONST_V<width>(b, 17), 
                                                "", 
                                                preHeader);
            break;
        case 32:
            tempCount = BinaryOperator::CreateAnd(count, CONST_V<width>(b, 0x1F), "", preHeader);
            break;
        case 64:
            tempCount = BinaryOperator::CreateAnd(count, CONST_V<width>(b, 0x3F), "", preHeader);
            break;
        default:
            break;
    }

    BranchInst::Create(loopHeader, preHeader);

    //while header
    // create PHI values
    PHINode   *dst_phi = PHINode::Create(Type::getIntNTy(b->getContext(), width),
                                    2,
                                    "",
                                    loopHeader);

    PHINode     *tempCount_phi = PHINode::Create(  Type::getIntNTy(b->getContext(), width),
                                            2,
                                            "",
                                            loopHeader);
    
    // set initial PHI values
    dst_phi->addIncoming(dst, preHeader);
    tempCount_phi->addIncoming(tempCount, preHeader);
    //check if tempCount == 0
    Value   *cmpRes = new ICmpInst(*loopHeader, CmpInst::ICMP_EQ, tempCount_phi, CONST_V<width>(b, 0));
    BranchInst::Create(afterWhile, whileBody, cmpRes, loopHeader);

    //while body

    //tempCF = LSB(dst)
    Value   *tempCF = BinaryOperator::CreateAnd(dst_phi, CONST_V<width>(b, 1), "", whileBody);

    //dst = (dst/2) + (CF*2^width)
    Value   *tempDst = BinaryOperator::CreateLShr(dst_phi, CONST_V<width>(b, 1), "", whileBody);
    Value   *cf_bit = F_READ(whileBody, CF);
    Value   *cf_zx = new ZExtInst(cf_bit, t, "", whileBody);
    Value   *multiplier = BinaryOperator::CreateShl(cf_zx, CONST_V<width>(b, width-1), "", whileBody);
    Value   *newDst = BinaryOperator::CreateAdd(tempDst, multiplier, "", whileBody);

    //CF = tempCF
    Value   *tempCF_trunc = new TruncInst(tempCF, Type::getInt1Ty(b->getContext()), "", whileBody);
    F_WRITE(whileBody, CF, tempCF_trunc);

    //tempCount -= 1
    Value   *newCount = BinaryOperator::CreateSub(tempCount_phi, CONST_V<width>(b, 1), "", whileBody);

    // update PHI values
    dst_phi->addIncoming(newDst, whileBody);
    tempCount_phi->addIncoming(newCount, whileBody);

    //branch back to the loopheader to check tempCount
    BranchInst::Create(loopHeader, whileBody);

    b = afterWhile;

    return dst_phi;
}

template <int width>
static InstTransResult doRcrM1(InstPtr ip, BasicBlock *&b,
                        Value           *addr) 
{

    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *imm = CONST_V<width>(b, 1);

    Value   *res = doRcrVV<width>(ip, b, dst, imm);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRcrMI(InstPtr ip, BasicBlock *&b,
                        Value           *addr,
                        const MCOperand &count) 
{

    TASSERT(addr != NULL, "");
    TASSERT(count.isImm(), "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *imm = CONST_V<width>(b, count.getImm());

    Value   *res = doRcrVV<width>(ip, b, dst, imm);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRcrMV(InstPtr ip, BasicBlock *&b,
                        Value           *addr,
                        Value           *rhs)
{

    TASSERT(addr != NULL, "");
    TASSERT(rhs != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);

    Value   *res = doRcrVV<width>(ip, b, dst, rhs);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRcrMCL(InstPtr ip, BasicBlock *&b,
                        Value           *addr) 
{

    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *cl = R_READ<width>(b, X86::CL);

    Value   *res = doRcrVV<width>(ip, b, dst, cl);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRcrR1(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg) 
{

    TASSERT(reg.isReg(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *imm = CONST_V<width>(b, 1);

    Value   *res = doRcrVV<width>(ip, b, dst, imm);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRcrRI(InstPtr ip, BasicBlock *&b,
                        const MCOperand &dst1,
                        const MCOperand &reg,
                        const MCOperand &count) 
{

    TASSERT(dst1.isReg(), "");
    TASSERT(reg.isReg(), "");
    TASSERT(count.isImm(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *imm = CONST_V<width>(b, count.getImm());

    Value   *res = doRcrVV<width>(ip, b, dst, imm);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRcrRCL(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg) 
{

    TASSERT(reg.isReg(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *cl = R_READ<width>(b, X86::CL);

    Value   *res = doRcrVV<width>(ip, b, dst, cl);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static Value *doRolVV(InstPtr ip, BasicBlock *&b, Value *dst, Value *count) {
    Function    *F = b->getParent();

    //create basic blocks to define the branching behavior 
    BasicBlock  *loopHeader = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *whileBody = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *afterWhile = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *singleBit = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *nonSingleBit = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *rest = BasicBlock::Create(F->getContext(), "", F);

    Value   *countMask;
    switch(width) {
        case 64:
            countMask = CONST_V<width>(b, 0x3F);
            break;
        default:
            countMask = CONST_V<width>(b, 0x1F);
            break;
    }

    Value   *andRes = BinaryOperator::CreateAnd(count, countMask, "", b);
    Value   *tempCount = BinaryOperator::Create(Instruction::SRem, andRes, CONST_V<width>(b, width), "", b);

    BranchInst::Create(loopHeader, b);

    //while header
    // create PHI values
    PHINode   *dst_phi = PHINode::Create(Type::getIntNTy(b->getContext(), width),
                                    2,
                                    "",
                                    loopHeader);

    PHINode     *tempCount_phi = PHINode::Create(  Type::getIntNTy(b->getContext(), width),
                                            2,
                                            "",
                                            loopHeader);
    
    PHINode     *CF_phi = PHINode::Create(  Type::getIntNTy(b->getContext(), width),
                                            2,
                                            "",
                                            loopHeader);
    // set initial PHI values
    dst_phi->addIncoming(dst, b);
    tempCount_phi->addIncoming(tempCount, b);
    CF_phi->addIncoming(CONST_V<width>(b, 0), b);
    //check if tempCount == 0
    Value   *cmpRes = new ICmpInst(*loopHeader, CmpInst::ICMP_EQ, tempCount_phi, CONST_V<width>(b, 0));
    BranchInst::Create(afterWhile, whileBody, cmpRes, loopHeader);

    //while body

    //tempCF = MSB(dst)
    Value   *tempCF = BinaryOperator::CreateLShr(dst_phi, CONST_V<width>(b, width-1), "", whileBody);

    //dst = (dst_phi*2) + tempCF
    Value   *tempDst = BinaryOperator::CreateAdd(BinaryOperator::Create(Instruction::Mul, 
                                                                        dst_phi, 
                                                                        CONST_V<width>(b, 2), 
                                                                        "", 
                                                                        whileBody),
                                    tempCF, 
                                    "", 
                                    whileBody);

    //tempCount -= 1
    Value   *newCount = BinaryOperator::CreateSub(tempCount_phi, CONST_V<width>(b, 1), "", whileBody);
    dst_phi->addIncoming(tempDst, whileBody);
    CF_phi->addIncoming(tempCF, whileBody);
    tempCount_phi->addIncoming(newCount, whileBody);

    //branch back to the loopheader to check tempCount_phi
    BranchInst::Create(loopHeader, whileBody);

    //write the CF with the LSB of dst
    Value   *lsb = new TruncInst(   BinaryOperator::CreateAnd(dst_phi, CONST_V<width>(b, 1), "", afterWhile), 
                                    Type::getInt1Ty(b->getContext()), 
                                    "", 
                                    afterWhile);
    F_WRITE(afterWhile, CF, lsb);

    Value   *rotateType = new ICmpInst(*afterWhile, CmpInst::ICMP_EQ, andRes, CONST_V<width>(b, 1));
    BranchInst::Create(singleBit, nonSingleBit, rotateType, afterWhile);

    //if it is single bit, set OF to MSB(dst) XOR LSB(dst)
    Value   *msb = new TruncInst(   BinaryOperator::CreateLShr(dst_phi, CONST_V<width>(b, width-1), "", singleBit), 
                                    Type::getInt1Ty(b->getContext()), 
                                    "", 
                                    singleBit);
    Value   *xorRes = BinaryOperator::CreateXor(msb, lsb, "", singleBit);
    F_WRITE(singleBit, OF, xorRes);
    BranchInst::Create(rest, singleBit);

    //if it is not single bit, zap OF
    //F_ZAP(nonSingleBit, OF);
    F_SET(nonSingleBit, OF); //OF Set to match testSemantics
    BranchInst::Create(rest, nonSingleBit);

    b = rest;

    return dst_phi;
}

template <int width>
static InstTransResult doRolM1(InstPtr ip, BasicBlock *&b,
                        Value           *addr) 
{

    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *imm = CONST_V<width>(b, 1);

    Value   *res = doRolVV<width>(ip, b, dst, imm);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRolMI(InstPtr ip, BasicBlock *&b,
                        Value           *addr,
                        const MCOperand &count) 
{

    TASSERT(addr != NULL, "");
    TASSERT(count.isImm(), "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *imm = CONST_V<width>(b, count.getImm());

    Value   *res = doRolVV<width>(ip, b, dst, imm);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRolMV(InstPtr ip, BasicBlock *&b,
                        Value           *addr,
                        Value           *rhs)
{

    TASSERT(addr != NULL, "");
    TASSERT(rhs != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);

    Value   *res = doRolVV<width>(ip, b, dst, rhs);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRolMCL(InstPtr ip, BasicBlock *&b,
                        Value           *addr) 
{

    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *cl = R_READ<width>(b, X86::CL);

    Value   *res = doRolVV<width>(ip, b, dst, cl);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRolR1(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg) 
{

    TASSERT(reg.isReg(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *imm = CONST_V<width>(b, 1);

    Value   *res = doRolVV<width>(ip, b, dst, imm);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRolRI(InstPtr ip, BasicBlock *&b,
                        const MCOperand &dst1,
                        const MCOperand &reg,
                        const MCOperand &count) 
{

    TASSERT(dst1.isReg(), "");
    TASSERT(reg.isReg(), "");
    TASSERT(count.isImm(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *imm = CONST_V<width>(b, count.getImm());

    Value   *res = doRolVV<width>(ip, b, dst, imm);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRolRCL(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg) 
{

    TASSERT(reg.isReg(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *cl = R_READ<width>(b, X86::CL);

    Value   *res = doRolVV<width>(ip, b, dst, cl);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static Value *doRorVV(InstPtr ip, BasicBlock *&b, Value *dst, Value *count) {
    Function    *F = b->getParent();

    //create basic blocks to define the branching behavior 
    BasicBlock  *loopHeader = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *whileBody = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *afterWhile = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *singleBit = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *nonSingleBit = BasicBlock::Create(F->getContext(), "", F);
    BasicBlock  *rest = BasicBlock::Create(F->getContext(), "", F);

    Value   *countMask;
    switch(width) {
        case 64:
            countMask = CONST_V<width>(b, 0x3F);
            break;
        default:
            countMask = CONST_V<width>(b, 0x1F);
            break;
    }

    Value   *andRes = BinaryOperator::CreateAnd(count, countMask, "", b);
    Value   *tempCount = BinaryOperator::Create(Instruction::SRem, andRes, CONST_V<width>(b, width), "", b);

    BranchInst::Create(loopHeader, b);

    //while header
    // create PHI values
    PHINode   *dst_phi = PHINode::Create(Type::getIntNTy(b->getContext(), width),
                                    2,
                                    "",
                                    loopHeader);

    PHINode     *tempCount_phi = PHINode::Create(  Type::getIntNTy(b->getContext(), width),
                                            2,
                                            "",
                                            loopHeader);
    
    PHINode     *CF_phi = PHINode::Create(  Type::getIntNTy(b->getContext(), width),
                                            2,
                                            "",
                                            loopHeader);
    // set initial PHI values
    dst_phi->addIncoming(dst, b);
    tempCount_phi->addIncoming(tempCount, b);
    CF_phi->addIncoming(CONST_V<width>(b, 0), b);
    //check if tempCount == 0
    Value   *cmpRes = new ICmpInst(*loopHeader, CmpInst::ICMP_EQ, tempCount_phi, CONST_V<width>(b, 0));
    BranchInst::Create(afterWhile, whileBody, cmpRes, loopHeader);

    //while body

    //tempCF = LSB(dst)
    Value   *tempCF = BinaryOperator::CreateAnd(dst_phi, CONST_V<width>(b, 1), "", whileBody);

    //dst = (dst/2) + (tempCF*2^width)
    //Value   *tempDst = BinaryOperator::Create(Instruction::SDiv, dst_phi, CONST_V<width>(b, 2), "", whileBody);
    Value   *tempDst = BinaryOperator::Create(Instruction::LShr, dst_phi, CONST_V<width>(b, 1), "", whileBody);
    Value   *multiplier = BinaryOperator::CreateShl(tempCF, CONST_V<width>(b, width-1), "", whileBody);
    Value   *newDst = BinaryOperator::CreateAdd(tempDst, multiplier, "", whileBody);

    //tempCount -= 1
    Value   *newCount = BinaryOperator::CreateSub(tempCount_phi, CONST_V<width>(b, 1), "", whileBody);

    //update PHI values
    dst_phi->addIncoming(newDst, whileBody);
    CF_phi->addIncoming(tempCF, whileBody);
    tempCount_phi->addIncoming(newCount, whileBody);

    //branch back to the loopheader to check tempCount
    BranchInst::Create(loopHeader, whileBody);

    //write the CF with the MSB
    Value   *msb = new TruncInst(   BinaryOperator::CreateLShr(dst_phi, CONST_V<width>(b, width-1), "", afterWhile), 
                                    Type::getInt1Ty(b->getContext()), 
                                    "", 
                                    afterWhile);
    F_WRITE(afterWhile, CF, msb);

    Value   *rotateType = new ICmpInst(*afterWhile, CmpInst::ICMP_EQ, andRes, CONST_V<width>(b, 1));
    BranchInst::Create(singleBit, nonSingleBit, rotateType, afterWhile);

    //if it is single bit, set OF to MSB(dst) XOR MSB_1(dst)
    Value   *msb_1 = new TruncInst( BinaryOperator::CreateLShr(dst_phi, CONST_V<width>(b, width-2), "", singleBit), 
                                    Type::getInt1Ty(b->getContext()), 
                                    "", 
                                    singleBit);
    Value   *xorRes = BinaryOperator::CreateXor(msb, msb_1, "", singleBit);
    F_WRITE(singleBit, OF, xorRes);
    BranchInst::Create(rest, singleBit);

    //if it is not single bit, zap OF
    F_ZAP(nonSingleBit, OF);
    //F_SET(nonSingleBit, OF); //OF Set to match testSemantics
    BranchInst::Create(rest, nonSingleBit);

    b = rest;

    return dst_phi;
}

template <int width>
static InstTransResult doRorM1(InstPtr ip, BasicBlock *&b,
                        Value           *addr) 
{

    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *imm = CONST_V<width>(b, 1);

    Value   *res = doRorVV<width>(ip, b, dst, imm);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRorMI(InstPtr ip, BasicBlock *&b,
                        Value           *addr,
                        const MCOperand &count) 
{

    TASSERT(addr != NULL, "");
    TASSERT(count.isImm(), "");

    Value   *dst = M_READ<width>(ip, b, addr);
    Value   *imm = CONST_V<width>(b, count.getImm());

    Value   *res = doRorVV<width>(ip, b, dst, imm);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRorMV(InstPtr ip, BasicBlock *&b,
                        Value           *addr,
                        Value           *rhs)
{

    TASSERT(addr != NULL, "");
    TASSERT(rhs != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);

    Value   *res = doRorVV<width>(ip, b, dst, rhs);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRorMCL(InstPtr ip, BasicBlock *&b,
                        Value           *addr) 
{

    TASSERT(addr != NULL, "");

    Value   *dst = M_READ<width>(ip, b, addr);
    
    Value   *cl = R_READ<width>(b, X86::CL);

    Value   *res = doRorVV<width>(ip, b, dst, cl);

    M_WRITE<width>(ip, b, addr, res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRorR1(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg) 
{

    TASSERT(reg.isReg(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *imm = CONST_V<width>(b, 1);

    Value   *res = doRorVV<width>(ip, b, dst, imm);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRorRI(InstPtr ip, BasicBlock *&b,
                        const MCOperand &dst1,
                        const MCOperand &reg,
                        const MCOperand &count) 
{

    TASSERT(dst1.isReg(), "");
    TASSERT(reg.isReg(), "");
    TASSERT(count.isImm(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());
    Value   *imm = CONST_V<width>(b, count.getImm());

    Value   *res = doRorVV<width>(ip, b, dst, imm);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

template <int width>
static InstTransResult doRorRCL(InstPtr ip, BasicBlock *&b,
                        const MCOperand &reg) 
{

    TASSERT(reg.isReg(), "");

    Value   *dst = R_READ<width>(b, reg.getReg());

    Value   *cl = R_READ<width>(b, X86::CL);

    Value   *res = doRorVV<width>(ip, b, dst, cl);

    R_WRITE<width>(b, reg.getReg(), res);

    return ContinueBlock;
}

GENERIC_TRANSLATION_MEM(RCL8m1, 
	doRclM1<8>(ip, block, ADDR(0)),
	doRclM1<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(RCL8mCL, 
	doRclMCL<8>(ip, block, ADDR(0)),
	doRclMCL<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(RCL8mi, 
	doRclMI<8>(ip, block, ADDR(0), OP(5)),
	doRclMI<8>(ip, block, STD_GLOBAL_OP(0), OP(5)))
GENERIC_TRANSLATION(RCL8r1, doRclR1<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCL8rCL, doRclRCL<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCL8ri, doRclRI<8>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(RCL16m1, 
	doRclM1<16>(ip, block, ADDR(0)),
	doRclM1<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(RCL16mCL, 
	doRclMCL<16>(ip, block, ADDR(0)),
	doRclMCL<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(RCL16mi, 
	doRclMI<16>(ip, block, ADDR(0), OP(5)),
	doRclMI<16>(ip, block, STD_GLOBAL_OP(0), OP(5)))
GENERIC_TRANSLATION(RCL16r1, doRclR1<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCL16rCL, doRclRCL<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCL16ri, doRclRI<16>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(RCL32m1, 
	doRclM1<32>(ip, block, ADDR(0)),
	doRclM1<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(RCL32mCL, 
	doRclMCL<32>(ip, block, ADDR(0)),
	doRclMCL<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_32MI(RCL32mi, 
	doRclMI<32>(ip, block, ADDR(0), OP(5)),
	doRclMI<32>(ip, block, STD_GLOBAL_OP(0), OP(5)),
    doRclMV<32>(ip, block, ADDR_NOREF(0), GLOBAL_DATA_OFFSET(block, natM, ip)))

GENERIC_TRANSLATION(RCL32r1, doRclR1<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCL32rCL, doRclRCL<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCL32ri, doRclRI<32>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(RCR8m1, 
	doRcrM1<8>(ip, block, ADDR(0)),
	doRcrM1<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(RCR8mCL, 
	doRcrMCL<8>(ip, block, ADDR(0)),
	doRcrMCL<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(RCR8mi, 
	doRcrMI<8>(ip, block, ADDR(0), OP(5)),
	doRcrMI<8>(ip, block, STD_GLOBAL_OP(0), OP(5)))
GENERIC_TRANSLATION(RCR8r1, doRcrR1<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCR8rCL, doRcrRCL<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCR8ri, doRcrRI<8>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(RCR16m1, 
	doRcrM1<16>(ip, block, ADDR(0)),
	doRcrM1<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(RCR16mCL, 
	doRcrMCL<16>(ip, block, ADDR(0)),
	doRcrMCL<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(RCR16mi, 
	doRcrMI<16>(ip, block, ADDR(0), OP(5)),
	doRcrMI<16>(ip, block, STD_GLOBAL_OP(0), OP(5)))
GENERIC_TRANSLATION(RCR16r1, doRcrR1<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCR16rCL, doRcrRCL<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCR16ri, doRcrRI<16>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(RCR32m1, 
	doRcrM1<32>(ip, block, ADDR(0)),
	doRcrM1<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(RCR32mCL, 
	doRcrMCL<32>(ip, block, ADDR(0)),
	doRcrMCL<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_32MI(RCR32mi, 
	doRcrMI<32>(ip, block, ADDR(0), OP(5)),
	doRcrMI<32>(ip, block, STD_GLOBAL_OP(0), OP(5)),
    doRcrMV<32>(ip,  block, ADDR_NOREF(0), GLOBAL_DATA_OFFSET(block, natM, ip)))

GENERIC_TRANSLATION(RCR32r1, doRcrR1<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCR32rCL, doRcrRCL<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(RCR32ri, doRcrRI<32>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(ROL8m1, 
	doRolM1<8>(ip, block, ADDR(0)),
	doRolM1<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(ROL8mCL, 
	doRolMCL<8>(ip, block, ADDR(0)),
	doRolMCL<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(ROL8mi, 
	doRolMI<8>(ip, block, ADDR(0), OP(5)),
	doRolMI<8>(ip, block, STD_GLOBAL_OP(0), OP(5)))
GENERIC_TRANSLATION(ROL8r1, doRolR1<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROL8rCL, doRolRCL<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROL8ri, doRolRI<8>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(ROL16m1, 
	doRolM1<16>(ip, block, ADDR(0)),
	doRolM1<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(ROL16mCL, 
	doRolMCL<16>(ip, block, ADDR(0)),
	doRolMCL<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(ROL16mi, 
	doRolMI<16>(ip, block, ADDR(0), OP(5)),
	doRolMI<16>(ip, block, STD_GLOBAL_OP(0), OP(5)))
GENERIC_TRANSLATION(ROL16r1, doRolR1<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROL16rCL, doRolRCL<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROL16ri, doRolRI<16>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(ROL32m1, 
	doRolM1<32>(ip, block, ADDR(0)),
	doRolM1<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(ROL32mCL, 
	doRolMCL<32>(ip, block, ADDR(0)),
	doRolMCL<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_32MI(ROL32mi, 
	doRolMI<32>(ip, block, ADDR(0), OP(5)),
	doRolMI<32>(ip, block, STD_GLOBAL_OP(0), OP(5)),
    doRolMV<32>(ip,  block, ADDR_NOREF(0), GLOBAL_DATA_OFFSET(block, natM, ip)))

GENERIC_TRANSLATION(ROL32r1, doRolR1<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROL32rCL, doRolRCL<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROL32ri, doRolRI<32>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(ROR8m1, 
	doRorM1<8>(ip, block, ADDR(0)),
	doRorM1<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(ROR8mCL, 
	doRorMCL<8>(ip, block, ADDR(0)),
	doRorMCL<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(ROR8mi, 
	doRorMI<8>(ip, block, ADDR(0), OP(5)),
	doRorMI<8>(ip, block, STD_GLOBAL_OP(0), OP(5)))
GENERIC_TRANSLATION(ROR8r1, doRorR1<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROR8rCL, doRorRCL<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROR8ri, doRorRI<8>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(ROR16m1, 
	doRorM1<16>(ip, block, ADDR(0)),
	doRorM1<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(ROR16mCL, 
	doRorMCL<16>(ip, block, ADDR(0)),
	doRorMCL<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(ROR16mi, 
	doRorMI<16>(ip, block, ADDR(0), OP(5)),
	doRorMI<16>(ip, block, STD_GLOBAL_OP(0), OP(5)))
GENERIC_TRANSLATION(ROR16r1, doRorR1<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROR16rCL, doRorRCL<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROR16ri, doRorRI<16>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(ROR32m1, 
	doRorM1<32>(ip, block, ADDR(0)),
	doRorM1<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(ROR32mCL, 
	doRorMCL<32>(ip, block, ADDR(0)),
	doRorMCL<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_32MI(ROR32mi, 
	doRorMI<32>(ip, block, ADDR(0), OP(5)),
	doRorMI<32>(ip, block, STD_GLOBAL_OP(0), OP(5)),
    doRorMV<32>(ip, block, ADDR_NOREF(0), GLOBAL_DATA_OFFSET(block, natM, ip)))

GENERIC_TRANSLATION(ROR32r1, doRorR1<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROR32rCL, doRorRCL<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(ROR32ri, doRorRI<32>(ip, block, OP(0), OP(1), OP(2)))
GENERIC_TRANSLATION_MEM(SAR16m1, 
	doSarM1<16>(ip, block, ADDR(0)),
	doSarM1<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SAR16mCL, 
	doSarMCL<16>(ip, block, ADDR(0)),
	doSarMCL<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SAR16mi, 
	doSarMI<16>(ip, block, ADDR(0), OP(1)),
	doSarMI<16>(ip, block, STD_GLOBAL_OP(0), OP(1)))
GENERIC_TRANSLATION(SAR16r1, doSarR1<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(SAR16rCL, doSarRCL<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(SAR16ri, doSarRI<16>(ip, block, OP(1), OP(2), OP(0)))
GENERIC_TRANSLATION_MEM(SAR32m1, 
	doSarM1<32>(ip, block, ADDR(0)),
	doSarM1<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SAR32mCL, 
	doSarMCL<32>(ip, block, ADDR(0)),
	doSarMCL<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_32MI(SAR32mi, 
	doSarMI<32>(ip, block, ADDR(0), OP(1)),
	doSarMI<32>(ip, block, STD_GLOBAL_OP(0), OP(1)),
    doSarMV<32>(ip,  block, ADDR_NOREF(0), GLOBAL_DATA_OFFSET(block, natM, ip)))

GENERIC_TRANSLATION(SAR32r1, doSarR1<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(SAR32rCL, doSarRCL<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(SAR32ri, doSarRI<32>(ip, block, OP(1), OP(2), OP(0)))
GENERIC_TRANSLATION_MEM(SAR8m1, 
	doSarM1<8>(ip, block, ADDR(0)),
	doSarM1<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SAR8mCL, 
	doSarMCL<8>(ip, block, ADDR(0)),
	doSarMCL<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SAR8mi, 
	doSarMI<8>(ip, block, ADDR(0), OP(1)),
	doSarMI<8>(ip, block, STD_GLOBAL_OP(0), OP(1)))
GENERIC_TRANSLATION(SAR8r1, doSarR1<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(SAR8rCL, doSarRCL<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(SAR8ri, doSarRI<8>(ip, block, OP(1), OP(2), OP(0)))
GENERIC_TRANSLATION_MEM(SHL16m1, 
	doShlM1<16>(ip, block, ADDR(0)),
	doShlM1<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SHL16mCL, 
	doShlMCL<16>(ip, block, ADDR(0)),
	doShlMCL<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SHL16mi, 
	doShlMI<16>(ip, block, ADDR(0), OP(5)),
	doShlMI<16>(ip, block, STD_GLOBAL_OP(0), OP(5)))
GENERIC_TRANSLATION(SHL16r1, doShlR1<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHL16rCL, doShlRCL<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHL16ri, doShlRI<16>(ip, block, OP(1), OP(2), OP(0)))
GENERIC_TRANSLATION_MEM(SHL32m1, 
	doShlM1<32>(ip, block, ADDR(0)),
	doShlM1<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SHL32mCL, 
	doShlMCL<32>(ip, block, ADDR(0)),
	doShlMCL<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_32MI(SHL32mi, 
	doShlMI<32>(ip, block, ADDR(0), OP(5)),
	doShlMI<32>(ip, block, STD_GLOBAL_OP(0), OP(5)),
    doShlMV<32>(ip,  block, ADDR_NOREF(0), GLOBAL_DATA_OFFSET(block, natM, ip)))

GENERIC_TRANSLATION(SHL32r1, doShlR1<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHL32rCL, doShlRCL<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHL32ri, doShlRI<32>(ip, block, OP(1), OP(2), OP(0)))
GENERIC_TRANSLATION_MEM(SHL8m1, 
	doShlM1<8>(ip, block, ADDR(0)),
	doShlM1<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SHL8mCL, 
	doShlMCL<8>(ip, block, ADDR(0)),
	doShlMCL<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SHL8mi, 
	doShlMI<8>(ip, block, ADDR(0), OP(1)),
	doShlMI<8>(ip, block, STD_GLOBAL_OP(0), OP(1)))
GENERIC_TRANSLATION(SHL8r1, doShlR1<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHL8rCL, doShlRCL<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHL8ri, doShlRI<8>(ip, block, OP(1), OP(2), OP(0)))
GENERIC_TRANSLATION_MEM(SHR16m1, 
	doShrM1<16>(ip, block, ADDR(0)),
	doShrM1<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SHR16mCL, 
	doShrMCL<16>(ip, block, ADDR(0)),
	doShrMCL<16>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SHR16mi, 
	doShrMI<16>(ip, block, ADDR(0), OP(5)),
	doShrMI<16>(ip, block, STD_GLOBAL_OP(0), OP(5)))
GENERIC_TRANSLATION(SHR16r1, doShrR1<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHR16rCL, doShrRCL<16>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHR16ri, doShrRI<16>(ip, block, OP(1), OP(2), OP(0)))
GENERIC_TRANSLATION_MEM(SHR32m1, 
	doShrM1<32>(ip, block, ADDR(0)),
	doShrM1<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SHR32mCL, 
	doShrMCL<32>(ip, block, ADDR(0)),
	doShrMCL<32>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_32MI(SHR32mi, 
	doShrMI<32>(ip, block, ADDR(0), OP(5)),
	doShrMI<32>(ip, block, STD_GLOBAL_OP(0), OP(5)),
    doShrMV<32>(ip,  block, ADDR_NOREF(0), GLOBAL_DATA_OFFSET(block, natM, ip)))

GENERIC_TRANSLATION(SHR32r1, doShrR1<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHR32rCL, doShrRCL<32>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHR32ri, doShrRI<32>(ip, block, OP(1), OP(2), OP(0)))
GENERIC_TRANSLATION_MEM(SHR8m1, 
	doShrM1<8>(ip, block, ADDR(0)),
	doShrM1<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SHR8mCL, 
	doShrMCL<8>(ip, block, ADDR(0)),
	doShrMCL<8>(ip, block, STD_GLOBAL_OP(0)))
GENERIC_TRANSLATION_MEM(SHR8mi, 
	doShrMI<8>(ip, block, ADDR(0), OP(1)),
	doShrMI<8>(ip, block, STD_GLOBAL_OP(0), OP(1)))
GENERIC_TRANSLATION(SHR8r1, doShrR1<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHR8rCL, doShrRCL<8>(ip, block, OP(0)))
GENERIC_TRANSLATION(SHR8ri, doShrRI<8>(ip, block, OP(1), OP(2), OP(0)))
GENERIC_TRANSLATION(SHRD32rri8, doShrdRI<32>(ip, block, OP(1), OP(2), OP(3)))
GENERIC_TRANSLATION(SHLD32rri8, doShldRI<32>(ip, block, OP(1), OP(2), OP(3)))
GENERIC_TRANSLATION(SHRD32rrCL, doShrdRCL<32>(ip, block, OP(1), OP(2)))
GENERIC_TRANSLATION(SHLD32rrCL, doShldRCL<32>(ip, block, OP(1), OP(2)))

void ShiftRoll_populateDispatchMap(DispatchMap &m) {
        m[X86::RCL8m1] = translate_RCL8m1;
        m[X86::RCL8mCL] = translate_RCL8mCL;
        m[X86::RCL8mi] = translate_RCL8mi;
        m[X86::RCL8r1] = translate_RCL8r1;
        m[X86::RCL8rCL] = translate_RCL8rCL;
        m[X86::RCL8ri] = translate_RCL8ri;
        m[X86::RCL16m1] = translate_RCL16m1;
        m[X86::RCL16mCL] = translate_RCL16mCL;
        m[X86::RCL16mi] = translate_RCL16mi;
        m[X86::RCL16r1] = translate_RCL16r1;
        m[X86::RCL16rCL] = translate_RCL16rCL;
        m[X86::RCL16ri] = translate_RCL16ri;
        m[X86::RCL32m1] = translate_RCL32m1;
        m[X86::RCL32mCL] = translate_RCL32mCL;
        m[X86::RCL32mi] = translate_RCL32mi;
        m[X86::RCL32r1] = translate_RCL32r1;
        m[X86::RCL32rCL] = translate_RCL32rCL;
        m[X86::RCL32ri] = translate_RCL32ri;
        m[X86::RCR8m1] = translate_RCR8m1;
        m[X86::RCR8mCL] = translate_RCR8mCL;
        m[X86::RCR8mi] = translate_RCR8mi;
        m[X86::RCR8r1] = translate_RCR8r1;
        m[X86::RCR8rCL] = translate_RCR8rCL;
        m[X86::RCR8ri] = translate_RCR8ri;
        m[X86::RCR16m1] = translate_RCR16m1;
        m[X86::RCR16mCL] = translate_RCR16mCL;
        m[X86::RCR16mi] = translate_RCR16mi;
        m[X86::RCR16r1] = translate_RCR16r1;
        m[X86::RCR16rCL] = translate_RCR16rCL;
        m[X86::RCR16ri] = translate_RCR16ri;
        m[X86::RCR32m1] = translate_RCR32m1;
        m[X86::RCR32mCL] = translate_RCR32mCL;
        m[X86::RCR32mi] = translate_RCR32mi;
        m[X86::RCR32r1] = translate_RCR32r1;
        m[X86::RCR32rCL] = translate_RCR32rCL;
        m[X86::RCR32ri] = translate_RCR32ri;
        m[X86::ROL8m1] = translate_ROL8m1;
        m[X86::ROL8mCL] = translate_ROL8mCL;
        m[X86::ROL8mi] = translate_ROL8mi;
        m[X86::ROL8r1] = translate_ROL8r1;
        m[X86::ROL8rCL] = translate_ROL8rCL;
        m[X86::ROL8ri] = translate_ROL8ri;
        m[X86::ROL16m1] = translate_ROL16m1;
        m[X86::ROL16mCL] = translate_ROL16mCL;
        m[X86::ROL16mi] = translate_ROL16mi;
        m[X86::ROL16r1] = translate_ROL16r1;
        m[X86::ROL16rCL] = translate_ROL16rCL;
        m[X86::ROL16ri] = translate_ROL16ri;
        m[X86::ROL32m1] = translate_ROL32m1;
        m[X86::ROL32mCL] = translate_ROL32mCL;
        m[X86::ROL32mi] = translate_ROL32mi;
        m[X86::ROL32r1] = translate_ROL32r1;
        m[X86::ROL32rCL] = translate_ROL32rCL;
        m[X86::ROL32ri] = translate_ROL32ri;
        m[X86::ROR8m1] = translate_ROR8m1;
        m[X86::ROR8mCL] = translate_ROR8mCL;
        m[X86::ROR8mi] = translate_ROR8mi;
        m[X86::ROR8r1] = translate_ROR8r1;
        m[X86::ROR8rCL] = translate_ROR8rCL;
        m[X86::ROR8ri] = translate_ROR8ri;
        m[X86::ROR16m1] = translate_ROR16m1;
        m[X86::ROR16mCL] = translate_ROR16mCL;
        m[X86::ROR16mi] = translate_ROR16mi;
        m[X86::ROR16r1] = translate_ROR16r1;
        m[X86::ROR16rCL] = translate_ROR16rCL;
        m[X86::ROR16ri] = translate_ROR16ri;
        m[X86::ROR32m1] = translate_ROR32m1;
        m[X86::ROR32mCL] = translate_ROR32mCL;
        m[X86::ROR32mi] = translate_ROR32mi;
        m[X86::ROR32r1] = translate_ROR32r1;
        m[X86::ROR32rCL] = translate_ROR32rCL;
        m[X86::ROR32ri] = translate_ROR32ri;
        m[X86::SAR16m1] = translate_SAR16m1;
        m[X86::SAR16mCL] = translate_SAR16mCL;
        m[X86::SAR16mi] = translate_SAR16mi;
        m[X86::SAR16r1] = translate_SAR16r1;
        m[X86::SAR16rCL] = translate_SAR16rCL;
        m[X86::SAR16ri] = translate_SAR16ri;
        m[X86::SAR32m1] = translate_SAR32m1;
        m[X86::SAR32mCL] = translate_SAR32mCL;
        m[X86::SAR32mi] = translate_SAR32mi;
        m[X86::SAR32r1] = translate_SAR32r1;
        m[X86::SAR32rCL] = translate_SAR32rCL;
        m[X86::SAR32ri] = translate_SAR32ri;
        m[X86::SAR8m1] = translate_SAR8m1;
        m[X86::SAR8mCL] = translate_SAR8mCL;
        m[X86::SAR8mi] = translate_SAR8mi;
        m[X86::SAR8r1] = translate_SAR8r1;
        m[X86::SAR8rCL] = translate_SAR8rCL;
        m[X86::SAR8ri] = translate_SAR8ri;
        m[X86::SHL16m1] = translate_SHL16m1;
        m[X86::SHL16mCL] = translate_SHL16mCL;
        m[X86::SHL16mi] = translate_SHL16mi;
        m[X86::SHL16r1] = translate_SHL16r1;
        m[X86::SHL16rCL] = translate_SHL16rCL;
        m[X86::SHL16ri] = translate_SHL16ri;
        m[X86::SHL32m1] = translate_SHL32m1;
        m[X86::SHL32mCL] = translate_SHL32mCL;
        m[X86::SHL32mi] = translate_SHL32mi;
        m[X86::SHL32r1] = translate_SHL32r1;
        m[X86::SHL32rCL] = translate_SHL32rCL;
        m[X86::SHL32ri] = translate_SHL32ri;
        m[X86::SHL8m1] = translate_SHL8m1;
        m[X86::SHL8mCL] = translate_SHL8mCL;
        m[X86::SHL8mi] = translate_SHL8mi;
        m[X86::SHL8r1] = translate_SHL8r1;
        m[X86::SHL8rCL] = translate_SHL8rCL;
        m[X86::SHL8ri] = translate_SHL8ri;
        m[X86::SHR16m1] = translate_SHR16m1;
        m[X86::SHR16mCL] = translate_SHR16mCL;
        m[X86::SHR16mi] = translate_SHR16mi;
        m[X86::SHR16r1] = translate_SHR16r1;
        m[X86::SHR16rCL] = translate_SHR16rCL;
        m[X86::SHR16ri] = translate_SHR16ri;
        m[X86::SHR32m1] = translate_SHR32m1;
        m[X86::SHR32mCL] = translate_SHR32mCL;
        m[X86::SHR32mi] = translate_SHR32mi;
        m[X86::SHR32r1] = translate_SHR32r1;
        m[X86::SHR32rCL] = translate_SHR32rCL;
        m[X86::SHR32ri] = translate_SHR32ri;
        m[X86::SHR8m1] = translate_SHR8m1;
        m[X86::SHR8mCL] = translate_SHR8mCL;
        m[X86::SHR8mi] = translate_SHR8mi;
        m[X86::SHR8r1] = translate_SHR8r1;
        m[X86::SHR8rCL] = translate_SHR8rCL;
        m[X86::SHR8ri] = translate_SHR8ri;
        m[X86::SHRD32rri8] = translate_SHRD32rri8;
        m[X86::SHRD32rrCL] = translate_SHRD32rrCL;
        m[X86::SHLD32rrCL] = translate_SHLD32rrCL;
        m[X86::SHLD32rri8] = translate_SHLD32rri8;
}
