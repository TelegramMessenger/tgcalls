#include "broadcast_mode.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <unistd.h>
#include <vector>

#include "broadcast_client.h"
#include "broadcast_listener.h"
#include "broadcast_metrics.h"
#include "group_participant.h"
#include "third-party/json11.hpp"

namespace {

constexpr int kListenerId = 100;
constexpr int kClientId = 1;
constexpr int kSpeakers = 3;
constexpr int kTilesSwapAtS = 10;
constexpr int kPromoteAtS = 12;
constexpr int kRetainMs = 30000;

// The round trip of a link profile (tools/go_sfu/broadcast_server.go's profiles): the delay before a rejoin.
int profileRoundTripMs(const std::string &profile) {
    if (profile == "wifi") return 20;
    if (profile == "lte") return 60;
    if (profile == "poor-lte") return 150;
    if (profile == "3g") return 300;
    if (profile == "step-down") return 80;
    auto comma = profile.find(',');
    if (comma != std::string::npos) {
        return std::atoi(profile.c_str() + comma + 1);
    }
    return 100;
}

json11::Json optionalJson(std::optional<int64_t> value) {
    return value ? json11::Json((double)*value) : json11::Json(nullptr);
}

tgcalls::VideoChannelDescription tile(ParticipantState const *speaker, int index, tgcalls::VideoChannelDescription::Quality quality) {
    tgcalls::VideoChannelDescription description;
    description.audioSsrc = speaker->audioSsrc;
    description.userId = cliUserId(index);
    description.endpointId = "ep-" + std::to_string(index);
    description.minQuality = tgcalls::VideoChannelDescription::Quality::Thumbnail;
    description.maxQuality = quality;
    return description;
}

json11::Json audioJson(broadcast::AudioReport const &report) {
    json11::Json::object heard;
    for (const auto &[key, value] : report.heardBlocks) {
        heard[key] = value;
    }
    return json11::Json::object{
        {"startMs", optionalJson(report.startMs)},
        {"stallCount", report.stallCount},
        {"stallMs", (double)report.stallMs},
        {"latencyMedianMs", optionalJson(report.latencyMedianMs)},
        {"latencyMaxMs", optionalJson(report.latencyMaxMs)},
        {"heardBlocks", heard},
    };
}

json11::Json videoJson(broadcast::VideoReport const &report) {
    json11::Json::object widths;
    for (const auto &[width, count] : report.widths) {
        widths[std::to_string(width)] = count;
    }
    return json11::Json::object{
        {"startMs", optionalJson(report.startMs)},
        {"frames", report.frames},
        {"repeatedFrames", report.repeatedFrames},
        {"outOfOrderFrames", report.outOfOrderFrames},
        {"droppedFrames", report.droppedFrames},
        {"unplacedFrames", report.unplacedFrames},
        {"stallCount", report.stallCount},
        {"stallMs", (double)report.stallMs},
        {"latencyMedianMs", optionalJson(report.latencyMedianMs)},
        {"latencyMaxMs", optionalJson(report.latencyMaxMs)},
        {"widths", widths},
    };
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

int runBroadcastMode(const BroadcastRunConfig &config) {
    gGroupQuiet = config.quiet;
    gGroupStartTime = std::chrono::steady_clock::now();
    const std::string &scenario = config.scenario;
    if (scenario != "audio" && scenario != "tiles" && scenario != "unified" && scenario != "handoff") {
        fprintf(stderr, "Error: --broadcast-scenario must be audio, tiles, unified or handoff\n");
        return 1;
    }
    bool unified = scenario == "unified";

    GoSfu_Init();
    GoInt sfu = GoSfu_Create();
    if (sfu <= 0) {
        fprintf(stderr, "Error: GoSfu_Create failed\n");
        return 1;
    }
    if (GoSfu_BroadcastConfigure(sfu, const_cast<char *>(config.fixtureDir.c_str()), config.availabilityDelayMs, kRetainMs) != 0) {
        fprintf(stderr, "Error: no broadcast fixtures in %s (run from the repository root, or pass --broadcast-fixtures)\n", config.fixtureDir.c_str());
        return 1;
    }
    std::string endpoints = "[";
    for (int i = 0; i < kSpeakers; i++) {
        endpoints += (i ? "," : "") + std::string("{\"participant\":") + std::to_string(i) + ",\"endpoint\":\"ep-" + std::to_string(i) + "\"}";
    }
    endpoints += "]";
    GoSfu_BroadcastSetVideoEndpoints(sfu, const_cast<char *>(endpoints.c_str()));

    auto threads = tgcalls::StaticThreads::getThreads();

    std::vector<std::unique_ptr<ParticipantState>> speakers;
    std::map<uint32_t, std::string> speakerNames;
    if (!unified) {
        for (int i = 0; i < kSpeakers; i++) {
            broadcast::SpeakerTone tone = i == 0 ? broadcast::SpeakerTone::marker()
                                                 : broadcast::SpeakerTone::fixed(broadcast::kSpeakerFrequencies[i - 1]);
            auto speaker = createParticipant(i, false, sfu, threads, config.quiet, false, &speakers, false, false, false,
                                             VideoFeed::Source, false, tone);
            if (!speaker) {
                fprintf(stderr, "Error: speaker %d failed to join\n", i);
                return 1;
            }
            speakerNames[speaker->audioSsrc] = "speaker" + std::to_string(i);
            speakers.push_back(std::move(speaker));
        }
        // The listener joins a call already under way: every speaker connected and its audio reaching the SFU for
        // a few seconds, so the parts it asks for first are not from before anyone spoke.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        bool flowing = false;
        while (std::chrono::steady_clock::now() < deadline) {
            flowing = true;
            for (const auto &speaker : speakers) {
                flowing = flowing && speaker->connected.load() && GoSfu_QuerySsrcPackets(sfu, (GoUint)speaker->audioSsrc) > 0;
            }
            if (flowing) {
                break;
            }
            sleepMs(50);
        }
        if (!flowing) {
            fprintf(stderr, "Error: the speakers' audio never reached the SFU\n");
            return 1;
        }
        sleepMs(3000);
    }

    if (GoSfu_BroadcastAddClient(sfu, kClientId, const_cast<char *>(config.profile.c_str()), const_cast<char *>(config.events.c_str())) != 0) {
        fprintf(stderr, "Error: bad --broadcast-profile %s or --broadcast-events %s\n", config.profile.c_str(), config.events.c_str());
        return 1;
    }

    auto renderer = std::make_shared<broadcast::RecordingRenderer>(unified);
    auto levels = std::make_shared<broadcast::LevelRecorder>();
    auto listenerSlot = std::make_shared<std::atomic<ParticipantState *>>(nullptr);
    int rejoinDelayMs = profileRoundTripMs(config.profile);
    std::atomic<int> rejoins{0};

    BroadcastListenerOptions options;
    options.clientId = kClientId;
    options.unified = unified;
    options.renderer = renderer;
    options.onLevels = [levels](tgcalls::GroupLevelsUpdate const &update) {
        levels->record(update);
    };
    options.onRejoinNeeded = [listenerSlot, rejoinDelayMs, &rejoins]() {
        rejoins++;
        std::thread([listenerSlot, rejoinDelayMs]() {
            ParticipantState *listener = listenerSlot->load();
            if (!listener) {
                return;
            }
            runBroadcastRejoin([listener]() { stopBroadcast(listener); },
                               [listener]() { resumeBroadcast(listener); }, rejoinDelayMs, listener->broadcastAlive);
        }).detach();
    };

    int64_t startMs = broadcast::wallClockMs();
    auto listener = createBroadcastListener(kListenerId, sfu, threads, options);
    listenerSlot->store(listener.get());

    std::map<std::string, std::shared_ptr<broadcast::CodedFrameSink>> sinks;
    if (scenario == "tiles") {
        for (int i = 0; i < kSpeakers; i++) {
            auto sink = std::make_shared<broadcast::CodedFrameSink>();
            sinks["ep-" + std::to_string(i)] = sink;
            listener->instance->addIncomingVideoOutput("ep-" + std::to_string(i), sink);
        }
        listener->instance->setRequestedVideoChannels({
            tile(speakers[0].get(), 0, tgcalls::VideoChannelDescription::Quality::Thumbnail),
            tile(speakers[1].get(), 1, tgcalls::VideoChannelDescription::Quality::Medium),
            tile(speakers[2].get(), 2, tgcalls::VideoChannelDescription::Quality::Full),
        });
    } else if (unified) {
        auto sink = std::make_shared<broadcast::CodedFrameSink>();
        sinks["unified"] = sink;
        listener->instance->addIncomingVideoOutput("unified", sink);
    }

    std::optional<int64_t> promotedAtMs;
    bool swapped = false;
    int64_t endMs = startMs + (int64_t)config.durationS * 1000;
    while (broadcast::wallClockMs() < endMs) {
        sleepMs(50);
        int64_t elapsedMs = broadcast::wallClockMs() - startMs;
        if (scenario == "tiles" && !swapped && elapsedMs >= kTilesSwapAtS * 1000) {
            swapped = true;
            listener->instance->setRequestedVideoChannels({
                tile(speakers[0].get(), 0, tgcalls::VideoChannelDescription::Quality::Full),
                tile(speakers[1].get(), 1, tgcalls::VideoChannelDescription::Quality::Medium),
                tile(speakers[2].get(), 2, tgcalls::VideoChannelDescription::Quality::Thumbnail),
            });
            groupLog("Broadcast", "tiles: ep-0 -> full, ep-2 -> thumbnail");
        }
        if (scenario == "handoff" && !promotedAtMs && elapsedMs >= kPromoteAtS * 1000) {
            promotedAtMs = broadcast::wallClockMs();
            if (!promoteListenerToSpeaker(listener.get(), sfu)) {
                fprintf(stderr, "Error: promotion failed\n");
            }
        }
    }

    char *statsRaw = GoSfu_BroadcastStats(sfu, kClientId);
    std::string statsError;
    json11::Json serverStats = json11::Json::parse(statsRaw ? statsRaw : "{}", statsError);
    if (statsRaw) {
        GoSfu_Free(statsRaw);
    }

    listenerSlot->store(nullptr);
    stopParticipant(listener.get(), sfu);
    for (auto &speaker : speakers) {
        stopParticipant(speaker.get(), sfu);
    }

    broadcast::AudioReport audio = renderer->audioReport(startMs, promotedAtMs);
    json11::Json::object video;
    for (const auto &[endpoint, sink] : sinks) {
        video[endpoint] = videoJson(sink->report(startMs));
    }
    json11::Json::object levelsJson;
    for (const auto &[name, stats] : levels->stats(speakerNames)) {
        levelsJson[name] = json11::Json::object{
            {"meanLevel", stats.meanLevel},
            {"speakingFraction", stats.speakingFraction},
            {"updates", stats.updates},
        };
    }
    json11::Json handoff = nullptr;
    if (promotedAtMs) {
        broadcast::HandoffReport report = renderer->handoffReport(startMs, *promotedAtMs);
        handoff = json11::Json::object{
            {"promotedAtMs", (double)report.promotedAtMs},
            {"gapBlocks", report.gapBlocks},
            {"overlapBlocks", report.overlapBlocks},
            {"rtcFirstHeardMs", optionalJson(report.rtcFirstHeardMs)},
        };
    }
    json11::Json result = json11::Json::object{
        {"schema", 1},
        {"implementation", "engine"},
        {"scenario", scenario},
        {"profile", config.profile},
        {"events", config.events},
        {"availabilityDelayMs", config.availabilityDelayMs},
        {"durationS", config.durationS},
        {"repetition", config.repetition},
        {"rejoins", rejoins.load()},
        {"audio", audioJson(audio)},
        {"video", video},
        {"levels", levelsJson},
        {"handoff", handoff},
        {"server", serverStats},
    };
    std::string text = result.dump();

    bool written = true;
    if (!config.reportPath.empty()) {
        FILE *file = fopen(config.reportPath.c_str(), "w");
        if (file) {
            fwrite(text.data(), 1, text.size(), file);
            fputc('\n', file);
            fclose(file);
        } else {
            fprintf(stderr, "Error: cannot write %s\n", config.reportPath.c_str());
            written = false;
        }
    }
    printf("Broadcast %s on %s: audio start %s ms, latency %s ms, stalls %d (%lld ms), heard marker %d / 2300 %d / 2900 %d / 1000 %d\n",
           scenario.c_str(), config.profile.c_str(),
           audio.startMs ? std::to_string(*audio.startMs).c_str() : "-",
           audio.latencyMedianMs ? std::to_string(*audio.latencyMedianMs).c_str() : "-",
           audio.stallCount, (long long)audio.stallMs, audio.heardBlocks["marker"], audio.heardBlocks["2300"],
           audio.heardBlocks["2900"], audio.heardBlocks["1000"]);
    if (config.reportPath.empty()) {
        printf("%s\n", text.c_str());
    }
    fflush(stdout);

    GoSfu_Destroy(sfu);
    GoSfu_Shutdown();
    _exit(written ? 0 : 1);
}
