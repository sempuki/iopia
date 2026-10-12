// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include "base/core.hpp"

// lib's parameter vocabulary, declared in iopia's namespace so that iopia
// names it plainly. Out marks an argument a function writes, InOut one it
// reads and writes, and Depend one it keeps a reference to.
namespace iopia {

using lib::Depend;
using lib::InOut;
using lib::Out;

}  // namespace iopia
