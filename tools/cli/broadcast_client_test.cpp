// The listener's request callbacks against the emulated broadcast server: what the old engine calls for time and
// parts, and the cancellation and teardown rules its fetch threads must keep.

#include "broadcast_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

const char *kFixtureDir = "submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/testdata/broadcast";

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct Completions {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<tgcalls::BroadcastPart> parts;

    std::function<void(tgcalls::BroadcastPart &&)> callback() {
        return [this](tgcalls::BroadcastPart &&part) {
            std::lock_guard<std::mutex> lock(mutex);
            parts.push_back(std::move(part));
            cv.notify_all();
        };
    }

    size_t waitFor(size_t count, int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return parts.size() >= count; });
        return parts.size();
    }
};

void testAvailableUnifiedPartCompletes(GoInt sfu) {
    auto alive = std::make_shared<std::atomic<bool>>(true);
    auto callbacks = makeBroadcastRequestCallbacks(sfu, 2, true, alive, [] {});
    Completions completions;
    int64_t last = GoSfu_BroadcastLastPartMs(sfu);
    callbacks.requestVideoBroadcastPart(last, 1000, 1, tgcalls::VideoChannelDescription::Quality::Full, completions.callback());
    CHECK_TRUE(completions.waitFor(1, 3000) == 1);
    if (completions.parts.size() == 1) {
        auto const &part = completions.parts[0];
        CHECK_TRUE(part.status == tgcalls::BroadcastPart::Status::Success);
        CHECK_TRUE(part.data.size() > 1000);
        CHECK_TRUE(part.timestampMilliseconds == last);
        // Seconds, as the engine reads it.
        double serverSeconds = GoSfu_BroadcastServerTimeMs(sfu) / 1000.0;
        CHECK_TRUE(part.responseTimestamp > serverSeconds - 3.0 && part.responseTimestamp <= serverSeconds + 0.01);
    }
}

void testCancelledFetchNeverCompletes(GoInt sfu) {
    int64_t future = GoSfu_BroadcastServerTimeMs(sfu) / 1000 * 1000 + 10000;

    // Listener A: every fetch cancelled while blocked in the server (3g: a 300 ms round trip), listener alive.
    auto aliveA = std::make_shared<std::atomic<bool>>(true);
    auto callbacksA = makeBroadcastRequestCallbacks(sfu, 1, false, aliveA, [] {});
    Completions completionsA;
    std::vector<std::shared_ptr<tgcalls::BroadcastPartTask>> tasks;
    for (int i = 0; i < 10; i++) {
        tasks.push_back(callbacksA.requestAudioBroadcastPart(future, 1000, completionsA.callback()));
    }
    // Listener B: nothing cancelled, but the listener is gone before the answers come back.
    auto aliveB = std::make_shared<std::atomic<bool>>(true);
    auto callbacksB = makeBroadcastRequestCallbacks(sfu, 1, false, aliveB, [] {});
    Completions completionsB;
    for (int i = 0; i < 10; i++) {
        callbacksB.requestAudioBroadcastPart(future, 1000, completionsB.callback());
    }
    // Control: the same request, neither cancelled nor torn down, completes (NotReady: the part is in the future).
    auto aliveC = std::make_shared<std::atomic<bool>>(true);
    auto callbacksC = makeBroadcastRequestCallbacks(sfu, 1, false, aliveC, [] {});
    Completions completionsC;
    callbacksC.requestAudioBroadcastPart(future, 1000, completionsC.callback());

    sleepMs(50);
    for (auto &task : tasks) {
        task->cancel();
    }
    aliveB->store(false);
    sleepMs(1500);
    CHECK_TRUE(completionsA.waitFor(1, 0) == 0);
    CHECK_TRUE(completionsB.waitFor(1, 0) == 0);
    CHECK_TRUE(completionsC.waitFor(1, 2000) == 1);
    if (completionsC.parts.size() == 1) {
        CHECK_TRUE(completionsC.parts[0].status == tgcalls::BroadcastPart::Status::NotReady);
    }
}

