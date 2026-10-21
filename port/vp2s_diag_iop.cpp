// IOP side of the VP2S_DIAG diagnostics: modules requested through SIF, EE RPC clients
// and servers, the IOP emulator state and the latest SIF RPC calls. Reads the runtime's
// kernel HLE state like the runtime debug panel does.

#include "ps2_runtime.h"
#include "Syscalls/Helpers/State.h"

#include <algorithm>
#include <iostream>
#include <vector>

void printIopDiagnostics(PS2Runtime &runtime)
{
    {
        std::lock_guard<std::mutex> lock(g_sif_module_mutex);
        std::cout << "[vp2s-iop] SIF modules: " << g_sif_modules_by_id.size() << std::endl;
        for (const auto &[id, record] : g_sif_modules_by_id)
        {
            std::cout << "[vp2s-iop]   module " << id << " " << (record.loaded ? "loaded" : "NOT loaded")
                      << " refs=" << record.refCount << " " << record.path << std::endl;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        std::cout << "[vp2s-iop] RPC init=" << g_rpc_initialized << " servers=" << g_rpc_servers.size()
                  << " clients=" << g_rpc_clients.size() << std::endl;
        for (const auto &[clientPtr, state] : g_rpc_clients)
        {
            std::cout << std::hex << "[vp2s-iop]   client 0x" << clientPtr << " sid=0x" << state.sid
                      << " lastRpc=0x" << state.last_rpc << std::dec << " busy=" << state.busy << std::endl;
        }
    }

    const ps2x::iop::DebugSnapshot iop = runtime.iopDebugSnapshot();
    std::cout << "[vp2s-iop] IOP emulator: modules=" << iop.emulatorLoadedModules << " threads=" << iop.emulatorThreads
              << " rpcServers=" << iop.emulatorRpcServers << " instructions=" << iop.emulatorInstructions
              << " cycles=" << iop.emulatorCycles << std::endl;
    for (const auto &service : iop.services)
    {
        std::cout << "[vp2s-iop]   service " << service.name << (service.active ? "" : " (inactive)") << " sids:";
        for (uint32_t sid : service.sids)
        {
            std::cout << std::hex << " 0x" << sid << std::dec;
        }
        std::cout << std::endl;
    }
    const size_t shown = std::min<size_t>(iop.diagnostics.size(), 30u);
    for (size_t i = iop.diagnostics.size() - shown; i < iop.diagnostics.size(); ++i)
    {
        std::cout << "[vp2s-iop]   diag: " << iop.diagnostics[i] << std::endl;
    }

    // Latest SIF RPC calls, oldest first.
    std::vector<SifRpcDebugEvent> events;
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        for (const SifRpcDebugEvent &event : g_sif_rpc_debug_history)
        {
            if (event.seq != 0u)
            {
                events.push_back(event);
            }
        }
    }
    std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) { return a.seq < b.seq; });
    const size_t first = events.size() > 10u ? events.size() - 10u : 0u;
    std::cout << "[vp2s-iop] RPC history: " << events.size() << " recorded" << std::endl;
    for (size_t i = first; i < events.size(); ++i)
    {
        const SifRpcDebugEvent &e = events[i];
        std::cout << std::hex << "[vp2s-iop]   #" << std::dec << e.seq << std::hex << " " << e.op << " sid=0x" << e.sid
                  << " rpc=0x" << e.rpcNum << " client=0x" << e.clientPtr << " flags=0x" << e.flags
                  << " ra=0x" << e.ra << std::dec << " result=" << e.result << std::endl;
    }
}
