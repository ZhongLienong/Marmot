#pragma once

#include <string_view>
#include "Common/BuildConfig/BuildConfig.h"

#if MIDORI_ENABLE_DISASSEMBLY

class MidoriExecutable;

namespace Disassembler
{
    void DisassembleBytecodeStream(const MidoriExecutable& executable, int proc_index, std::string_view proc_name);

    void DisassembleInstruction(const MidoriExecutable& executable, int proc_index, int& offset);
  
};
#endif
