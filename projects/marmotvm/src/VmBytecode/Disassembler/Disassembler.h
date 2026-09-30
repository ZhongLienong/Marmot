#pragma once

#include <string_view>
#include <cstdio>


class VmExecutable;

namespace Disassembler
{
    void DisassembleBytecodeStream(std::FILE* output, const VmExecutable& executable, int proc_index, std::string_view proc_name);

    void DisassembleInstruction(std::FILE* output, const VmExecutable& executable, int proc_index, int& offset);

};
