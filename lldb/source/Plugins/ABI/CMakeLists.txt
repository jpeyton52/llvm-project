set_property(DIRECTORY PROPERTY LLDB_PLUGIN_KIND ABI)
set_property(DIRECTORY PROPERTY LLDB_TOLERATED_PLUGIN_DEPENDENCIES
  ProcessUtility
  TypeSystem
)

foreach(target AArch64 ARM ARC Hexagon LoongArch Mips MSP430 PowerPC RISCV SystemZ X86)
  if (${target} IN_LIST LLVM_TARGETS_TO_BUILD)
    add_subdirectory(${target})
  endif()
endforeach()
