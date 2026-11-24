#include <gtest/gtest.h>
#include "helpers/ethernet/uplink/UplinkPolicy.h"

namespace {
const uint32_t kDetect = UplinkPolicy::detectMillis();
const uint32_t kIpWait = UplinkPolicy::ipWaitMillis();
const uint32_t kHold = UplinkPolicy::lossHoldMillis();
}  // namespace

TEST(UplinkPolicy, DisabledNeverHoldsWifi) {
  UplinkPolicy p;  // begin() never called: hardware absent
  p.update(0, true, true);
  p.update(100000, false, false);
  EXPECT_EQ(UplinkPolicy::UPLINK_DISABLED, p.state());
  EXPECT_FALSE(p.holdsWifiOff());
}

TEST(UplinkPolicy, HoldsWifiOffWhileDetectingThenReleasesIfNoCable) {
  UplinkPolicy p;
  p.begin(1000);
  p.update(1000, false, false);
  EXPECT_EQ(UplinkPolicy::UPLINK_DETECTING, p.state());
  EXPECT_TRUE(p.holdsWifiOff());
  p.update(1000 + kDetect - 1, false, false);
  EXPECT_TRUE(p.holdsWifiOff());
  p.update(1000 + kDetect, false, false);
  EXPECT_EQ(UplinkPolicy::UPLINK_NO_LINK, p.state());
  EXPECT_FALSE(p.holdsWifiOff());
}

TEST(UplinkPolicy, LinkUpDuringDetectionHoldsWifiThroughDhcp) {
  UplinkPolicy p;
  p.begin(0);
  p.update(1500, true, false);
  EXPECT_EQ(UplinkPolicy::UPLINK_CONNECTING, p.state());
  EXPECT_TRUE(p.holdsWifiOff());
  p.update(4000, true, true);
  EXPECT_EQ(UplinkPolicy::UPLINK_CONNECTED, p.state());
  EXPECT_TRUE(p.holdsWifiOff());
}

TEST(UplinkPolicy, UnplugFallsBackToWifiAfterShortHold) {
  UplinkPolicy p;
  p.begin(0);
  p.update(2000, true, true);
  ASSERT_EQ(UplinkPolicy::UPLINK_CONNECTED, p.state());
  p.update(10000, false, false);  // cable pulled (address cleared with the link)
  EXPECT_EQ(UplinkPolicy::UPLINK_LINK_LOST, p.state());
  EXPECT_TRUE(p.holdsWifiOff());  // debounce
  p.update(10000 + kHold - 1, false, false);
  EXPECT_TRUE(p.holdsWifiOff());
  p.update(10000 + kHold, false, false);
  EXPECT_EQ(UplinkPolicy::UPLINK_NO_LINK, p.state());
  EXPECT_FALSE(p.holdsWifiOff());  // Wi-Fi may start
}

TEST(UplinkPolicy, BriefFlapNeverReleasesWifi) {
  UplinkPolicy p;
  p.begin(0);
  p.update(2000, true, true);
  p.update(5000, false, false);
  p.update(5000 + kHold - 500, true, false);  // link back before the hold expires
  EXPECT_EQ(UplinkPolicy::UPLINK_CONNECTING, p.state());
  EXPECT_TRUE(p.holdsWifiOff());
  p.update(5000 + kHold + 2000, true, true);
  EXPECT_EQ(UplinkPolicy::UPLINK_CONNECTED, p.state());
}

TEST(UplinkPolicy, ReplugAfterFallbackHoldsWifiOffAgain) {
  UplinkPolicy p;
  p.begin(0);
  p.update(2000, true, true);
  p.update(5000, false, false);
  p.update(5000 + kHold, false, false);
  ASSERT_FALSE(p.holdsWifiOff());
  p.update(60000, true, false);
  EXPECT_EQ(UplinkPolicy::UPLINK_CONNECTING, p.state());
  EXPECT_TRUE(p.holdsWifiOff());
}

TEST(UplinkPolicy, LinkWithoutAddressFallsBackAfterIpWait) {
  UplinkPolicy p;
  p.begin(0);
  p.update(2000, true, false);
  ASSERT_EQ(UplinkPolicy::UPLINK_CONNECTING, p.state());
  p.update(2000 + kIpWait - 1, true, false);
  EXPECT_TRUE(p.holdsWifiOff());
  p.update(2000 + kIpWait, true, false);
  EXPECT_EQ(UplinkPolicy::UPLINK_NO_IP, p.state());
  EXPECT_FALSE(p.holdsWifiOff());
}

TEST(UplinkPolicy, AddressArrivingLateReclaimsUplinkFromWifi) {
  UplinkPolicy p;
  p.begin(0);
  p.update(2000, true, false);
  p.update(2000 + kIpWait, true, false);
  ASSERT_FALSE(p.holdsWifiOff());
  p.update(2000 + kIpWait + 5000, true, true);
  EXPECT_EQ(UplinkPolicy::UPLINK_CONNECTED, p.state());
  EXPECT_TRUE(p.holdsWifiOff());
}

TEST(UplinkPolicy, LostLeaseOnLiveLinkRestartsConnectingWindow) {
  UplinkPolicy p;
  p.begin(0);
  p.update(2000, true, true);
  p.update(100000, true, false);  // lease lost, carrier still up
  EXPECT_EQ(UplinkPolicy::UPLINK_CONNECTING, p.state());
  EXPECT_TRUE(p.holdsWifiOff());
  p.update(100000 + kIpWait, true, false);
  EXPECT_FALSE(p.holdsWifiOff());
}

TEST(UplinkPolicy, UnpluggedWhileStalledGoesStraightToNoLinkAfterHold) {
  UplinkPolicy p;
  p.begin(0);
  p.update(2000, true, false);
  p.update(2000 + kIpWait, true, false);
  ASSERT_EQ(UplinkPolicy::UPLINK_NO_IP, p.state());
  p.update(80000, false, false);
  EXPECT_EQ(UplinkPolicy::UPLINK_LINK_LOST, p.state());
  p.update(80000 + kHold, false, false);
  EXPECT_EQ(UplinkPolicy::UPLINK_NO_LINK, p.state());
}

TEST(UplinkPolicy, SurvivesMillisWraparound) {
  UplinkPolicy p;
  const uint32_t start = 0xFFFFFFFFu - 1000u;  // clock wraps during the detect window
  p.begin(start);
  p.update(start + 1000, false, false);
  EXPECT_TRUE(p.holdsWifiOff());
  p.update(static_cast<uint32_t>(start + kDetect), false, false);
  EXPECT_EQ(UplinkPolicy::UPLINK_NO_LINK, p.state());
}

TEST(UplinkPolicy, StateNamesAreStable) {
  EXPECT_STREQ("connected", UplinkPolicy::stateName(UplinkPolicy::UPLINK_CONNECTED));
  EXPECT_STREQ("connecting", UplinkPolicy::stateName(UplinkPolicy::UPLINK_CONNECTING));
  EXPECT_STREQ("no_link", UplinkPolicy::stateName(UplinkPolicy::UPLINK_NO_LINK));
  EXPECT_STREQ("no_link", UplinkPolicy::stateName(UplinkPolicy::UPLINK_LINK_LOST));
  EXPECT_STREQ("no_ip", UplinkPolicy::stateName(UplinkPolicy::UPLINK_NO_IP));
  EXPECT_STREQ("detecting", UplinkPolicy::stateName(UplinkPolicy::UPLINK_DETECTING));
  EXPECT_STREQ("unavailable", UplinkPolicy::stateName(UplinkPolicy::UPLINK_DISABLED));
}
