// Starts the VP2S_PROFILE sampler (vp2s_profile.cpp) in vp2s_vu1_replay, which has no hook of its own for it.
void startProfiler();

namespace
{
    const bool g_profilerStarted = (startProfiler(), true);
}
