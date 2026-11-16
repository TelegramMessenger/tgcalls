#include "v2/SignalingSctpConnection.h"

#include "third-party/json11.hpp"

#include <cstdio>
#include <map>
#include <string>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

using tgcalls::SignalingSctpConnection;

// The engines parse Descriptor::config::customParameters with json11 and read
// the resulting object, so the fixtures go through the same parser: json11
// stores a number with a fraction, an exponent or 10+ digits as a double.
SignalingSctpConnection::Options optionsFromJson(std::string const &json) {
    std::string error;
    const auto parsed = json11::Json::parse(json, error);
    return SignalingSctpConnection::Options::fromCustomParameters(parsed.object_items());
}

bool isDefault(SignalingSctpConnection::Options const &options) {
    return options.t1InitTimeoutMs == 400 && options.t1CookieTimeoutMs == 400 && options.maxBackoffMs == 750;
}

void TestAbsentKeysKeepTheDefaults() {
    CHECK_TRUE(isDefault(optionsFromJson("{}")));
    CHECK_TRUE(isDefault(optionsFromJson(R"({"network_use_mtproto": true})")));
}

void TestOverridesApply() {
    const auto options = optionsFromJson(R"({"network_sctp_t1_init_ms": 1500, "network_sctp_t1_cookie_ms": 1200, "network_sctp_max_backoff_ms": 3000})");
    CHECK_TRUE(options.t1InitTimeoutMs == 1500);
    CHECK_TRUE(options.t1CookieTimeoutMs == 1200);
    CHECK_TRUE(options.maxBackoffMs == 3000);
}

void TestEachKeyIsIndependent() {
    const auto options = optionsFromJson(R"({"network_sctp_max_backoff_ms": 2000})");
    CHECK_TRUE(options.t1InitTimeoutMs == 400);
    CHECK_TRUE(options.t1CookieTimeoutMs == 400);
    CHECK_TRUE(options.maxBackoffMs == 2000);
}

void TestZeroKeepsTheDefault() {
    CHECK_TRUE(isDefault(optionsFromJson(R"({"network_sctp_t1_init_ms": 0, "network_sctp_t1_cookie_ms": 0, "network_sctp_max_backoff_ms": 0})")));
}

// A non-positive field makes the socket factory fall back to dcsctp's own
// 1000/1000/3000 ms, which is neither the signaling defaults nor the request.
void TestNegativeKeepsTheDefault() {
    CHECK_TRUE(isDefault(optionsFromJson(R"({"network_sctp_t1_init_ms": -1, "network_sctp_t1_cookie_ms": -400, "network_sctp_max_backoff_ms": -750})")));
}

void TestNonNumbersKeepTheDefault() {
    CHECK_TRUE(isDefault(optionsFromJson(R"({"network_sctp_t1_init_ms": "1500", "network_sctp_t1_cookie_ms": true, "network_sctp_max_backoff_ms": null})")));
    CHECK_TRUE(isDefault(optionsFromJson(R"({"network_sctp_t1_init_ms": [1500], "network_sctp_max_backoff_ms": {"ms": 3000}})")));
}

void TestFractionsTruncate() {
    const auto options = optionsFromJson(R"({"network_sctp_t1_init_ms": 1500.9, "network_sctp_max_backoff_ms": 3e3})");
    CHECK_TRUE(options.t1InitTimeoutMs == 1500);
    CHECK_TRUE(options.maxBackoffMs == 3000);
}

// 60 s is RFC 4960's RTO.Max. A seconds-for-milliseconds slip (1500000) would
// otherwise hold the first INIT retransmission for 25 minutes.
void TestTheRangeIsCapped() {
    const auto atCap = optionsFromJson(R"({"network_sctp_t1_init_ms": 60000, "network_sctp_t1_cookie_ms": 60000, "network_sctp_max_backoff_ms": 60000})");
    CHECK_TRUE(atCap.t1InitTimeoutMs == 60000);
    CHECK_TRUE(atCap.t1CookieTimeoutMs == 60000);
    CHECK_TRUE(atCap.maxBackoffMs == 60000);

    CHECK_TRUE(isDefault(optionsFromJson(R"({"network_sctp_t1_init_ms": 60001, "network_sctp_t1_cookie_ms": 1500000, "network_sctp_max_backoff_ms": 2147483647})")));
}

// Beyond int's range json11's int_value() is an unchecked static_cast from
// double (undefined behaviour; INT_MAX on arm64, INT_MIN on x86_64).
void TestNumbersBeyondIntKeepTheDefault() {
    CHECK_TRUE(isDefault(optionsFromJson(R"({"network_sctp_t1_init_ms": 3000000000, "network_sctp_t1_cookie_ms": 1e10, "network_sctp_max_backoff_ms": -1e300})")));
}

} // namespace

int main() {
    TestAbsentKeysKeepTheDefaults();
    TestOverridesApply();
    TestEachKeyIsIndependent();
    TestZeroKeepsTheDefault();
    TestNegativeKeepsTheDefault();
    TestNonNumbersKeepTheDefault();
    TestFractionsTruncate();
    TestTheRangeIsCapped();
    TestNumbersBeyondIntKeepTheDefault();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
