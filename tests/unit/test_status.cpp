#include <gtest/gtest.h>

#include "mm/common/Status.hpp"

namespace mm {
namespace {

TEST(Status, DefaultIsOk) {
    const Status s;
    EXPECT_TRUE(s.is_ok());
    EXPECT_FALSE(s.is_error());
    EXPECT_EQ(s.code(), ErrorCode::Ok);
}

TEST(Status, CarriesCodeAndMessage) {
    const Status s(ErrorCode::Unavailable, "user data stream closed");
    EXPECT_TRUE(s.is_error());
    EXPECT_EQ(s.code(), ErrorCode::Unavailable);
    EXPECT_EQ(s.message(), "user data stream closed");
    EXPECT_EQ(s.to_string(), "Unavailable: user data stream closed");
}

TEST(Status, IsAllocationFreeAndCopyable) {
    static_assert(std::is_trivially_copyable_v<Status>);
    SUCCEED();
}

TEST(Result, HoldsValue) {
    const Result<int> r(42);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.value(), 42);
}

TEST(Result, HoldsError) {
    const Result<int> r(Status(ErrorCode::NotFound, "no such order"));
    ASSERT_TRUE(r.is_error());
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.status().code(), ErrorCode::NotFound);
    EXPECT_EQ(r.value_or(-1), -1);
}

TEST(Result, RejectsSuccessfulFailure) {
    // Constructing an error Result from an ok Status is always a call-site bug;
    // it must not produce a Result that claims failure with no reason.
    const Result<int> r{Status::ok()};
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.status().code(), ErrorCode::Internal);
}

Status failing() { return {ErrorCode::Timeout, "ack timeout"}; }

Status caller(bool& reached_end) {
    MM_RETURN_IF_ERROR(failing());
    reached_end = true;
    return Status::ok();
}

TEST(Status, ReturnIfErrorShortCircuits) {
    bool reached_end = false;
    const Status s = caller(reached_end);
    EXPECT_FALSE(reached_end);
    EXPECT_EQ(s.code(), ErrorCode::Timeout);
}

TEST(ErrorCode, EveryCodeHasAName) {
    for (std::uint16_t i = 0; i <= static_cast<std::uint16_t>(ErrorCode::Internal); ++i) {
        EXPECT_NE(to_string(static_cast<ErrorCode>(i)), "Unknown")
            << "ErrorCode " << i << " has no string form";
    }
}

}  // namespace
}  // namespace mm
