#include "legacy/oracle.h"
#include <bmc/test.h>

TEST(legacy_oracle_derives_the_reference_chan_address) {
    const auto chan = oracle::channel("general", "general", 3);
    CHECK_EQ(chan.address, "BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr");
    CHECK(chan.chan);
}
