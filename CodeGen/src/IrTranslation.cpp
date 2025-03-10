// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "IrTranslation.h"

#include "Luau/Bytecode.h"
#include "Luau/IrBuilder.h"
#include "Luau/IrUtils.h"

#include "CustomExecUtils.h"

#include "lobject.h"
#include "ltm.h"

namespace Luau
{
namespace CodeGen
{

// Helper to consistently define a switch to instruction fallback code
struct FallbackStreamScope
{
    FallbackStreamScope(IrBuilder& build, IrOp fallback, IrOp next)
        : build(build)
        , next(next)
    {
        LUAU_ASSERT(fallback.kind == IrOpKind::Block);
        LUAU_ASSERT(next.kind == IrOpKind::Block);

        build.inst(IrCmd::JUMP, next);
        build.beginBlock(fallback);
    }

    ~FallbackStreamScope()
    {
        build.beginBlock(next);
    }

    IrBuilder& build;
    IrOp next;
};

void translateInstLoadNil(IrBuilder& build, const Instruction* pc)
{
    int ra = LUAU_INSN_A(*pc);

    build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TNIL));
}

void translateInstLoadB(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);

    build.inst(IrCmd::STORE_INT, build.vmReg(ra), build.constInt(LUAU_INSN_B(*pc)));
    build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TBOOLEAN));

    if (int target = LUAU_INSN_C(*pc))
        build.inst(IrCmd::JUMP, build.blockAtInst(pcpos + 1 + target));
}

void translateInstLoadN(IrBuilder& build, const Instruction* pc)
{
    int ra = LUAU_INSN_A(*pc);

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(ra), build.constDouble(double(LUAU_INSN_D(*pc))));
    build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TNUMBER));
}

void translateInstLoadK(IrBuilder& build, const Instruction* pc)
{
    int ra = LUAU_INSN_A(*pc);

    // TODO: per-component loads and stores might be preferable
    IrOp load = build.inst(IrCmd::LOAD_TVALUE, build.vmConst(LUAU_INSN_D(*pc)));
    build.inst(IrCmd::STORE_TVALUE, build.vmReg(ra), load);
}

void translateInstLoadKX(IrBuilder& build, const Instruction* pc)
{
    int ra = LUAU_INSN_A(*pc);
    uint32_t aux = pc[1];

    // TODO: per-component loads and stores might be preferable
    IrOp load = build.inst(IrCmd::LOAD_TVALUE, build.vmConst(aux));
    build.inst(IrCmd::STORE_TVALUE, build.vmReg(ra), load);
}

void translateInstMove(IrBuilder& build, const Instruction* pc)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);

    // TODO: per-component loads and stores might be preferable
    IrOp load = build.inst(IrCmd::LOAD_TVALUE, build.vmReg(rb));
    build.inst(IrCmd::STORE_TVALUE, build.vmReg(ra), load);
}

void translateInstJump(IrBuilder& build, const Instruction* pc, int pcpos)
{
    build.inst(IrCmd::JUMP, build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc)));
}

void translateInstJumpBack(IrBuilder& build, const Instruction* pc, int pcpos)
{
    build.inst(IrCmd::INTERRUPT, build.constUint(pcpos));
    build.inst(IrCmd::JUMP, build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc)));
}

void translateInstJumpIf(IrBuilder& build, const Instruction* pc, int pcpos, bool not_)
{
    int ra = LUAU_INSN_A(*pc);

    IrOp target = build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc));
    IrOp next = build.blockAtInst(pcpos + 1);

    // TODO: falsy/truthy conditions should be deconstructed into more primitive operations
    if (not_)
        build.inst(IrCmd::JUMP_IF_FALSY, build.vmReg(ra), target, next);
    else
        build.inst(IrCmd::JUMP_IF_TRUTHY, build.vmReg(ra), target, next);

    // Fallthrough in original bytecode is implicit, so we start next internal block here
    if (build.isInternalBlock(next))
        build.beginBlock(next);
}

void translateInstJumpIfEq(IrBuilder& build, const Instruction* pc, int pcpos, bool not_)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = pc[1];

    IrOp target = build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc));
    IrOp next = build.blockAtInst(pcpos + 2);
    IrOp numberCheck = build.block(IrBlockKind::Internal);
    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp ta = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra));
    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::JUMP_EQ_TAG, ta, tb, numberCheck, not_ ? target : next);

    build.beginBlock(numberCheck);

    // fast-path: number
    build.inst(IrCmd::CHECK_TAG, ta, build.constTag(LUA_TNUMBER), fallback);

    IrOp va = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(ra));
    IrOp vb = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(rb));

    build.inst(IrCmd::JUMP_CMP_NUM, va, vb, build.cond(IrCondition::NotEqual), not_ ? target : next, not_ ? next : target);

    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::JUMP_CMP_ANY, build.vmReg(ra), build.vmReg(rb), build.cond(not_ ? IrCondition::NotEqual : IrCondition::Equal), target, next);
}

