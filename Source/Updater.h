#pragma once
#include <JuceHeader.h>

// =============================================================================
//  One-click updates from the GitHub "Latest build" release.
//  Each CI build is numbered (RENASOUND_BUILD_NUMBER) and publishes
//  version.txt next to the installers. The updater compares that number with
//  its own, downloads the installer for this platform, and hands it to a small
//  script that waits for RENASOUND to quit, swaps the app and starts it again.
// =============================================================================

#ifndef RENASOUND_BUILD_NUMBER
 #define RENASOUND_BUILD_NUMBER 0
#endif

namespace rs
{

class Updater : private juce::Thread, private juce::AsyncUpdater
{
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, ReadyToInstall, Failed };

    static constexpr const char* repository = "davacada/renasound-daw";
    static int currentBuild() { return RENASOUND_BUILD_NUMBER; }

    Updater();
    ~Updater() override;

    void check();                          // in the background
    void download();                       // after an update was found
    bool installAndRestart();              // after the download; the caller then quits the app

    State getState() const        { return state.load(); }
    float getProgress() const     { return progress.load(); }
    int getLatestBuild() const    { return latestBuild.load(); }
    juce::String getError() const { const juce::ScopedLock sl (lock); return error; }

    std::function<void()> onStateChanged;   // message thread

private:
    std::atomic<State> state { State::Idle };
    std::atomic<float> progress { 0.0f };
    std::atomic<int> latestBuild { 0 };
    std::atomic<bool> wantDownload { false };
    juce::CriticalSection lock;
    juce::String error;
    juce::File downloaded;

    void run() override;
    void handleAsyncUpdate() override { if (onStateChanged) onStateChanged(); }
    void setState (State s) { state = s; triggerAsyncUpdate(); }
    void fail (const juce::String& why) { { const juce::ScopedLock sl (lock); error = why; } setState (State::Failed); }

    static juce::String assetName();
    static juce::URL assetUrl (const juce::String& name);
};

} // namespace rs
