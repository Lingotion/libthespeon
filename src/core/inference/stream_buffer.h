// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include "metagraph_runner.h"

namespace thespeon {

// Coalesces the character graph's unusually short first audio chunks before
// forwarding them. Non-audio callbacks retain their original ordering.
metagraph::CallbackHandler PrebufferAudio(
    metagraph::CallbackHandler handler);

}  // namespace thespeon