void translateInstJumpIfCond(IrBuilder& build, const Instruction* pc, int pcpos, IrCondition cond)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = pc[1];

    IrOp target = build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc));
    IrOp next = build.blockAtInst(pcpos + 2);
    IrOp fallback = build.block(IrBlockKind::Fallback);

    // fast-path: number
    IrOp ta = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra));
    build.inst(IrCmd::CHECK_TAG, ta, build.constTag(LUA_TNUMBER), fallback);

    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::CHECK_TAG, tb, build.constTag(LUA_TNUMBER), fallback);

    IrOp va = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(ra));
    IrOp vb = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(rb));

    build.inst(IrCmd::JUMP_CMP_NUM, va, vb, build.cond(cond), target, next);

    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::JUMP_CMP_ANY, build.vmReg(ra), build.vmReg(rb), build.cond(cond), target, next);
}

void translateInstJumpX(IrBuilder& build, const Instruction* pc, int pcpos)
{
    build.inst(IrCmd::INTERRUPT, build.constUint(pcpos));
    build.inst(IrCmd::JUMP, build.blockAtInst(pcpos + 1 + LUAU_INSN_E(*pc)));
}

void translateInstJumpxEqNil(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    bool not_ = (pc[1] & 0x80000000) != 0;

    IrOp target = build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc));
    IrOp next = build.blockAtInst(pcpos + 2);

    IrOp ta = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra));
    build.inst(IrCmd::JUMP_EQ_TAG, ta, build.constTag(LUA_TNIL), not_ ? next : target, not_ ? target : next);

    // Fallthrough in original bytecode is implicit, so we start next internal block here
    if (build.isInternalBlock(next))
        build.beginBlock(next);
}

void translateInstJumpxEqB(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    uint32_t aux = pc[1];
    bool not_ = (aux & 0x80000000) != 0;

    IrOp target = build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc));
    IrOp next = build.blockAtInst(pcpos + 2);
    IrOp checkValue = build.block(IrBlockKind::Internal);

    IrOp ta = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra));

    build.inst(IrCmd::JUMP_EQ_TAG, ta, build.constTag(LUA_TBOOLEAN), checkValue, not_ ? target : next);

    build.beginBlock(checkValue);
    IrOp va = build.inst(IrCmd::LOAD_INT, build.vmReg(ra));

    build.inst(IrCmd::JUMP_EQ_INT, va, build.constInt(aux & 0x1), not_ ? next : target, not_ ? target : next);

    // Fallthrough in original bytecode is implicit, so we start next internal block here
    if (build.isInternalBlock(next))
        build.beginBlock(next);
}

void translateInstJumpxEqN(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    uint32_t aux = pc[1];
    bool not_ = (aux & 0x80000000) != 0;

    IrOp target = build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc));
    IrOp next = build.blockAtInst(pcpos + 2);
    IrOp checkValue = build.block(IrBlockKind::Internal);

    IrOp ta = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra));

    build.inst(IrCmd::JUMP_EQ_TAG, ta, build.constTag(LUA_TNUMBER), checkValue, not_ ? target : next);

    build.beginBlock(checkValue);
    IrOp va = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(ra));

    LUAU_ASSERT(build.function.proto);
    TValue protok = build.function.proto->k[aux & 0xffffff];

    LUAU_ASSERT(protok.tt == LUA_TNUMBER);
    IrOp vb = build.constDouble(protok.value.n);

    build.inst(IrCmd::JUMP_CMP_NUM, va, vb, build.cond(IrCondition::NotEqual), not_ ? target : next, not_ ? next : target);

    // Fallthrough in original bytecode is implicit, so we start next internal block here
    if (build.isInternalBlock(next))
        build.beginBlock(next);
}

void translateInstJumpxEqS(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    uint32_t aux = pc[1];
    bool not_ = (aux & 0x80000000) != 0;

    IrOp target = build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc));
    IrOp next = build.blockAtInst(pcpos + 2);
    IrOp checkValue = build.block(IrBlockKind::Internal);

    IrOp ta = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra));
    build.inst(IrCmd::JUMP_EQ_TAG, ta, build.constTag(LUA_TSTRING), checkValue, not_ ? target : next);

    build.beginBlock(checkValue);
    IrOp va = build.inst(IrCmd::LOAD_POINTER, build.vmReg(ra));
    IrOp vb = build.inst(IrCmd::LOAD_POINTER, build.vmConst(aux & 0xffffff));

    build.inst(IrCmd::JUMP_EQ_POINTER, va, vb, not_ ? next : target, not_ ? target : next);

    // Fallthrough in original bytecode is implicit, so we start next internal block here
    if (build.isInternalBlock(next))
        build.beginBlock(next);
}

