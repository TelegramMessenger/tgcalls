// Host test: GroupInstanceReferenceImpl reports audio levels from its start, without a connection, as
// GroupInstanceCustomImpl does (a levels array every 100 ms in any mode, its own level at least).
// The app's group call state waits for the engine's first levels report (PresentationGroupCall's speaking participants),
// and a voice chat's stream listener never gets a connection: the engine gets no join answer, ICE never connects.
// Reporting levels only once ICE was connected left such a call's state, member list and Raise Hand never updated.

#include "group/GroupInstanceReferenceImpl.h"
#include "FakeAudioDeviceModule.h"
#include "StaticThreads.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

bool waitFor(std::atomic<bool> const &flag, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return flag.load();
}

// As a stream listener's call: the app asks for the join payload, the join answers with a stream, and the engine's join
// response is never set.
void TestLevelsAreReportedWithoutAConnection() {
    std::atomic<int> levelReports{0};
    std::atomic<bool> sawOwnLevel{false};
    std::atomic<bool> connected{false};

    tgcalls::FakeAudioDeviceModule::Options deviceOptions;
    deviceOptions.samples_per_sec = 48000;
    deviceOptions.num_channels = 2;

    tgcalls::GroupInstanceDescriptor descriptor;
    descriptor.threads = tgcalls::StaticThreads::getThreads();
    descriptor.createAudioDeviceModule = tgcalls::FakeAudioDeviceModule::Creator(nullptr, nullptr, deviceOptions);
    descriptor.networkStateUpdated = [&connected](tgcalls::GroupNetworkState state) {
        if (state.isConnected) {
            connected.store(true);
        }
    };
    descriptor.audioLevelsUpdated = [&levelReports, &sawOwnLevel](tgcalls::GroupLevelsUpdate const &update) {
        levelReports.fetch_add(1);
        for (const auto &level : update.updates) {
            if (level.ssrc == 0) {
                sawOwnLevel.store(true);
            }
        }
    };

    auto instance = std::make_unique<tgcalls::GroupInstanceReferenceImpl>(std::move(descriptor));

    std::atomic<bool> gotPayload{false};
    instance->emitJoinPayload([&gotPayload](tgcalls::GroupJoinPayload const &) {
        gotPayload.store(true);
    });
    CHECK_TRUE(waitFor(gotPayload, std::chrono::milliseconds(5000)));

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    std::printf("level reports in 1 s without a connection: %d (connected: %d)\n", levelReports.load(), connected.load() ? 1 : 0);
    CHECK_TRUE(!connected.load());
    CHECK_TRUE(levelReports.load() >= 3);
    CHECK_TRUE(sawOwnLevel.load());

    std::atomic<bool> stopped{false};
    instance->stop([&stopped]() {
        stopped.store(true);
    });
    CHECK_TRUE(waitFor(stopped, std::chrono::milliseconds(5000)));
    instance.reset();
}

}  // namespace

int main() {
    TestLevelsAreReportedWithoutAConnection();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
