#pragma once

#include <string_view>

#if MIDORI_ENABLE_DISASSEMBLY

class VmExecutable;

namespace Disassembler
{
    void DisassembleBytecodeStream(const VmExecutable& executable, int proc_index, std::string_view proc_name);

    void DisassembleInstruction(const VmExecutable& executable, int proc_index, int& offset);

};
#endif
