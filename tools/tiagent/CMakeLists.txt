include_directories(
  ../../../../llvm/include
  /usr/local/buildtools/java/jdk/include
  /usr/local/buildtools/java/jdk/include/linux
  )

add_lldb_library(tiagent SHARED
  agent.cpp
  jni_wrapper.cpp

  LINK_COMPONENTS
    Core
    ExecutionEngine
    Support
    MCJIT
    OrcJIT
    # AArch64CodeGen
    # AArch64Desc
    # AArch64Info
    # AArch64AsmParser
    # X86CodeGen
    # X86Desc
    # X86Info
    # X86AsmParser
    CodeGen
    Core
    MC
    MIRParser
    SelectionDAG
    Target
    ${LLVM_LINK_COMPONENTS}

  )
