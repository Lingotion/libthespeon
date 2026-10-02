// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

// Backend is selected by THESPEON_PROFILER in CMake; names must be literals.
#if defined(THESPEON_PROFILER_TRACY)
#include <tracy/Tracy.hpp>
#define THESPEON_PROFILE_SCOPE(name) ZoneScopedN(name)
#define THESPEON_PROFILE_FRAME(name) FrameMarkNamed(name)
#else
#define THESPEON_PROFILE_SCOPE(name)
#define THESPEON_PROFILE_FRAME(name)
#endif