int64_t requestTime(BroadcastRequestCallbacks const &callbacks) {
    std::mutex mutex;
    std::condition_variable cv;
    int64_t value = -1;
    auto task = callbacks.requestCurrentTime([&](int64_t timestamp) {
        std::lock_guard<std::mutex> lock(mutex);
        value = timestamp;
        cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, std::chrono::seconds(2), [&] { return value != -1; });
    return value;
}

void testUnifiedTimeIsLastPart(GoInt sfu) {
    auto alive = std::make_shared<std::atomic<bool>>(true);
    auto callbacks = makeBroadcastRequestCallbacks(sfu, 2, true, alive, [] {});
    int64_t before = GoSfu_BroadcastLastPartMs(sfu);
    int64_t value = requestTime(callbacks);
    int64_t after = GoSfu_BroadcastLastPartMs(sfu);
    CHECK_TRUE(value >= before && value <= after && value % 1000 == 0);
}

void testListenerTimeIsServerTime(GoInt sfu) {
    auto alive = std::make_shared<std::atomic<bool>>(true);
    auto callbacks = makeBroadcastRequestCallbacks(sfu, 2, false, alive, [] {});
    int64_t before = GoSfu_BroadcastServerTimeMs(sfu);
    int64_t value = requestTime(callbacks);
    int64_t after = GoSfu_BroadcastServerTimeMs(sfu);
    CHECK_TRUE(value >= before && value <= after);
}

void testJoinMissingCallsRejoinWithoutCompletion(GoInt sfu) {
    CHECK_TRUE(GoSfu_BroadcastAddClient(sfu, 3, const_cast<char *>("wifi"), const_cast<char *>("join-missing@0")) == 0);
    auto alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<int> rejoins{0};
    auto callbacks = makeBroadcastRequestCallbacks(sfu, 3, true, alive, [&rejoins] { rejoins++; });
    Completions completions;
    callbacks.requestVideoBroadcastPart(GoSfu_BroadcastLastPartMs(sfu), 1000, 1, tgcalls::VideoChannelDescription::Quality::Full, completions.callback());
    sleepMs(500);
    CHECK_TRUE(rejoins.load() == 1);
    CHECK_TRUE(completions.waitFor(1, 0) == 0);
}

} // namespace

// PresentationGroupCall.requestCall sets connection mode None at once (broadcast stops), then getCurrentGroupCall and
// the join take a round trip each before it sets Broadcast again.
void testRejoinStopsAtOnceAndResumesAfterTwoRoundTrips() {
    auto alive = std::make_shared<std::atomic<bool>>(true);
    auto start = std::chrono::steady_clock::now();
    auto elapsedMs = [start]() {
        return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    };
    int64_t stoppedAtMs = -1;
    int64_t resumedAtMs = -1;
    runBroadcastRejoin([&]() { stoppedAtMs = elapsedMs(); }, [&]() { resumedAtMs = elapsedMs(); }, 100, alive);
    CHECK_TRUE(stoppedAtMs >= 0 && stoppedAtMs < 30);
    CHECK_TRUE(resumedAtMs >= 200 && resumedAtMs < 400);

    // Torn down while rejoining: broadcast is not entered again.
    auto leaving = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> stopped{false};
    std::atomic<bool> resumed{false};
    std::thread rejoin([&]() {
        runBroadcastRejoin([&]() { stopped = true; }, [&]() { resumed = true; }, 100, leaving);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    leaving->store(false);
    rejoin.join();
    CHECK_TRUE(stopped.load() && !resumed.load());
}

int main() {
    testRejoinStopsAtOnceAndResumesAfterTwoRoundTrips();
    GoSfu_Init();
    GoInt sfu = GoSfu_Create();
    CHECK_TRUE(sfu > 0);
    CHECK_TRUE(GoSfu_BroadcastConfigure(sfu, const_cast<char *>(kFixtureDir), 300, 30000) == 0);
    CHECK_TRUE(GoSfu_BroadcastAddClient(sfu, 1, const_cast<char *>("3g"), const_cast<char *>("")) == 0);
    CHECK_TRUE(GoSfu_BroadcastAddClient(sfu, 2, const_cast<char *>("wifi"), const_cast<char *>("")) == 0);

    testAvailableUnifiedPartCompletes(sfu);
    testCancelledFetchNeverCompletes(sfu);
    testUnifiedTimeIsLastPart(sfu);
    testListenerTimeIsServerTime(sfu);
    testJoinMissingCallsRejoinWithoutCompletion(sfu);

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
