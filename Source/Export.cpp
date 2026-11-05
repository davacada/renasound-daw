#include "Export.h"

namespace rs::exporter
{

juce::String extension (Format f)
{
    switch (f)
    {
        case Format::Wav:  return ".wav";
        case Format::Aiff: return ".aiff";
        case Format::Flac: return ".flac";
        case Format::Ogg:  return ".ogg";
    }
    return ".wav";
}

juce::String describe (const Settings& s)
{
    const char* names[] { "WAV", "AIFF", "FLAC", "Ogg Vorbis" };
    juce::String d = names[(int) s.format];
    if (s.format == Format::Ogg) d << ", " << s.oggKbps << " kbps";
    else d << ", " << (s.bitDepth == 32 ? juce::String ("32-bit float") : juce::String (s.bitDepth) + "-bit");
    if (s.sampleRate > 0) d << ", " << juce::String (s.sampleRate / 1000.0, s.sampleRate == 44100 || s.sampleRate == 88200 ? 1 : 0) << " kHz";
    return d;
}

namespace
{
    // ---- the cover picture: its bytes, MIME type and size ----
    struct Cover { juce::MemoryBlock data; juce::String mime; int width = 0, height = 0; };

    Cover loadCover (const juce::File& f)
    {
        Cover c;
        if (! f.existsAsFile()) return c;
        auto img = juce::ImageFileFormat::loadFrom (f);
        if (! img.isValid()) return c;
        const bool isJpeg = f.hasFileExtension ("jpg;jpeg"), isPng = f.hasFileExtension ("png");
        // big pictures are scaled down to 1400 px, which every player handles
        if (img.getWidth() > 1600 || img.getHeight() > 1600 || ! (isJpeg || isPng))
        {
            const float scale = juce::jmin (1.0f, 1400.0f / (float) juce::jmax (img.getWidth(), img.getHeight()));
            img = img.rescaled (juce::jmax (1, juce::roundToInt (img.getWidth() * scale)), juce::jmax (1, juce::roundToInt (img.getHeight() * scale)),
                                juce::Graphics::highResamplingQuality);
            juce::MemoryOutputStream mo (c.data, false);
            juce::JPEGImageFormat jpg;
            jpg.setQuality (0.9f);
            jpg.writeImageToStream (img, mo);
            c.mime = "image/jpeg";
        }
        else
        {
            f.loadFileAsData (c.data);
            c.mime = isJpeg ? "image/jpeg" : "image/png";
        }
        c.width = img.getWidth(); c.height = img.getHeight();
        return c;
    }

    void be32 (juce::MemoryOutputStream& o, juce::uint32 v) { o.writeByte ((char) (v >> 24)); o.writeByte ((char) (v >> 16)); o.writeByte ((char) (v >> 8)); o.writeByte ((char) v); }
    void be24 (juce::MemoryOutputStream& o, juce::uint32 v) { o.writeByte ((char) (v >> 16)); o.writeByte ((char) (v >> 8)); o.writeByte ((char) v); }

    void utf16 (juce::MemoryOutputStream& o, const juce::String& s, bool terminate)
    {
        o.writeByte ((char) 0xff); o.writeByte ((char) 0xfe);   // BOM, little-endian
        auto* p = s.toUTF16().getAddress();
        for (; *p != 0; ++p) o.writeShort ((short) *p);
        if (terminate) o.writeShort (0);
    }

    void frame (juce::MemoryOutputStream& tag, const char* id, const juce::MemoryBlock& body)
    {
        tag.write (id, 4);
        be32 (tag, (juce::uint32) body.getSize());
        tag.writeShort (0);   // flags
        tag.write (body.getData(), body.getSize());
    }

    void textFrame (juce::MemoryOutputStream& tag, const char* id, const juce::String& text)
    {
        if (text.trim().isEmpty()) return;
        juce::MemoryBlock b;
        juce::MemoryOutputStream o (b, false);
        o.writeByte (1);   // UTF-16
        utf16 (o, text.trim(), false);
        o.flush();
        frame (tag, id, b);
    }

