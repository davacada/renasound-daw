#pragma once
#include <JuceHeader.h>

// =============================================================================
//  Writing a mixdown (or a stem) to a file: WAV, AIFF, FLAC or Ogg Vorbis,
//  at a chosen bit depth / bit rate and sample rate, with the song's details
//  (title, artist, album, writers, producers) and cover picture embedded:
//    WAV / AIFF  an ID3 tag chunk (read by Apple Music, Finder, Mp3tag...) plus WAV INFO
//    FLAC        Vorbis comments and a front-cover PICTURE block
//    Ogg         Vorbis comments (the cover is saved next to the file)
// =============================================================================

namespace rs::exporter
{

enum class Format { Wav, Aiff, Flac, Ogg };

struct SongInfo
{
    juce::String title, artist, album, writers, producers;
    juce::File cover;   // an image file, or none
};

struct Settings
{
    Format format = Format::Wav;
    int bitDepth = 24;          // WAV / AIFF: 16, 24 or 32 (float); FLAC: 16 or 24
    int oggKbps = 256;          // Ogg: 128 ... 320
    double sampleRate = 0;      // 0 = the project's rate
};

juce::String extension (Format);
juce::String describe (const Settings&);   // "WAV, 24-bit, 48 kHz"

// writes the audio (2 channels at `rate`) to `file`, resampling if the settings ask for another rate
bool write (const juce::AudioBuffer<float>& audio, double rate, const Settings&, const SongInfo&, const juce::File& file, juce::String& error);

// the ID3v2.3 tag RENASOUND embeds (exposed for testing)
juce::MemoryBlock makeId3 (const SongInfo&);

} // namespace rs::exporter
