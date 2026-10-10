// Copyright 2026 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include "base/core.hpp"

// lib's parameter vocabulary, declared in iopia's namespace so that iopia
// names it plainly: arguments a function writes (Out), reads and writes
// (InOut), or keeps a reference to (Depend).
namespace iopia {

using lib::Depend;
using lib::InOut;
using lib::Out;

}  // namespace iopia