    void userFrame (juce::MemoryOutputStream& tag, const juce::String& description, const juce::String& text)
    {
        if (text.trim().isEmpty()) return;
        juce::MemoryBlock b;
        juce::MemoryOutputStream o (b, false);
        o.writeByte (1);
        utf16 (o, description, true);
        utf16 (o, text.trim(), false);
        o.flush();
        frame (tag, "TXXX", b);
    }

    void padEven (juce::MemoryBlock& b) { if (b.getSize() % 2 != 0) b.append ("\0", 1); }

    // appends a chunk to a RIFF (little-endian) or AIFF (big-endian) file and fixes the file's size field
    bool appendChunk (const juce::File& f, const char* id, const juce::MemoryBlock& data, bool bigEndian)
    {
        juce::MemoryBlock chunk;
        {
            juce::MemoryOutputStream o (chunk, false);
            o.write (id, 4);
            if (bigEndian) be32 (o, (juce::uint32) data.getSize()); else o.writeInt ((int) data.getSize());
            o.write (data.getData(), data.getSize());
            if (data.getSize() % 2 != 0) o.writeByte (0);
        }
        {
            juce::FileOutputStream out (f);   // appends
            if (! out.openedOk()) return false;
            out.write (chunk.getData(), chunk.getSize());
        }
        const auto size = (juce::uint32) (f.getSize() - 8);
        juce::FileOutputStream out (f);
        if (! out.openedOk() || ! out.setPosition (4)) return false;
        if (bigEndian) { char b[4] { (char) (size >> 24), (char) (size >> 16), (char) (size >> 8), (char) size }; out.write (b, 4); }
        else out.writeInt ((int) size);
        return true;
    }

