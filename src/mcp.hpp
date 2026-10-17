// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Umbrella header for the mcp-cpp library. Including this is fine for
// applications that want everything; performance-sensitive headers may
// include the focused headers (e.g. `protocol.hpp`) directly.

#pragma once

#include "client.hpp"
#include "error.hpp"
#include "log.hpp"
#include "plugin.hpp"
#include "plugin_manager.hpp"
#include "protocol.hpp"
#include "server.hpp"
#include "session.hpp"
#if !defined(_WIN32)
#include "stdio_transport.hpp"  // POSIX-only; see that header's guard
#endif
#include "transport.hpp"
