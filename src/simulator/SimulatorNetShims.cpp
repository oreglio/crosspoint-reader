#ifdef SIMULATOR
#include <netdb.h>

namespace freeink {

// SecureNet declares hostResolves() for SecureHttpClient's plain-HTTP path, but
// its SecureClient.cpp is built for espressif32 only. Raindrop and Tasks sync
// reach that path over plain http, so the simulator needs a host-side lookup.
bool hostResolves(const char* host) {
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &result) != 0) return false;
  freeaddrinfo(result);
  return true;
}

}  // namespace freeink
#endif
