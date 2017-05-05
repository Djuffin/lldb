include_directories(
  ../../../../llvm/include
  )


add_lldb_library(tiagent SHARED
  agent.cpp

  LINK_COMPONENTS
    Core
    ExecutionEngine
    Support
    MCJIT
    OrcJIT
    AArch64CodeGen
    AArch64Desc
    AArch64Info
    AArch64AsmParser
    CodeGen
    Core
    MC
    MIRParser
    SelectionDAG
    Target

  )
