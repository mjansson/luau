// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/IrData.h"

#include <string>
#include <vector>

namespace Luau
{
namespace CodeGen
{

const char* getCmdName(IrCmd cmd);
const char* getBlockKindName(IrBlockKind kind);

struct IrToStringContext
{
    std::string& result;
    std::vector<IrBlock>& blocks;
    std::vector<IrConst>& constants;
};

void toString(IrToStringContext& ctx, const IrInst& inst, uint32_t index);
void toString(IrToStringContext& ctx, const IrBlock& block, uint32_t index); // Block title
void toString(IrToStringContext& ctx, IrOp op);

void toString(std::string& result, IrConst constant);

void toStringDetailed(IrToStringContext& ctx, const IrInst& inst, uint32_t index);
void toStringDetailed(IrToStringContext& ctx, const IrBlock& block, uint32_t index); // Block title

std::string toString(IrFunction& function, bool includeDetails);

std::string dump(IrFunction& function);

} // namespace CodeGen
} // namespace Luau
