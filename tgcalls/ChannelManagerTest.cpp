// Host test for ChannelManager, which the custom engines (InstanceV2Impl, GroupInstanceCustomImpl) create their
// channels through.
// - The media engine it owns is terminated before it is destroyed. Since M159 WebRtcVoiceEngine's destructor no longer
//   stops the audio device (it only checks that Terminate ran), so an engine destroyed without Terminate leaves
//   playout and recording running with a callback into freed audio state.
// - A voice channel's audio options reach the audio processing. M123's voice channel applied them engine-wide when it
//   was created and when it started sending; M159's never does (only Init and ApplyGlobalOptions do).
// - tgcalls and the WebRTC library see one AudioOptions layout: WEBRTC_IOS adds a field to it, so both sides must agree
//   on the define (the macOS host builds WebRTC without it, as tgcalls' host targets are built).

#include "ChannelManager.h"
#include "FakeAudioDeviceModule.h"

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/peer_connection_interface.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "call/call.h"
#include "media/base/media_engine.h"
#include "pc/channel.h"
#include "pc/media_factory.h"
#include "rtc_base/thread.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
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

struct Lifecycle {
    std::vector<std::string> events;
    std::vector<webrtc::Thread *> threads;
};

// Records Init, Terminate and its destruction, with the thread each ran on. ChannelManager never reaches the voice
// and video engines in this test.
class RecordingMediaEngine : public webrtc::MediaEngineInterface {
public:
    explicit RecordingMediaEngine(Lifecycle *lifecycle) : _lifecycle(lifecycle) {
    }

    ~RecordingMediaEngine() override {
        record("destroy");
    }

    void Init() override {
        record("init");
    }

    void Terminate() override {
        record("terminate");
    }

    webrtc::VoiceEngineInterface &voice() override {
        std::abort();
    }
    webrtc::VideoEngineInterface &video() override {
        std::abort();
    }
    const webrtc::VoiceEngineInterface &voice() const override {
        std::abort();
    }
    const webrtc::VideoEngineInterface &video() const override {
        std::abort();
    }

private:
    void record(const char *event) {
        _lifecycle->events.push_back(event);
        _lifecycle->threads.push_back(webrtc::Thread::Current());
    }

    Lifecycle *_lifecycle;
};

void TestEngineIsTerminatedOnTheWorkerBeforeItIsDestroyed() {
    webrtc::AutoThread signalingThread;
    std::unique_ptr<webrtc::Thread> workerThread = webrtc::Thread::Create();
    std::unique_ptr<webrtc::Thread> networkThread = webrtc::Thread::CreateWithSocketServer();
    workerThread->Start();
    networkThread->Start();

    Lifecycle lifecycle;
    {
        std::unique_ptr<tgcalls::ChannelManager> channelManager = tgcalls::ChannelManager::Create(
            webrtc::CreateEnvironment(), std::make_unique<RecordingMediaEngine>(&lifecycle), workerThread.get(), networkThread.get());
    }

    CHECK_TRUE((lifecycle.events == std::vector<std::string>{ "init", "terminate", "destroy" }));
    for (webrtc::Thread *thread : lifecycle.threads) {
        CHECK_TRUE(thread == workerThread.get());
    }

    networkThread->Stop();
    workerThread->Stop();
}

// The options the custom engines pass with a voice channel reach the engine's audio processing. Each option is set to
// the opposite of what the engine's Init left (noise suppression is off by default under WEBRTC_IOS and on elsewhere),
// on one thread for worker and network, as tgcalls runs them (StaticThreads).
void TestVoiceChannelOptionsReachTheAudioProcessing() {
    webrtc::AutoThread signalingThread;
    std::unique_ptr<webrtc::Thread> thread = webrtc::Thread::CreateWithSocketServer();
    thread->Start();

    const webrtc::Environment env = webrtc::CreateEnvironment();
    webrtc::scoped_refptr<webrtc::AudioProcessing> audioProcessing = webrtc::BuiltinAudioProcessingBuilder().Build(env);

    webrtc::PeerConnectionFactoryDependencies dependencies;
    dependencies.signaling_thread = webrtc::Thread::Current();
    dependencies.network_thread = thread.get();
    dependencies.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
    dependencies.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();
    dependencies.video_encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
    dependencies.video_decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
    dependencies.adm = tgcalls::FakeAudioDeviceModule::Creator(nullptr, nullptr, tgcalls::FakeAudioDeviceModule::Options())(
        &env.task_queue_factory());
    dependencies.audio_processing_builder = webrtc::CustomAudioProcessing(audioProcessing);
    webrtc::EnableMedia(dependencies);

    std::unique_ptr<tgcalls::ChannelManager> channelManager = tgcalls::ChannelManager::Create(
        env, dependencies.media_factory->CreateMediaEngine(env, dependencies), thread.get(), thread.get());

    std::unique_ptr<webrtc::Call> call;
    thread->BlockingCall([&]() {
        webrtc::CallConfig callConfig(env, thread.get());
        callConfig.audio_state = channelManager->media_engine()->voice().GetAudioState();
        call = dependencies.media_factory->CreateCall(std::move(callConfig));
    });

    const webrtc::AudioProcessing::Config before = audioProcessing->GetConfig();
    webrtc::AudioOptions options;
    options.noise_suppression = !before.noise_suppression.enabled;
    options.highpass_filter = !before.high_pass_filter.enabled;
    tgcalls::VoiceChannel *channel = channelManager->CreateVoiceChannel(
        call.get(), webrtc::MediaConfig(), "0", false, webrtc::CryptoOptions(), options);
    CHECK_TRUE(channel != nullptr);

    const webrtc::AudioProcessing::Config after = audioProcessing->GetConfig();
    CHECK_TRUE(after.noise_suppression.enabled == *options.noise_suppression);
    CHECK_TRUE(after.high_pass_filter.enabled == *options.highpass_filter);

    channelManager->DestroyChannel(channel);
    thread->BlockingCall([&]() {
        call.reset();
    });
    channelManager.reset();
    thread->Stop();
}

// AudioOptions::ToString() is compiled in the library: it reads the options with the library's layout, while this file
// set them with tgcalls'. One layout prints the field that was set; two print it under a neighbouring field's name.
void TestAudioOptionsHaveTheLibrarysLayout() {
    webrtc::AudioOptions options;
    options.auto_gain_control = true;
    const std::string description = options.ToString();
    if (description.find("agc: ") == std::string::npos || description.find("ios_force_software_aec_HACK") != std::string::npos) {
        std::printf("AudioOptions with only auto_gain_control set reads as: %s\n", description.c_str());
    }
    CHECK_TRUE(description.find("agc: ") != std::string::npos);
    CHECK_TRUE(description.find("ios_force_software_aec_HACK") == std::string::npos);
}

}  // namespace

int main() {
    TestEngineIsTerminatedOnTheWorkerBeforeItIsDestroyed();
    TestVoiceChannelOptionsReachTheAudioProcessing();
    TestAudioOptionsHaveTheLibrarysLayout();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