static void translateInstBinaryNumeric(IrBuilder& build, int ra, int rb, int rc, IrOp opc, int pcpos, TMS tm)
{
    IrOp fallback = build.block(IrBlockKind::Fallback);

    // fast-path: number
    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::CHECK_TAG, tb, build.constTag(LUA_TNUMBER), fallback);

    if (rc != -1 && rc != rb) // TODO: optimization should handle second check, but we'll test it later
    {
        IrOp tc = build.inst(IrCmd::LOAD_TAG, build.vmReg(rc));
        build.inst(IrCmd::CHECK_TAG, tc, build.constTag(LUA_TNUMBER), fallback);
    }

    IrOp vb = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(rb));
    IrOp vc;

    if (opc.kind == IrOpKind::VmConst)
    {
        LUAU_ASSERT(build.function.proto);
        TValue protok = build.function.proto->k[opc.index];

        LUAU_ASSERT(protok.tt == LUA_TNUMBER);
        vc = build.constDouble(protok.value.n);
    }
    else
    {
        vc = build.inst(IrCmd::LOAD_DOUBLE, opc);
    }

    IrOp va;

    switch (tm)
    {
    case TM_ADD:
        va = build.inst(IrCmd::ADD_NUM, vb, vc);
        break;
    case TM_SUB:
        va = build.inst(IrCmd::SUB_NUM, vb, vc);
        break;
    case TM_MUL:
        va = build.inst(IrCmd::MUL_NUM, vb, vc);
        break;
    case TM_DIV:
        va = build.inst(IrCmd::DIV_NUM, vb, vc);
        break;
    case TM_MOD:
        va = build.inst(IrCmd::MOD_NUM, vb, vc);
        break;
    case TM_POW:
        va = build.inst(IrCmd::POW_NUM, vb, vc);
        break;
    default:
        LUAU_ASSERT(!"unsupported binary op");
    }

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(ra), va);

    if (ra != rb && ra != rc) // TODO: optimization should handle second check, but we'll test this later
        build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TNUMBER));

    IrOp next = build.blockAtInst(pcpos + 1);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::DO_ARITH, build.vmReg(ra), build.vmReg(rb), opc, build.constInt(tm));
    build.inst(IrCmd::JUMP, next);
}

void translateInstBinary(IrBuilder& build, const Instruction* pc, int pcpos, TMS tm)
{
    translateInstBinaryNumeric(build, LUAU_INSN_A(*pc), LUAU_INSN_B(*pc), LUAU_INSN_C(*pc), build.vmReg(LUAU_INSN_C(*pc)), pcpos, tm);
}

void translateInstBinaryK(IrBuilder& build, const Instruction* pc, int pcpos, TMS tm)
{
    translateInstBinaryNumeric(build, LUAU_INSN_A(*pc), LUAU_INSN_B(*pc), -1, build.vmConst(LUAU_INSN_C(*pc)), pcpos, tm);
}

void translateInstNot(IrBuilder& build, const Instruction* pc)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);

    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    IrOp vb = build.inst(IrCmd::LOAD_INT, build.vmReg(rb));

    IrOp va = build.inst(IrCmd::NOT_ANY, tb, vb);

    build.inst(IrCmd::STORE_INT, build.vmReg(ra), va);
    build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TBOOLEAN));
}

void translateInstMinus(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);

    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::CHECK_TAG, tb, build.constTag(LUA_TNUMBER), fallback);

    // fast-path: number
    IrOp vb = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(rb));
    IrOp va = build.inst(IrCmd::UNM_NUM, vb);

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(ra), va);

    if (ra != rb)
        build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TNUMBER));

    IrOp next = build.blockAtInst(pcpos + 1);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::DO_ARITH, build.vmReg(LUAU_INSN_A(*pc)), build.vmReg(LUAU_INSN_B(*pc)), build.vmReg(LUAU_INSN_B(*pc)), build.constInt(TM_UNM));
    build.inst(IrCmd::JUMP, next);
}

void translateInstLength(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);

    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::CHECK_TAG, tb, build.constTag(LUA_TTABLE), fallback);

    // fast-path: table without __len
    IrOp vb = build.inst(IrCmd::LOAD_POINTER, build.vmReg(rb));
    build.inst(IrCmd::CHECK_NO_METATABLE, vb, fallback);

    IrOp va = build.inst(IrCmd::TABLE_LEN, vb);

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(ra), va);
    build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TNUMBER));

    IrOp next = build.blockAtInst(pcpos + 1);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::DO_LEN, build.vmReg(LUAU_INSN_A(*pc)), build.vmReg(LUAU_INSN_B(*pc)));
    build.inst(IrCmd::JUMP, next);
}

