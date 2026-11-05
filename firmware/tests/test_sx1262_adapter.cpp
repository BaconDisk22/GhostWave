#include <cstdio>
#include <cstdint>
#include "ghostwave/sx1262_adapter.h"
struct Mock : gw::Sx1262Driver {
 bool enabled=false, occupied=false, outcome=false;
 unsigned calls=0;
 bool ready() const override { return enabled; }
 bool busy() override { return occupied; }
 bool send(const uint8_t*, size_t) override { ++calls; return outcome; }
};
int main() {
 Mock mock; gw::Sx1262Adapter adapter(mock); uint8_t data=42;
 if (!(adapter.channelBusy())) { std::fprintf(stderr, "SX1262 adapter contract failure at line %d\n", __LINE__); return 1; }
 if (!(!adapter.transmit(&data,1)); assert(mock.calls==0)) { std::fprintf(stderr, "SX1262 adapter contract failure at line %d\n", __LINE__); return 1; }
 mock.enabled=true; if (!(!adapter.channelBusy())) { std::fprintf(stderr, "SX1262 adapter contract failure at line %d\n", __LINE__); return 1; }
 if (!(!adapter.transmit(nullptr,1)); assert(!adapter.transmit(&data,0))) { std::fprintf(stderr, "SX1262 adapter contract failure at line %d\n", __LINE__); return 1; }
 if (!(!adapter.transmit(&data,256)); assert(mock.calls==0)) { std::fprintf(stderr, "SX1262 adapter contract failure at line %d\n", __LINE__); return 1; }
 mock.occupied=true; if (!(adapter.channelBusy())) { std::fprintf(stderr, "SX1262 adapter contract failure at line %d\n", __LINE__); return 1; } mock.occupied=false;
 if (!(!adapter.transmit(&data,1)); assert(mock.calls==1)) { std::fprintf(stderr, "SX1262 adapter contract failure at line %d\n", __LINE__); return 1; }
 mock.outcome=true; if (!(adapter.transmit(&data,1)); assert(mock.calls==2)) { std::fprintf(stderr, "SX1262 adapter contract failure at line %d\n", __LINE__); return 1; }
}
