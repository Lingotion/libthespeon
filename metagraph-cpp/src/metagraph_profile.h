#pragma once

// Backend is selected by METAGRAPH_PROFILER in CMake; names must be literals.
#if defined(METAGRAPH_PROFILER_TRACY)
#include <tracy/Tracy.hpp>
#define METAGRAPH_PROFILE_SCOPE(name) ZoneScopedN(name)
#define METAGRAPH_PROFILE_SCOPE_DYNAMIC(name, text, size) \
  ZoneScopedN(name);                                      \
  ZoneName(text, size)
#else
#define METAGRAPH_PROFILE_SCOPE(name)
#define METAGRAPH_PROFILE_SCOPE_DYNAMIC(name, text, size)
#endif