void translateInstNewTable(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int b = LUAU_INSN_B(*pc);
    uint32_t aux = pc[1];

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));

    IrOp va = build.inst(IrCmd::NEW_TABLE, build.constUint(aux), build.constUint(b == 0 ? 0 : 1 << (b - 1)));
    build.inst(IrCmd::STORE_POINTER, build.vmReg(ra), va);
    build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TTABLE));

    build.inst(IrCmd::CHECK_GC);
}

void translateInstDupTable(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int k = LUAU_INSN_D(*pc);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));

    IrOp table = build.inst(IrCmd::LOAD_POINTER, build.vmConst(k));
    IrOp va = build.inst(IrCmd::DUP_TABLE, table);
    build.inst(IrCmd::STORE_POINTER, build.vmReg(ra), va);
    build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TTABLE));

    build.inst(IrCmd::CHECK_GC);
}

void translateInstGetUpval(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int up = LUAU_INSN_B(*pc);

    build.inst(IrCmd::GET_UPVALUE, build.vmReg(ra), build.vmUpvalue(up));
}

void translateInstSetUpval(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int up = LUAU_INSN_B(*pc);

    build.inst(IrCmd::SET_UPVALUE, build.vmUpvalue(up), build.vmReg(ra));
}

void translateInstCloseUpvals(IrBuilder& build, const Instruction* pc)
{
    int ra = LUAU_INSN_A(*pc);

    build.inst(IrCmd::CLOSE_UPVALS, build.vmReg(ra));
}

void translateInstForNPrep(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);

    IrOp loopStart = build.blockAtInst(pcpos + getOpLength(LuauOpcode(LUAU_INSN_OP(*pc))));
    IrOp loopExit = build.blockAtInst(getJumpTarget(*pc, pcpos));
    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp nextStep = build.block(IrBlockKind::Internal);
    IrOp direct = build.block(IrBlockKind::Internal);
    IrOp reverse = build.block(IrBlockKind::Internal);

    IrOp tagLimit = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra + 0));
    build.inst(IrCmd::CHECK_TAG, tagLimit, build.constTag(LUA_TNUMBER), fallback);
    IrOp tagStep = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra + 1));
    build.inst(IrCmd::CHECK_TAG, tagStep, build.constTag(LUA_TNUMBER), fallback);
    IrOp tagIdx = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra + 2));
    build.inst(IrCmd::CHECK_TAG, tagIdx, build.constTag(LUA_TNUMBER), fallback);
    build.inst(IrCmd::JUMP, nextStep);

    // After successful conversion of arguments to number in a fallback, we return here
    build.beginBlock(nextStep);

    IrOp zero = build.constDouble(0.0);
    IrOp limit = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(ra + 0));
    IrOp step = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(ra + 1));
    IrOp idx = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(ra + 2));

    // step <= 0
    build.inst(IrCmd::JUMP_CMP_NUM, step, zero, build.cond(IrCondition::LessEqual), reverse, direct);

    // TODO: target branches can probably be arranged better, but we need tests for NaN behavior preservation

    // step <= 0 is false, check idx <= limit
    build.beginBlock(direct);
    build.inst(IrCmd::JUMP_CMP_NUM, idx, limit, build.cond(IrCondition::LessEqual), loopStart, loopExit);

    // step <= 0 is true, check limit <= idx
    build.beginBlock(reverse);
    build.inst(IrCmd::JUMP_CMP_NUM, limit, idx, build.cond(IrCondition::LessEqual), loopStart, loopExit);

    // Fallback will try to convert loop variables to numbers or throw an error
    build.beginBlock(fallback);
    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::PREPARE_FORN, build.vmReg(ra + 0), build.vmReg(ra + 1), build.vmReg(ra + 2));
    build.inst(IrCmd::JUMP, nextStep);

    // Fallthrough in original bytecode is implicit, so we start next internal block here
    if (build.isInternalBlock(loopStart))
        build.beginBlock(loopStart);
}

