#include "Updater.h"
#if JUCE_WINDOWS
 #include <process.h>
 #define RS_GETPID _getpid
#else
 #include <unistd.h>
 #define RS_GETPID getpid
#endif

namespace rs
{

Updater::Updater() : juce::Thread ("RENASOUND updater") {}

Updater::~Updater()
{
    cancelPendingUpdate();
    stopThread (4000);
}

juce::String Updater::assetName()
{
   #if JUCE_MAC
    return "RENASOUND-macOS.dmg";
   #elif JUCE_WINDOWS
    return "RENASOUND-Windows.zip";
   #else
    return {};
   #endif
}

juce::URL Updater::assetUrl (const juce::String& name)
{
    return juce::URL ("https://github.com/" + juce::String (repository) + "/releases/latest/download/" + name);
}

void Updater::check()
{
    if (isThreadRunning()) return;
    wantDownload = false;
    setState (State::Checking);
    startThread();
}

void Updater::download()
{
    if (isThreadRunning() || state.load() != State::Available) return;
    wantDownload = true;
    startThread();
}

void Updater::run()
{
    auto open = [] (const juce::URL& url, int timeoutMs)
    {
        return url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                          .withConnectionTimeoutMs (timeoutMs)
                                          .withNumRedirectsToFollow (8));
    };

    if (! wantDownload)
    {
        auto stream = open (assetUrl ("version.txt"), 8000);
        if (stream == nullptr) { fail ("Couldn't reach GitHub to check for updates."); return; }
        const int build = stream->readEntireStreamAsString().trim().getIntValue();
        if (build <= 0) { fail ("The update information on GitHub couldn't be read."); return; }
        latestBuild = build;
        setState (build > currentBuild() ? State::Available : State::UpToDate);
        return;
    }

    if (assetName().isEmpty()) { fail ("Automatic updates aren't available on this system."); return; }
    setState (State::Downloading);
    progress = 0.0f;

    auto stream = open (assetUrl (assetName()), 15000);
    if (stream == nullptr) { fail ("The download couldn't start."); return; }

    auto target = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("RENASOUND-update").getChildFile (assetName());
    target.getParentDirectory().createDirectory();
    target.deleteFile();
    {
        juce::FileOutputStream out (target);
        if (! out.openedOk()) { fail ("The update couldn't be saved."); return; }
        const auto total = stream->getTotalLength();
        juce::HeapBlock<char> buffer (65536);
        juce::int64 done = 0;
        while (! stream->isExhausted() && ! threadShouldExit())
        {
            const int n = stream->read (buffer, 65536);
            if (n <= 0) break;
            out.write (buffer, (size_t) n);
            done += n;
            if (total > 0) progress = (float) ((double) done / (double) total);
            triggerAsyncUpdate();
        }
        if (threadShouldExit()) return;
        if (total > 0 && done < total) { fail ("The download was interrupted."); return; }
    }
    downloaded = target;
    setState (State::ReadyToInstall);
}

bool Updater::installAndRestart()
{
    if (state.load() != State::ReadyToInstall || ! downloaded.existsAsFile()) return false;
    const auto pid = juce::String ((int) RS_GETPID());

   #if JUCE_MAC
    // the .app bundle we're running from
    auto app = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    auto script = downloaded.getSiblingFile ("install.sh");
    script.replaceWithText (
        "#!/bin/bash\n"
        "APP=\"$1\"; DMG=\"$2\"; PID=\"$3\"\n"
        "while kill -0 \"$PID\" 2>/dev/null; do sleep 0.3; done\n"
        "MNT=$(mktemp -d)\n"
        "hdiutil attach -nobrowse -quiet -mountpoint \"$MNT\" \"$DMG\" || exit 1\n"
        "rm -rf \"$APP.previous\"\n"
        "if mv \"$APP\" \"$APP.previous\" && ditto \"$MNT/RENASOUND.app\" \"$APP\"; then rm -rf \"$APP.previous\";\n"
        "else rm -rf \"$APP\"; mv \"$APP.previous\" \"$APP\"; fi\n"
        "hdiutil detach -quiet \"$MNT\"\n"
        "xattr -dr com.apple.quarantine \"$APP\" 2>/dev/null\n"
        "open \"$APP\"\n");
    script.setExecutePermission (true);
    const auto cmd = "nohup /bin/bash " + script.getFullPathName().quoted() + " " + app.getFullPathName().quoted() + " "
                   + downloaded.getFullPathName().quoted() + " " + pid + " >/dev/null 2>&1 &";
    return std::system (cmd.toRawUTF8()) == 0;
   #elif JUCE_WINDOWS
    // unpack the new exe next to the download, then swap it in once we've quit
    auto dir = downloaded.getParentDirectory().getChildFile ("new");
    dir.deleteRecursively();
    juce::ZipFile zip (downloaded);
    if (zip.uncompressTo (dir).failed()) return false;
    auto newExe = dir.getChildFile ("RENASOUND.exe");
    if (! newExe.existsAsFile()) return false;
    auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    auto script = downloaded.getSiblingFile ("install.cmd");
    script.replaceWithText (
        "@echo off\r\n"
        ":wait\r\n"
        "tasklist /FI \"PID eq " + pid + "\" | find \"" + pid + "\" >nul && (timeout /t 1 /nobreak >nul & goto wait)\r\n"
        "copy /Y \"" + newExe.getFullPathName() + "\" \"" + exe.getFullPathName() + "\" >nul\r\n"
        "start \"\" \"" + exe.getFullPathName() + "\"\r\n");
    const auto cmd = "start \"\" /min cmd /c \"" + script.getFullPathName() + "\"";
    return std::system (cmd.toRawUTF8()) == 0;
   #else
    juce::ignoreUnused (pid);
    return false;
   #endif
}

} // namespace rs
