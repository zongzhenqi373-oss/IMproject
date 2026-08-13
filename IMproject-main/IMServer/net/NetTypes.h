#pragma once

#include <cstdint>

// Can hold either a WinSock SOCKET (pointer-sized on x64) or an IPv4 address.
using NetEndpoint = std::uintptr_t;