    // replaces a FLAC file's comments and picture with ours
    bool tagFlac (const juce::File& f, const SongInfo& info, const Cover& cover)
    {
        juce::MemoryBlock file;
        if (! f.loadFileAsData (file) || file.getSize() < 8 || std::memcmp (file.getData(), "fLaC", 4) != 0) return false;
        auto* d = static_cast<const juce::uint8*> (file.getData());
        size_t pos = 4;
        std::vector<std::pair<int, juce::MemoryBlock>> keep;
        bool last = false;
        while (! last && pos + 4 <= file.getSize())
        {
            last = (d[pos] & 0x80) != 0;
            const int type = d[pos] & 0x7f;
            const size_t len = ((size_t) d[pos + 1] << 16) | ((size_t) d[pos + 2] << 8) | d[pos + 3];
            if (pos + 4 + len > file.getSize()) return false;
            if (type != 1 && type != 4 && type != 6) keep.push_back ({ type, juce::MemoryBlock (d + pos + 4, len) });   // drop padding, comments, pictures
            pos += 4 + len;
        }
        const size_t audioStart = pos;

        juce::MemoryBlock comments;
        {
            juce::MemoryOutputStream o (comments, false);
            const juce::String vendor ("RENASOUND");
            o.writeInt (vendor.getNumBytesAsUTF8()); o.write (vendor.toRawUTF8(), vendor.getNumBytesAsUTF8());
            juce::StringArray items;
            auto add = [&items] (const char* k, const juce::String& v) { if (v.trim().isNotEmpty()) items.add (juce::String (k) + "=" + v.trim()); };
            add ("TITLE", info.title); add ("ARTIST", info.artist); add ("ALBUM", info.album);
            add ("COMPOSER", info.writers); add ("LYRICIST", info.writers); add ("PRODUCER", info.producers);
            add ("ENCODER", "RENASOUND");
            o.writeInt (items.size());
            for (auto& s : items) { o.writeInt ((int) s.getNumBytesAsUTF8()); o.write (s.toRawUTF8(), s.getNumBytesAsUTF8()); }
        }
        keep.push_back ({ 4, comments });

        if (cover.data.getSize() > 0)
        {
            juce::MemoryBlock pic;
            juce::MemoryOutputStream o (pic, false);
            be32 (o, 3);   // front cover
            be32 (o, (juce::uint32) cover.mime.length()); o.write (cover.mime.toRawUTF8(), (size_t) cover.mime.length());
            be32 (o, 0);   // no description
            be32 (o, (juce::uint32) cover.width); be32 (o, (juce::uint32) cover.height); be32 (o, 24); be32 (o, 0);
            be32 (o, (juce::uint32) cover.data.getSize()); o.write (cover.data.getData(), cover.data.getSize());
            o.flush();
            keep.push_back ({ 6, pic });
        }

        juce::MemoryBlock out;
        {
            juce::MemoryOutputStream o (out, false);
            o.write ("fLaC", 4);
            for (size_t i = 0; i < keep.size(); ++i)
            {
                o.writeByte ((char) ((i + 1 == keep.size() ? 0x80 : 0) | keep[i].first));
                be24 (o, (juce::uint32) keep[i].second.getSize());
                o.write (keep[i].second.getData(), keep[i].second.getSize());
            }
            o.write (d + audioStart, file.getSize() - audioStart);
        }
        return f.replaceWithData (out.getData(), out.getSize());
    }
}

juce::MemoryBlock makeId3 (const SongInfo& info)
{
    juce::MemoryBlock frames;
    {
        juce::MemoryOutputStream tag (frames, false);
        textFrame (tag, "TIT2", info.title);
        textFrame (tag, "TPE1", info.artist);
        textFrame (tag, "TALB", info.album);
        textFrame (tag, "TCOM", info.writers);     // writers = composers
        textFrame (tag, "TEXT", info.writers);     // and lyricists
        userFrame (tag, "PRODUCER", info.producers);
        textFrame (tag, "TSSE", "RENASOUND");
        const auto cover = loadCover (info.cover);
        if (cover.data.getSize() > 0)
        {
            juce::MemoryBlock b;
            juce::MemoryOutputStream o (b, false);
            o.writeByte (0);   // ISO-8859-1 for the MIME type
            o.write (cover.mime.toRawUTF8(), (size_t) cover.mime.length()); o.writeByte (0);
            o.writeByte (3);   // front cover
            o.writeByte (0);   // empty description
            o.write (cover.data.getData(), cover.data.getSize());
            o.flush();
            frame (tag, "APIC", b);
        }
    }
    juce::MemoryBlock out;
    juce::MemoryOutputStream o (out, false);
    o.write ("ID3", 3);
    o.writeByte (3); o.writeByte (0);   // version 2.3.0
    o.writeByte (0);                    // flags
    const auto n = (juce::uint32) frames.getSize();
    o.writeByte ((char) ((n >> 21) & 0x7f)); o.writeByte ((char) ((n >> 14) & 0x7f));
    o.writeByte ((char) ((n >> 7) & 0x7f));  o.writeByte ((char) (n & 0x7f));   // "synchsafe" size
    o.write (frames.getData(), frames.getSize());
    o.flush();
    return out;
}

bool write (const juce::AudioBuffer<float>& audioIn, double rate, const Settings& s, const SongInfo& info, const juce::File& file, juce::String& error)
{
    // resample to the rate asked for
    const juce::AudioBuffer<float>* audio = &audioIn;
    juce::AudioBuffer<float> resampled;
    double outRate = rate;
    if (s.sampleRate > 0 && std::abs (s.sampleRate - rate) > 0.5)
    {
        const double ratio = rate / s.sampleRate;   // input samples per output sample
        const int outLen = (int) std::floor (audioIn.getNumSamples() / ratio);
        resampled.setSize (2, outLen);
        for (int ch = 0; ch < 2; ++ch)
        {
            juce::WindowedSincInterpolator interp;
            interp.process (ratio, audioIn.getReadPointer (juce::jmin (ch, audioIn.getNumChannels() - 1)), resampled.getWritePointer (ch), outLen);
        }
        audio = &resampled;
        outRate = s.sampleRate;
    }

    file.deleteFile();
    std::unique_ptr<juce::AudioFormat> format;
    switch (s.format)
    {
        case Format::Wav:  format = std::make_unique<juce::WavAudioFormat>(); break;
        case Format::Aiff: format = std::make_unique<juce::AiffAudioFormat>(); break;
        case Format::Flac: format = std::make_unique<juce::FlacAudioFormat>(); break;
        case Format::Ogg:  format = std::make_unique<juce::OggVorbisAudioFormat>(); break;
    }

    auto options = juce::AudioFormatWriterOptions{}.withSampleRate (outRate).withNumChannels (2);
    if (s.format == Format::Ogg)
    {
        // the closest of the encoder's quality steps to the bit rate asked for
        const auto qualities = format->getQualityOptions();
        int best = 0;
        for (int i = 0; i < qualities.size(); ++i)
            if (std::abs (qualities[i].getIntValue() - s.oggKbps) < std::abs (qualities[best].getIntValue() - s.oggKbps)) best = i;
        options = options.withBitsPerSample (16).withQualityOptionIndex (best);
        auto tag = [&options] (const char* k, const juce::String& v) { if (v.trim().isNotEmpty()) options = options.withMetadata (k, v.trim()); };
        tag (juce::OggVorbisAudioFormat::id3title, info.title);
        tag (juce::OggVorbisAudioFormat::id3artist, info.artist);
        tag (juce::OggVorbisAudioFormat::id3album, info.album);
        juce::String credits;
        if (info.writers.isNotEmpty()) credits << "Written by " << info.writers;
        if (info.producers.isNotEmpty()) credits << (credits.isNotEmpty() ? ". " : "") << "Produced by " << info.producers;
        tag (juce::OggVorbisAudioFormat::id3comment, credits);
        tag (juce::OggVorbisAudioFormat::encoderName, "RENASOUND");
    }
    else
    {
        const int bits = s.format == Format::Flac ? juce::jlimit (16, 24, s.bitDepth) : s.bitDepth;
        options = options.withBitsPerSample (bits);
        if (bits == 32) options = options.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        if (s.format == Format::Wav)
        {
            auto tag = [&options] (const char* k, const juce::String& v) { if (v.trim().isNotEmpty()) options = options.withMetadata (k, v.trim()); };
            tag (juce::WavAudioFormat::riffInfoTitle, info.title);
            tag (juce::WavAudioFormat::riffInfoArtist, info.artist);
            tag (juce::WavAudioFormat::riffInfoProductName, info.album);
            tag (juce::WavAudioFormat::riffInfoMusicBy, info.writers);
            tag (juce::WavAudioFormat::riffInfoProducedBy, info.producers);
            tag (juce::WavAudioFormat::riffInfoSoftware, "RENASOUND");
        }
    }

    {
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (stream == nullptr) { error = "Couldn't write to " + file.getFullPathName(); return false; }
        auto writer = format->createWriterFor (stream, options);
        if (writer == nullptr) { error = "This format can't be written with these settings."; return false; }
        if (! writer->writeFromAudioSampleBuffer (*audio, 0, audio->getNumSamples())) { error = "Writing the file failed."; return false; }
    }

    // the song's details and cover
    switch (s.format)
    {
        case Format::Wav:  { auto id3 = makeId3 (info); padEven (id3); appendChunk (file, "id3 ", id3, false); break; }
        case Format::Aiff: { auto id3 = makeId3 (info); padEven (id3); appendChunk (file, "ID3 ", id3, true); break; }
        case Format::Flac: tagFlac (file, info, loadCover (info.cover)); break;
        case Format::Ogg:
            if (info.cover.existsAsFile())   // Ogg players rarely read embedded covers: put it next to the file
                info.cover.copyFileTo (file.getSiblingFile (file.getFileNameWithoutExtension() + " cover" + info.cover.getFileExtension()));
            break;
    }
    return true;
}

} // namespace rs::exporter
