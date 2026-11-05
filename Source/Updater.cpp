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
    // the newest installers are kept as plain files on the releases repo's main branch
    return juce::URL ("https://raw.githubusercontent.com/" + juce::String (repository) + "/main/" + name);
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
    // Runs after RENASOUND has quit: swaps in the new app and opens it. Every step goes to
    // ~/Library/Logs/RENASOUND Update.log; if the swap can't be done, the installer window opens
    // so the new version can be dragged into Applications by hand, and the old app reopens.
    script.replaceWithText (R"SH(#!/bin/bash
APP="$1"; DMG="$2"; PID="$3"
LOG="$HOME/Library/Logs/RENASOUND Update.log"
mkdir -p "$HOME/Library/Logs"
exec >>"$LOG" 2>&1
echo "---- $(date) update: app=$APP dmg=$DMG pid=$PID"

# wait (up to a minute) for RENASOUND to quit
for i in $(seq 1 200); do kill -0 "$PID" 2>/dev/null || break; sleep 0.3; done

# an app macOS is running from a temporary copy (App Translocation) cannot be replaced in place:
# put the new one in Applications instead
case "$APP" in *AppTranslocation*) APP="/Applications/RENASOUND.app"; echo "translocated: using $APP";; esac
DEST_DIR="$(dirname "$APP")"
if [ ! -w "$DEST_DIR" ]; then echo "cannot write to $DEST_DIR"; fi

manual() {
  echo "falling back to the installer window: $1"
  open "$DMG"
  [ -d "$APP" ] && open "$APP"
  exit 1
}

MNT="$(mktemp -d /tmp/renasound-update.XXXXXX)"
hdiutil attach -nobrowse -noautoopen -mountpoint "$MNT" "$DMG" || manual "could not open the download"
[ -d "$MNT/RENASOUND.app" ] || { hdiutil detach -quiet "$MNT"; manual "no RENASOUND.app in the download"; }

# copy the new app next to the old one first, then swap them
rm -rf "$APP.new" "$APP.previous"
if ! ditto "$MNT/RENASOUND.app" "$APP.new"; then
  rm -rf "$APP.new"; hdiutil detach -quiet "$MNT"; manual "could not copy the new app to $DEST_DIR"
fi
hdiutil detach -quiet "$MNT"
if [ -d "$APP" ] && ! mv "$APP" "$APP.previous"; then
  rm -rf "$APP.new"; manual "could not move the old app aside"
fi
if ! mv "$APP.new" "$APP"; then
  [ -d "$APP.previous" ] && mv "$APP.previous" "$APP"
  manual "could not put the new app in place"
fi
rm -rf "$APP.previous"
xattr -dr com.apple.quarantine "$APP" 2>/dev/null
echo "installed, opening $APP"
open "$APP"
)SH");
    script.setExecutePermission (true);
    const auto cmd = "/usr/bin/nohup /bin/bash " + script.getFullPathName().quoted() + " " + app.getFullPathName().quoted() + " "
                   + downloaded.getFullPathName().quoted() + " " + pid + " </dev/null >/dev/null 2>&1 &";
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
