// SPDX-License-Identifier: AGPL-3.0-only
//
// Umbrella header for the mcpqt library. Including this is fine for
// applications that want everything; performance-sensitive headers may
// include the focused headers (e.g. `protocol.h`) directly.

#pragma once

#include "client.h"
#include "error.h"
#include "log.h"
#include "plugin.h"
#include "plugin_manager.h"
#include "protocol.h"
#include "server.h"
#include "session.h"
#if !defined(_WIN32)
#include "stdio_transport.h" // POSIX-only; see that header's guard
#endif
#include "transport.h"
