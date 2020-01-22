// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
//
// Copyright 2003-2020 by Todd Strader. This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License.
// Version 2.0.
//
// Verilator is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//=========================================================================
///
/// \file
/// \brief Verilator: Common functions for replay tool
///
///     See verilator_replay
///
/// Code available from: http://www.veripool.org/verilator
///
//=========================================================================

#include "verilated_replay.h"
#include <cstring>

// TODO -- collapse into constructor?
int VerilatedReplay::init() {
    createMod();
    addSignals();
    for (SignalNameMap::iterator it = m_inputNames.begin(); it != m_inputNames.end(); ++it) {
        addInputName(it->first);
    }
    for (SignalNameMap::iterator it = m_outputNames.begin(); it != m_outputNames.end(); ++it) {
        addOutputName(it->first);
    }
    openFst(m_fstName);
    searchFst(NULL);
    m_time = fstReaderGetStartTime(m_fstp);
    // TODO -- use FST timescale
    m_simTime = m_time;

    for (VarMap::iterator it = m_inputs.begin(); it != m_inputs.end();
         ++it) {
        VL_PRINTF("input %s = %d\n", it->second.fullName.c_str(), it->first);
        fstReaderSetFacProcessMask(m_fstp, it->first);
        // TODO -- double check the size hasn't changed or just defer looking at size until here
        m_inputHandles[it->first] = FstSignal(it->second.hier.u.var.length,
                m_inputNames[it->second.fullName].signal);
    }

    for (VarMap::iterator it = m_outputs.begin(); it != m_outputs.end();
         ++it) {
        VL_PRINTF("output %s = %d\n", it->second.fullName.c_str(), it->first);
        fstReaderSetFacProcessMask(m_fstp, it->first);
        size_t bits = it->second.hier.u.var.length;
        size_t bytes = (bits + 7) / 8;
        vluint8_t* buffer = new vluint8_t [bytes];
        // TODO -- double check the size hasn't changed or just defer looking at size until here
        m_outputHandles[it->first] = FstSignal(bits, m_outputNames[it->second.fullName].signal,
                                               buffer);
    }

    return 0;
}

VerilatedReplay::~VerilatedReplay() {
    fstReaderClose(m_fstp);

    for (SignalHandleMap::iterator it = m_outputHandles.begin(); it != m_outputHandles.end(); ++it) {
        delete [] it->second.expected;
    }

#if VM_TRACE
    if (m_tfp) m_tfp->close();
#endif
    delete(m_modp);
}

void VerilatedReplay::addInput(const std::string& fullName, vluint8_t* signal, size_t size) {
    m_inputNames[fullName] = FstSignal(size, signal);
}

void VerilatedReplay::addOutput(const std::string& fullName, vluint8_t* signal, size_t size) {
    m_outputNames[fullName] = FstSignal(size, signal);
}

int VerilatedReplay::replay() {
    // TODO -- lockless ring buffer for separate reader/replay threads
    // TODO -- should I be using fstReaderIterBlocks instead? (only one CB)
    // It appears that 0 is the error return code
    if (fstReaderIterBlocks2(m_fstp, &VerilatedReplay::fstCallback,
                             &VerilatedReplay::fstCallbackVarlen, this, NULL) == 0) {
        VL_PRINTF("Error iterating FST\n");
        exit(-1);
    }

    // One final eval + trace since we only eval on time changes
    eval();
    trace();
    final();

    return 0;
}

void VerilatedReplay::fstCb(uint64_t time, fstHandle facidx,
                            const unsigned char* valuep, uint32_t len) {
    // Watch for new time steps and eval before we start working on the new time
    if (m_time != time) {
        eval();
        trace();
        m_time = time;
        // TODO -- use FST timescale
        m_simTime = m_time;
    }

    // TODO -- remove
    VL_PRINTF("%lu %u %s\n", time, facidx, valuep);

    if (m_outputHandles.empty() || m_inputHandles.find(facidx) != m_inputHandles.end()) {
        handleInput(facidx, valuep, len);
    } else {
        handleOutput(facidx, valuep, len);
    }
}

void VerilatedReplay::copyValue(unsigned char* to, const unsigned char* valuep, uint32_t len) {
    vluint8_t byte = 0;
    for (size_t bit = 0; bit < len; ++bit) {
        char value = valuep[len - 1 - bit];
        if (value == '1') byte |= 1 << (bit % 8);
        if ((bit + 1) % 8 == 0 || bit == len - 1) {
            *to = byte;
            ++to;
            byte = 0;
        }
    }
}

void VerilatedReplay::handleInput(fstHandle facidx, const unsigned char* valuep, uint32_t len) {
    // TODO -- is len always right, or should we use strlen() or something?
    // TODO -- handle values other than 0/1, what can show up here?
    vluint8_t* signal = m_inputHandles[facidx].signal;
    copyValue(signal, valuep, len);
}

void VerilatedReplay::handleOutput(fstHandle facidx, const unsigned char* valuep, uint32_t len) {
    FstSignal& fstSignal = m_outputHandles[facidx];
    size_t bytes = (len + 7) / 8;
    copyValue(fstSignal.expected, valuep, len);
}

void VerilatedReplay::fstCallbackVarlen(void* userDatap, uint64_t time, fstHandle facidx,
                                        const unsigned char* valuep, uint32_t len) {
    reinterpret_cast<VerilatedReplay*>(userDatap)->fstCb(time, facidx, valuep, len);
}

void VerilatedReplay::fstCallback(void* userDatap, uint64_t time, fstHandle facidx,
                                  const unsigned char* valuep) {
    // Cribbed from fstminer.c in the gtkwave repo
    uint32_t len;

    if(valuep) {
        len = strlen((const char *)valuep);
    } else {
        len = 0;
    }

    fstCallbackVarlen(userDatap, time, facidx, valuep, len);
}

void VerilatedReplay::outputCheck() {
    for (SignalHandleMap::iterator it = m_outputHandles.begin(); it != m_outputHandles.end(); ++it) {
        size_t bytes = (it->second.bits + 7) / 8;
        if (std::memcmp(it->second.expected, it->second.signal, bytes)) {
            fstHandle facidx = it->first;
            // TODO -- timescale, actually print out values with Verilator runtime, etc.
            VL_PRINTF("Miscompare: %s @ %ld\n", m_outputs[facidx].fullName.c_str(),
                      m_time);
        }
    }
}

void VerilatedReplay::createMod() {
    // TODO -- maybe get rid of the need for VM_PREFIX by generating these things
    m_modp = new VM_PREFIX;
    // TODO -- make VerilatedModule destructor virtual so we can delete from the base class?
#if VM_TRACE
    Verilated::traceEverOn(true);
    m_tfp = new VerilatedFstC;
    m_modp->trace(m_tfp, 99);
    // TODO -- command line parameter
    m_tfp->open("replay.fst");
#endif  // VM_TRACE
}

void VerilatedReplay::eval() {
    // TODO -- make eval, trace and final virtual methods of VerilatedModule?
    m_modp->eval();
    outputCheck();
}

void VerilatedReplay::trace() {
#if VM_TRACE
    // TODO -- make this optional
    if (m_tfp) m_tfp->dump(m_simTime);
#endif  // VM_TRACE
}

void VerilatedReplay::final() {
    m_modp->final();
}