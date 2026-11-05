#include <cassert>
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
 assert(adapter.channelBusy());
 assert(!adapter.transmit(&data,1)); assert(mock.calls==0);
 mock.enabled=true; assert(!adapter.channelBusy());
 assert(!adapter.transmit(nullptr,1)); assert(!adapter.transmit(&data,0));
 assert(!adapter.transmit(&data,256)); assert(mock.calls==0);
 mock.occupied=true; assert(adapter.channelBusy()); mock.occupied=false;
 assert(!adapter.transmit(&data,1)); assert(mock.calls==1);
 mock.outcome=true; assert(adapter.transmit(&data,1)); assert(mock.calls==2);
}
