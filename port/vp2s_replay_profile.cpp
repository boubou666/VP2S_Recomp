// Starts the VP2S_PROFILE sampler (vp2s_profile.cpp) in the replay tools, which have no hook of their own for it.
void startProfiler();

namespace
{
    const bool g_profilerStarted = (startProfiler(), true);
}
