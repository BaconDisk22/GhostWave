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
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "SX1262 contract failed at line %d\n", __LINE__); return 1; } } while (false)
int main() {
 Mock mock; gw::Sx1262Adapter adapter(mock); uint8_t data=42;
 CHECK(adapter.channelBusy());
 CHECK(!adapter.transmit(&data,1)); CHECK(mock.calls==0);
 mock.enabled=true; CHECK(!adapter.channelBusy());
 CHECK(!adapter.transmit(nullptr,1)); CHECK(!adapter.transmit(&data,0));
 CHECK(!adapter.transmit(&data,256)); CHECK(mock.calls==0);
 mock.occupied=true; CHECK(adapter.channelBusy()); mock.occupied=false;
 CHECK(!adapter.transmit(&data,1)); CHECK(mock.calls==1);
 mock.outcome=true; CHECK(adapter.transmit(&data,1)); CHECK(mock.calls==2);
 return 0;
}