void translateInstForNLoop(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);

    IrOp loopRepeat = build.blockAtInst(getJumpTarget(*pc, pcpos));
    IrOp loopExit = build.blockAtInst(pcpos + getOpLength(LuauOpcode(LUAU_INSN_OP(*pc))));

    build.inst(IrCmd::INTERRUPT, build.constUint(pcpos));

    IrOp zero = build.constDouble(0.0);
    IrOp limit = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(ra + 0));
    IrOp step = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(ra + 1));

    IrOp idx = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(ra + 2));
    idx = build.inst(IrCmd::ADD_NUM, idx, step);
    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(ra + 2), idx);

    IrOp direct = build.block(IrBlockKind::Internal);
    IrOp reverse = build.block(IrBlockKind::Internal);

    // step <= 0
    build.inst(IrCmd::JUMP_CMP_NUM, step, zero, build.cond(IrCondition::LessEqual), reverse, direct);

    // step <= 0 is false, check idx <= limit
    build.beginBlock(direct);
    build.inst(IrCmd::JUMP_CMP_NUM, idx, limit, build.cond(IrCondition::LessEqual), loopRepeat, loopExit);

    // step <= 0 is true, check limit <= idx
    build.beginBlock(reverse);
    build.inst(IrCmd::JUMP_CMP_NUM, limit, idx, build.cond(IrCondition::LessEqual), loopRepeat, loopExit);

    // Fallthrough in original bytecode is implicit, so we start next internal block here
    if (build.isInternalBlock(loopExit))
        build.beginBlock(loopExit);
}

void translateInstForGPrepNext(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);

    IrOp target = build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc));
    IrOp fallback = build.block(IrBlockKind::Fallback);

    // fast-path: pairs/next
    build.inst(IrCmd::CHECK_SAFE_ENV, fallback);
    IrOp tagB = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra + 1));
    build.inst(IrCmd::CHECK_TAG, tagB, build.constTag(LUA_TTABLE), fallback);
    IrOp tagC = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra + 2));
    build.inst(IrCmd::CHECK_TAG, tagC, build.constTag(LUA_TNIL), fallback);

    build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TNIL));

    // setpvalue(ra + 2, reinterpret_cast<void*>(uintptr_t(0)));
    build.inst(IrCmd::STORE_INT, build.vmReg(ra + 2), build.constInt(0));
    build.inst(IrCmd::STORE_TAG, build.vmReg(ra + 2), build.constTag(LUA_TLIGHTUSERDATA));

    build.inst(IrCmd::JUMP, target);

    // FallbackStreamScope not used here because this instruction doesn't fallthrough to next instruction
    build.beginBlock(fallback);
    build.inst(IrCmd::LOP_FORGPREP_XNEXT_FALLBACK, build.constUint(pcpos), target);
}

void translateInstForGPrepInext(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);

    IrOp target = build.blockAtInst(pcpos + 1 + LUAU_INSN_D(*pc));
    IrOp fallback = build.block(IrBlockKind::Fallback);
    IrOp finish = build.block(IrBlockKind::Internal);

    // fast-path: ipairs/inext
    build.inst(IrCmd::CHECK_SAFE_ENV, fallback);
    IrOp tagB = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra + 1));
    build.inst(IrCmd::CHECK_TAG, tagB, build.constTag(LUA_TTABLE), fallback);
    IrOp tagC = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra + 2));
    build.inst(IrCmd::CHECK_TAG, tagC, build.constTag(LUA_TNUMBER), fallback);

    IrOp numC = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(ra + 2));
    build.inst(IrCmd::JUMP_CMP_NUM, numC, build.constDouble(0.0), build.cond(IrCondition::NotEqual), fallback, finish);

    build.beginBlock(finish);

    build.inst(IrCmd::STORE_TAG, build.vmReg(ra), build.constTag(LUA_TNIL));

    // setpvalue(ra + 2, reinterpret_cast<void*>(uintptr_t(0)));
    build.inst(IrCmd::STORE_INT, build.vmReg(ra + 2), build.constInt(0));
    build.inst(IrCmd::STORE_TAG, build.vmReg(ra + 2), build.constTag(LUA_TLIGHTUSERDATA));

    build.inst(IrCmd::JUMP, target);

    // FallbackStreamScope not used here because this instruction doesn't fallthrough to next instruction
    build.beginBlock(fallback);
    build.inst(IrCmd::LOP_FORGPREP_XNEXT_FALLBACK, build.constUint(pcpos), target);
}

