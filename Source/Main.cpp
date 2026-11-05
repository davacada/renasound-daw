#include "MainComponent.h"
#if JUCE_WINDOWS
 #define NOMINMAX
 #include <windows.h>
 #include <shlobj.h>
#endif

// Windows: .renasound files get RENASOUND's document icon and open in RENASOUND.
// (The app is a single .exe you can keep anywhere, so it registers itself for the current user.)
static void registerProjectFiles()
{
   #if JUCE_WINDOWS
    using Reg = juce::WindowsRegistry;
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    auto icon = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("Renasound").getChildFile ("RenasoundDocument.ico");
    if (! icon.existsAsFile() || icon.getSize() != (juce::int64) BinaryData::RenasoundDocument_icoSize)
    {
        icon.getParentDirectory().createDirectory();
        icon.replaceWithData (BinaryData::RenasoundDocument_ico, (size_t) BinaryData::RenasoundDocument_icoSize);
    }

    const juce::String root = "HKEY_CURRENT_USER\\Software\\Classes\\";
    const auto command = exe.getFullPathName().quoted() + " \"%1\"";
    const auto iconValue = icon.getFullPathName();
    if (Reg::getValue (root + "RENASOUND.Project\\shell\\open\\command\\") == command
        && Reg::getValue (root + "RENASOUND.Project\\DefaultIcon\\") == iconValue
        && Reg::getValue (root + ".renasound\\") == "RENASOUND.Project")
        return;   // already set up for this copy of the app

    Reg::setValue (root + ".renasound\\", "RENASOUND.Project");
    Reg::setValue (root + "RENASOUND.Project\\", "RENASOUND Project");
    Reg::setValue (root + "RENASOUND.Project\\DefaultIcon\\", iconValue);
    Reg::setValue (root + "RENASOUND.Project\\shell\\open\\command\\", command);
    SHChangeNotify (SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);   // refresh icons in Explorer
   #endif
}

class RenasoundApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "RENASOUND"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        mainWindow = std::make_unique<MainWindow>();
        registerProjectFiles();

        // opening a .renasound file from Finder / Explorer
        auto path = commandLine.unquoted().trim();
        if ((path.endsWithIgnoreCase (".renasound") || path.endsWithIgnoreCase (".als") || path.fromLastOccurrenceOf (".", false, false).startsWithIgnoreCase ("mx")) && juce::File::isAbsolutePath (path))
            mainWindow->content->loadProjectFile (juce::File (path));
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        auto path = commandLine.unquoted().trim();
        if (mainWindow != nullptr && (path.endsWithIgnoreCase (".renasound") || path.endsWithIgnoreCase (".als") || path.fromLastOccurrenceOf (".", false, false).startsWithIgnoreCase ("mx")) && juce::File::isAbsolutePath (path))
            mainWindow->content->askToSaveThen ([this, path] { mainWindow->content->loadProjectFile (juce::File (path)); });
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            mainWindow->content->askToSaveThen ([] { juce::JUCEApplication::getInstance()->quit(); });
        else
            quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow()
            : DocumentWindow ("RENASOUND", juce::Colour (0xff15161b), DocumentWindow::allButtons)
        {
            content = new rs::MainComponent();
            content->commandManager.registerAllCommandsForTarget (juce::JUCEApplication::getInstance());

            setUsingNativeTitleBar (true);
            setContentOwned (content, true);
            content->onTitleChanged = [this] (const juce::String& t) { if (getName() != t) setName (t); };

           #if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu (content);
           #else
            setMenuBar (content);
           #endif

            addKeyListener (content->commandManager.getKeyMappings());
            setResizable (true, false);
            setResizeLimits (960, 620, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            content->grabKeyboardFocus();
        }

        ~MainWindow() override
        {
           #if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu (nullptr);
           #else
            setMenuBar (nullptr);
           #endif
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        rs::MainComponent* content = nullptr;   // owned by the window
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (RenasoundApplication)
