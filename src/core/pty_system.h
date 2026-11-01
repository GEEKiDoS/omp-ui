#pragma once

// Include this wrapper with angle brackets so include_next skips src/core's
// public include directory, whose pty.h is our API rather than the system API.
#include_next <pty.h>