void translateInstForGLoopIpairs(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    LUAU_ASSERT(int(pc[1]) < 0);

    IrOp loopRepeat = build.blockAtInst(getJumpTarget(*pc, pcpos));
    IrOp loopExit = build.blockAtInst(pcpos + getOpLength(LuauOpcode(LUAU_INSN_OP(*pc))));
    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp hasElem = build.block(IrBlockKind::Internal);

    build.inst(IrCmd::INTERRUPT, build.constUint(pcpos));

    // fast-path: builtin table iteration
    IrOp tagA = build.inst(IrCmd::LOAD_TAG, build.vmReg(ra));
    build.inst(IrCmd::CHECK_TAG, tagA, build.constTag(LUA_TNIL), fallback);

    IrOp table = build.inst(IrCmd::LOAD_POINTER, build.vmReg(ra + 1));
    IrOp index = build.inst(IrCmd::LOAD_INT, build.vmReg(ra + 2));

    IrOp elemPtr = build.inst(IrCmd::GET_ARR_ADDR, table, index);

    // Terminate if array has ended
    build.inst(IrCmd::CHECK_ARRAY_SIZE, table, index, loopExit);

    // Terminate if element is nil
    IrOp elemTag = build.inst(IrCmd::LOAD_TAG, elemPtr);
    build.inst(IrCmd::JUMP_EQ_TAG, elemTag, build.constTag(LUA_TNIL), loopExit, hasElem);
    build.beginBlock(hasElem);

    IrOp nextIndex = build.inst(IrCmd::ADD_INT, index, build.constInt(1));

    // We update only a dword part of the userdata pointer that's reused in loop iteration as an index
    // Upper bits start and remain to be 0
    build.inst(IrCmd::STORE_INT, build.vmReg(ra + 2), nextIndex);
    // Tag should already be set to lightuserdata

    // setnvalue(ra + 3, double(index + 1));
    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(ra + 3), build.inst(IrCmd::INT_TO_NUM, nextIndex));
    build.inst(IrCmd::STORE_TAG, build.vmReg(ra + 3), build.constTag(LUA_TNUMBER));

    // setobj2s(L, ra + 4, e);
    IrOp elemTV = build.inst(IrCmd::LOAD_TVALUE, elemPtr);
    build.inst(IrCmd::STORE_TVALUE, build.vmReg(ra + 4), elemTV);

    build.inst(IrCmd::JUMP, loopRepeat);

    build.beginBlock(fallback);
    build.inst(IrCmd::LOP_FORGLOOP_FALLBACK, build.constUint(pcpos), loopRepeat, loopExit);

    // Fallthrough in original bytecode is implicit, so we start next internal block here
    if (build.isInternalBlock(loopExit))
        build.beginBlock(loopExit);
}

void translateInstGetTableN(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);
    int c = LUAU_INSN_C(*pc);

    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::CHECK_TAG, tb, build.constTag(LUA_TTABLE), fallback);

    IrOp vb = build.inst(IrCmd::LOAD_POINTER, build.vmReg(rb));

    build.inst(IrCmd::CHECK_ARRAY_SIZE, vb, build.constUint(c), fallback);
    build.inst(IrCmd::CHECK_NO_METATABLE, vb, fallback);

    IrOp arrEl = build.inst(IrCmd::GET_ARR_ADDR, vb, build.constUint(c));

    // TODO: per-component loads and stores might be preferable
    IrOp arrElTval = build.inst(IrCmd::LOAD_TVALUE, arrEl);
    build.inst(IrCmd::STORE_TVALUE, build.vmReg(ra), arrElTval);

    IrOp next = build.blockAtInst(pcpos + 1);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::GET_TABLE, build.vmReg(ra), build.vmReg(rb), build.constUint(c + 1));
    build.inst(IrCmd::JUMP, next);
}

void translateInstSetTableN(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);
    int c = LUAU_INSN_C(*pc);

    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::CHECK_TAG, tb, build.constTag(LUA_TTABLE), fallback);

    IrOp vb = build.inst(IrCmd::LOAD_POINTER, build.vmReg(rb));

    build.inst(IrCmd::CHECK_ARRAY_SIZE, vb, build.constUint(c), fallback);
    build.inst(IrCmd::CHECK_NO_METATABLE, vb, fallback);
    build.inst(IrCmd::CHECK_READONLY, vb, fallback);

    IrOp arrEl = build.inst(IrCmd::GET_ARR_ADDR, vb, build.constUint(c));

    // TODO: per-component loads and stores might be preferable
    IrOp tva = build.inst(IrCmd::LOAD_TVALUE, build.vmReg(ra));
    build.inst(IrCmd::STORE_TVALUE, arrEl, tva);

    build.inst(IrCmd::BARRIER_TABLE_FORWARD, vb, build.vmReg(ra));

    IrOp next = build.blockAtInst(pcpos + 1);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::SET_TABLE, build.vmReg(ra), build.vmReg(rb), build.constUint(c + 1));
    build.inst(IrCmd::JUMP, next);
}

