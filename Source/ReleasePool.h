#pragma once
#include <JuceHeader.h>
#include <memory>
#include <vector>

// =============================================================================
//  Keeps replaced sounds (samples, sample libraries, drum kits) alive until the
//  audio side has finished with them, then frees them on the main thread.
//  Without this, the last reference can be dropped by the audio thread, which
//  then frees megabytes of memory in the middle of playback: a dropout.
//  Hand it the OLD pointer whenever you swap one out.
// =============================================================================

namespace rs
{

class ReleasePool : private juce::Timer, private juce::DeletedAtShutdown
{
public:
    static ReleasePool& get()
    {
        if (instance == nullptr) instance = new ReleasePool();   // freed with JUCE at shutdown
        return *instance;
    }
    ~ReleasePool() override { stopTimer(); if (instance == this) instance = nullptr; }

    template <typename T>
    void add (const std::shared_ptr<T>& p)
    {
        if (p == nullptr) return;
        const juce::ScopedLock sl (lock);
        items.push_back (std::static_pointer_cast<const void> (std::shared_ptr<const T> (p)));
    }

private:
    ReleasePool() { startTimer (1000); }

    void timerCallback() override
    {
        // only the pool still holds it: nobody else (audio voices included) is using it
        std::vector<std::shared_ptr<const void>> done;
        {
            const juce::ScopedLock sl (lock);
            for (auto it = items.begin(); it != items.end();)
            {
                if (it->use_count() <= 1) { done.push_back (std::move (*it)); it = items.erase (it); }
                else ++it;
            }
        }
        // (freed here, on the main thread, outside the lock)
    }

    juce::CriticalSection lock;
    std::vector<std::shared_ptr<const void>> items;
    static inline ReleasePool* instance = nullptr;
};

} // namespace rs
