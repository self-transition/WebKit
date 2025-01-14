/*
 * Copyright (C) 2013-2018 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#if ENABLE(DFG_JIT)

#include "CodeBlock.h"
#include "CompilationResult.h"
#include "DFGCommonData.h"
#include "DFGMinifiedGraph.h"
#include "DFGOSREntry.h"
#include "DFGOSRExit.h"
#include "DFGVariableEventStream.h"
#include "ExecutionCounter.h"
#include "JITCode.h"
#include <wtf/SegmentedVector.h>

namespace JSC {

class TrackedReferences;

struct SimpleJumpTable;
struct StringJumpTable;

namespace DFG {

class JITCompiler;

class JITData final : public TrailingArray<JITData, void*> {
    WTF_MAKE_FAST_ALLOCATED;
    friend class LLIntOffsetsExtractor;
public:
    using Base = TrailingArray<JITData, void*>;
    using ExitVector = FixedVector<MacroAssemblerCodeRef<OSRExitPtrTag>>;

    static ptrdiff_t offsetOfExits() { return OBJECT_OFFSETOF(JITData, m_exits); }
    static ptrdiff_t offsetOfIsInvalidated() { return OBJECT_OFFSETOF(JITData, m_isInvalidated); }

    static std::unique_ptr<JITData> create(unsigned poolSize, ExitVector&& exits)
    {
        return std::unique_ptr<JITData> { new (NotNull, fastMalloc(Base::allocationSize(poolSize))) JITData(poolSize, WTFMove(exits)) };
    }

    void setExitCode(unsigned exitIndex, MacroAssemblerCodeRef<OSRExitPtrTag> code)
    {
        m_exits[exitIndex] = WTFMove(code);
    }
    const MacroAssemblerCodeRef<OSRExitPtrTag>& exitCode(unsigned exitIndex) const { return m_exits[exitIndex]; }

    bool isInvalidated() const { return !!m_isInvalidated; }

    void invalidate()
    {
        m_isInvalidated = 1;
    }

private:
    explicit JITData(unsigned size, ExitVector&& exits)
        : Base(size)
        , m_exits(WTFMove(exits))
    {
    }

    ExitVector m_exits;
    uint8_t m_isInvalidated { 0 };
};

class JITCode final : public DirectJITCode {
public:
    JITCode(bool isUnlinked);
    ~JITCode() final;
    
    CommonData* dfgCommon() final;
    JITCode* dfg() final;
    bool isUnlinked() const { return common.isUnlinked(); }
    
    OSREntryData* osrEntryDataForBytecodeIndex(BytecodeIndex bytecodeIndex)
    {
        return tryBinarySearch<OSREntryData, BytecodeIndex>(
            m_osrEntry, m_osrEntry.size(), bytecodeIndex,
            getOSREntryDataBytecodeIndex);
    }

    void finalizeOSREntrypoints(Vector<DFG::OSREntryData>&&);
    
    void reconstruct(
        CodeBlock*, CodeOrigin, unsigned streamIndex, Operands<ValueRecovery>& result);
    
    // This is only applicable if we're at a point where all values are spilled to the
    // stack. Currently, it also has the restriction that the values must be in their
    // bytecode-designated stack slots.
    void reconstruct(
        CallFrame*, CodeBlock*, CodeOrigin, unsigned streamIndex, Operands<std::optional<JSValue>>& result);

#if ENABLE(FTL_JIT)
    // NB. All of these methods take CodeBlock* because they may want to use
    // CodeBlock's logic about scaling thresholds. It should be a DFG CodeBlock.
    
    bool checkIfOptimizationThresholdReached(CodeBlock*);
    void optimizeNextInvocation(CodeBlock*);
    void dontOptimizeAnytimeSoon(CodeBlock*);
    void optimizeAfterWarmUp(CodeBlock*);
    void optimizeSoon(CodeBlock*);
    void forceOptimizationSlowPathConcurrently(CodeBlock*);
    void setOptimizationThresholdBasedOnCompilationResult(CodeBlock*, CompilationResult);
#endif // ENABLE(FTL_JIT)
    
    void validateReferences(const TrackedReferences&) final;
    
    void shrinkToFit(const ConcurrentJSLocker&) final;

    RegisterSet liveRegistersToPreserveAtExceptionHandlingCallSite(CodeBlock*, CallSiteIndex) final;
#if ENABLE(FTL_JIT)
    CodeBlock* osrEntryBlock() { return m_osrEntryBlock.get(); }
    void setOSREntryBlock(VM&, const JSCell* owner, CodeBlock* osrEntryBlock);
    void clearOSREntryBlockAndResetThresholds(CodeBlock* dfgCodeBlock);
#endif

    static ptrdiff_t commonDataOffset() { return OBJECT_OFFSETOF(JITCode, common); }

    std::optional<CodeOrigin> findPC(CodeBlock*, void* pc) final;

    using DirectJITCode::initializeCodeRefForDFG;

    PCToCodeOriginMap* pcToCodeOriginMap() override { return common.m_pcToCodeOriginMap.get(); }
    
private:
    friend class JITCompiler; // Allow JITCompiler to call setCodeRef().

public:
    CommonData common;
    FixedVector<DFG::OSREntryData> m_osrEntry;
    FixedVector<DFG::OSRExit> m_osrExit;
    FixedVector<DFG::SpeculationRecovery> m_speculationRecovery;
    FixedVector<SimpleJumpTable> m_switchJumpTables;
    FixedVector<StringJumpTable> m_stringSwitchJumpTables;
    DFG::VariableEventStream variableEventStream;
    DFG::MinifiedGraph minifiedDFG;

#if ENABLE(FTL_JIT)
    uint8_t neverExecutedEntry { 1 };

    UpperTierExecutionCounter tierUpCounter;

    // For osrEntryPoint that are in inner loop, this maps their bytecode to the bytecode
    // of the outerloop entry points in order (from innermost to outermost).
    //
    // The key may not always be a target for OSR Entry but the list in the value is guaranteed
    // to be usable for OSR Entry.
    HashMap<BytecodeIndex, FixedVector<BytecodeIndex>> tierUpInLoopHierarchy;

    // Map each bytecode of CheckTierUpAndOSREnter to its stream index.
    HashMap<BytecodeIndex, unsigned> bytecodeIndexToStreamIndex;

    enum class TriggerReason : uint8_t {
        DontTrigger,
        CompilationDone,
        StartCompilation,
    };

    // Map each bytecode of CheckTierUpAndOSREnter to its trigger forcing OSR Entry.
    // This can never be modified after it has been initialized since the addresses of the triggers
    // are used by the JIT.
    HashMap<BytecodeIndex, TriggerReason> tierUpEntryTriggers;

    WriteBarrier<CodeBlock> m_osrEntryBlock;
    unsigned osrEntryRetry { 0 };
    bool abandonOSREntry { false };
#endif // ENABLE(FTL_JIT)
};

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