void translateInstGetTable(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);
    int rc = LUAU_INSN_C(*pc);

    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::CHECK_TAG, tb, build.constTag(LUA_TTABLE), fallback);
    IrOp tc = build.inst(IrCmd::LOAD_TAG, build.vmReg(rc));
    build.inst(IrCmd::CHECK_TAG, tc, build.constTag(LUA_TNUMBER), fallback);

    // fast-path: table with a number index
    IrOp vb = build.inst(IrCmd::LOAD_POINTER, build.vmReg(rb));
    IrOp vc = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(rc));

    IrOp index = build.inst(IrCmd::NUM_TO_INDEX, vc, fallback);

    index = build.inst(IrCmd::SUB_INT, index, build.constInt(1));

    build.inst(IrCmd::CHECK_ARRAY_SIZE, vb, index, fallback);
    build.inst(IrCmd::CHECK_NO_METATABLE, vb, fallback);

    IrOp arrEl = build.inst(IrCmd::GET_ARR_ADDR, vb, index);

    // TODO: per-component loads and stores might be preferable
    IrOp arrElTval = build.inst(IrCmd::LOAD_TVALUE, arrEl);
    build.inst(IrCmd::STORE_TVALUE, build.vmReg(ra), arrElTval);

    IrOp next = build.blockAtInst(pcpos + 1);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::GET_TABLE, build.vmReg(ra), build.vmReg(rb), build.vmReg(rc));
    build.inst(IrCmd::JUMP, next);
}

void translateInstSetTable(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);
    int rc = LUAU_INSN_C(*pc);

    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::CHECK_TAG, tb, build.constTag(LUA_TTABLE), fallback);
    IrOp tc = build.inst(IrCmd::LOAD_TAG, build.vmReg(rc));
    build.inst(IrCmd::CHECK_TAG, tc, build.constTag(LUA_TNUMBER), fallback);

    // fast-path: table with a number index
    IrOp vb = build.inst(IrCmd::LOAD_POINTER, build.vmReg(rb));
    IrOp vc = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(rc));

    IrOp index = build.inst(IrCmd::NUM_TO_INDEX, vc, fallback);

    index = build.inst(IrCmd::SUB_INT, index, build.constInt(1));

    build.inst(IrCmd::CHECK_ARRAY_SIZE, vb, index, fallback);
    build.inst(IrCmd::CHECK_NO_METATABLE, vb, fallback);
    build.inst(IrCmd::CHECK_READONLY, vb, fallback);

    IrOp arrEl = build.inst(IrCmd::GET_ARR_ADDR, vb, index);

    // TODO: per-component loads and stores might be preferable
    IrOp tva = build.inst(IrCmd::LOAD_TVALUE, build.vmReg(ra));
    build.inst(IrCmd::STORE_TVALUE, arrEl, tva);

    build.inst(IrCmd::BARRIER_TABLE_FORWARD, vb, build.vmReg(ra));

    IrOp next = build.blockAtInst(pcpos + 1);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::SET_TABLE, build.vmReg(ra), build.vmReg(rb), build.vmReg(rc));
    build.inst(IrCmd::JUMP, next);
}

void translateInstGetImport(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int k = LUAU_INSN_D(*pc);
    uint32_t aux = pc[1];

    IrOp fastPath = build.block(IrBlockKind::Internal);
    IrOp fallback = build.block(IrBlockKind::Fallback);

    build.inst(IrCmd::CHECK_SAFE_ENV, fallback);

    // note: if import failed, k[] is nil; we could check this during codegen, but we instead use runtime fallback
    // this allows us to handle ahead-of-time codegen smoothly when an import fails to resolve at runtime
    IrOp tk = build.inst(IrCmd::LOAD_TAG, build.vmConst(k));
    build.inst(IrCmd::JUMP_EQ_TAG, tk, build.constTag(LUA_TNIL), fallback, fastPath);

    build.beginBlock(fastPath);

    // TODO: per-component loads and stores might be preferable
    IrOp tvk = build.inst(IrCmd::LOAD_TVALUE, build.vmConst(k));
    build.inst(IrCmd::STORE_TVALUE, build.vmReg(ra), tvk);

    IrOp next = build.blockAtInst(pcpos + 2);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::GET_IMPORT, build.vmReg(ra), build.constUint(aux));
    build.inst(IrCmd::JUMP, next);
}

void translateInstGetTableKS(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);
    uint32_t aux = pc[1];

    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::CHECK_TAG, tb, build.constTag(LUA_TTABLE), fallback);

    IrOp vb = build.inst(IrCmd::LOAD_POINTER, build.vmReg(rb));

    IrOp addrSlotEl = build.inst(IrCmd::GET_SLOT_NODE_ADDR, vb, build.constUint(pcpos));

    build.inst(IrCmd::CHECK_SLOT_MATCH, addrSlotEl, build.vmConst(aux), fallback);

    // TODO: per-component loads and stores might be preferable
    IrOp tvn = build.inst(IrCmd::LOAD_NODE_VALUE_TV, addrSlotEl);
    build.inst(IrCmd::STORE_TVALUE, build.vmReg(ra), tvn);

    IrOp next = build.blockAtInst(pcpos + 2);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::FALLBACK_GETTABLEKS, build.constUint(pcpos), build.vmReg(ra), build.vmReg(rb), build.vmConst(aux));
    build.inst(IrCmd::JUMP, next);
}

void translateInstSetTableKS(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);
    uint32_t aux = pc[1];

    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp tb = build.inst(IrCmd::LOAD_TAG, build.vmReg(rb));
    build.inst(IrCmd::CHECK_TAG, tb, build.constTag(LUA_TTABLE), fallback);

    IrOp vb = build.inst(IrCmd::LOAD_POINTER, build.vmReg(rb));

    IrOp addrSlotEl = build.inst(IrCmd::GET_SLOT_NODE_ADDR, vb, build.constUint(pcpos));

    build.inst(IrCmd::CHECK_SLOT_MATCH, addrSlotEl, build.vmConst(aux), fallback);
    build.inst(IrCmd::CHECK_READONLY, vb, fallback);

    // TODO: per-component loads and stores might be preferable
    IrOp tva = build.inst(IrCmd::LOAD_TVALUE, build.vmReg(ra));
    build.inst(IrCmd::STORE_NODE_VALUE_TV, addrSlotEl, tva);

    build.inst(IrCmd::BARRIER_TABLE_FORWARD, vb, build.vmReg(ra));

    IrOp next = build.blockAtInst(pcpos + 2);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::FALLBACK_SETTABLEKS, build.constUint(pcpos), build.vmReg(ra), build.vmReg(rb), build.vmConst(aux));
    build.inst(IrCmd::JUMP, next);
}

void translateInstGetGlobal(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    uint32_t aux = pc[1];

    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp env = build.inst(IrCmd::LOAD_ENV);
    IrOp addrSlotEl = build.inst(IrCmd::GET_SLOT_NODE_ADDR, env, build.constUint(pcpos));

    build.inst(IrCmd::CHECK_SLOT_MATCH, addrSlotEl, build.vmConst(aux), fallback);

    // TODO: per-component loads and stores might be preferable
    IrOp tvn = build.inst(IrCmd::LOAD_NODE_VALUE_TV, addrSlotEl);
    build.inst(IrCmd::STORE_TVALUE, build.vmReg(ra), tvn);

    IrOp next = build.blockAtInst(pcpos + 2);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::FALLBACK_GETGLOBAL, build.constUint(pcpos), build.vmReg(ra), build.vmConst(aux));
    build.inst(IrCmd::JUMP, next);
}

void translateInstSetGlobal(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    uint32_t aux = pc[1];

    IrOp fallback = build.block(IrBlockKind::Fallback);

    IrOp env = build.inst(IrCmd::LOAD_ENV);
    IrOp addrSlotEl = build.inst(IrCmd::GET_SLOT_NODE_ADDR, env, build.constUint(pcpos));

    build.inst(IrCmd::CHECK_SLOT_MATCH, addrSlotEl, build.vmConst(aux), fallback);
    build.inst(IrCmd::CHECK_READONLY, env, fallback);

    // TODO: per-component loads and stores might be preferable
    IrOp tva = build.inst(IrCmd::LOAD_TVALUE, build.vmReg(ra));
    build.inst(IrCmd::STORE_NODE_VALUE_TV, addrSlotEl, tva);

    build.inst(IrCmd::BARRIER_TABLE_FORWARD, env, build.vmReg(ra));

    IrOp next = build.blockAtInst(pcpos + 2);
    FallbackStreamScope scope(build, fallback, next);

    build.inst(IrCmd::FALLBACK_SETGLOBAL, build.constUint(pcpos), build.vmReg(ra), build.vmConst(aux));
    build.inst(IrCmd::JUMP, next);
}

void translateInstConcat(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);
    int rc = LUAU_INSN_C(*pc);

    build.inst(IrCmd::SET_SAVEDPC, build.constUint(pcpos + 1));
    build.inst(IrCmd::CONCAT, build.constUint(rc - rb + 1), build.constUint(rc));

    // TODO: per-component loads and stores might be preferable
    IrOp tvb = build.inst(IrCmd::LOAD_TVALUE, build.vmReg(rb));
    build.inst(IrCmd::STORE_TVALUE, build.vmReg(ra), tvb);

    build.inst(IrCmd::CHECK_GC);
}

void translateInstCapture(IrBuilder& build, const Instruction* pc, int pcpos)
{
    int type = LUAU_INSN_A(*pc);
    int index = LUAU_INSN_B(*pc);

    switch (type)
    {
    case LCT_VAL:
        build.inst(IrCmd::CAPTURE, build.vmReg(index), build.constBool(false));
        break;
    case LCT_REF:
        build.inst(IrCmd::CAPTURE, build.vmReg(index), build.constBool(true));
        break;
    case LCT_UPVAL:
        build.inst(IrCmd::CAPTURE, build.vmUpvalue(index), build.constBool(false));
        break;
    default:
        LUAU_ASSERT(!"Unknown upvalue capture type");
    }
}

} // namespace CodeGen
} // namespace Luau
