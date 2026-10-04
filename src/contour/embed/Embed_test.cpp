// SPDX-License-Identifier: Apache-2.0
#include <contour/embed/Embed.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/FakeDisplaySurface.hpp>

#include <core/log/LogSink.hpp>
#include <core/log/LogStore.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>
#include <string>

#include <QtTest/QSignalSpy>

using contour::embed::Runtime;
using contour::embed::RuntimeOptions;
using contour::embed::Terminal;

namespace
{

/// Spins the event loop until @p done holds or two seconds pass.
template <typename Predicate>
bool eventually(Predicate done)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

/// One Runtime for the whole binary: it is a process-wide composition root.
Runtime& runtime()
{
    static Runtime instance { RuntimeOptions {} };
    return instance;
}

/// What a host's configuration file left a session with.
struct Loaded
{
    int inputMappings;                          ///< Key and mouse bindings in force.
    std::string profile;                        ///< The profile a session starts in.
    contour::config::Permission writeClipboard; ///< That profile's answer to a child's clipboard write.
    std::string reported;                       ///< What the error log was told while loading.
};

/// Collects what is written to the error log while it is alive.
class ErrorLogCapture
{
  public:
    ErrorLogCapture(): _previous { core::log::errorLog.sink() } { core::log::errorLog.setSink(_sink); }
    ~ErrorLogCapture() { core::log::errorLog.setSink(_previous); }
    ErrorLogCapture(ErrorLogCapture const&) = delete;
    ErrorLogCapture& operator=(ErrorLogCapture const&) = delete;
    ErrorLogCapture(ErrorLogCapture&&) = delete;
    ErrorLogCapture& operator=(ErrorLogCapture&&) = delete;

    /// @return Everything written so far.
    [[nodiscard]] std::string text() const { return _text.str(); }

  private:
    std::ostringstream _text;
    core::log::Sink _sink { true, _text };
    core::log::Sink& _previous;
};

/// Builds a Runtime of its own from @p configFile and reports what a terminal on it starts with.
///
/// A host has one Runtime; these exist only long enough to be asked what they loaded.
Loaded loadedFrom(std::filesystem::path const& configFile)
{
    ErrorLogCapture const errors;
    Runtime host { RuntimeOptions { .configFile = configFile } };
    Terminal const terminal { host, QSize { 80, 24 } };
    auto const* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    return Loaded { .inputMappings = host.inputMappingCountForTesting(),
                    .profile = session->profileName(),
                    .writeClipboard = session->profile().permissions.value().writeClipboard,
                    .reported = errors.text() };
}

/// Writes @p text as `contour.yml` in @p directory.
std::filesystem::path writeConfig(QTemporaryDir const& directory, QByteArrayView text)
{
    QFile file { directory.filePath(QStringLiteral("contour.yml")) };
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(text.data(), text.size());
    file.close();
    return file.fileName().toStdString();
}

} // namespace

TEST_CASE("embed: fed bytes are on the page", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 80, 24 } };
    terminal.feed("\033[2J\033[Hhello from the host");
    CHECK(
        eventually([&] { return terminal.mainPageText().contains(QStringLiteral("hello from the host")); }));
}

TEST_CASE("embed: what the terminal writes reaches the host", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 80, 24 } };
    QSignalSpy written { &terminal, &Terminal::input };
    // Primary device attributes: every terminal answers, and the answer starts CSI ?.
    terminal.feed("\033[c");
    REQUIRE(eventually([&] { return !written.isEmpty(); }));
    CHECK(written.first().first().toByteArray().startsWith("\033[?"));
}

TEST_CASE("embed: the built-in profile draws no status line", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 80, 24 } };
    terminal.feed("\033[2J\033[Hx");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QChar('x')); }));
    // The status line is a screen of its own and never part of the main page's text, so what it
    // shows cannot be looked for there. What is asserted is the state the terminal holds.
    // Not qobject_cast: TerminalSession is declared an interface (Q_DECLARE_INTERFACE), so that cast
    // asks for an interface id no class implements and answers null for a perfectly good session.
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    CHECK(session->terminal().statusDisplayType() == vtbackend::StatusDisplayType::None);
}

TEST_CASE("embed: a hyperlink is offered, not opened", "[contour][embed]")
{
    QSignalSpy offered { &runtime(), &Runtime::openUrlRequested };
    CHECK(runtime().offerUrlForTesting(QUrl { QStringLiteral("https://example.org/") }));
    REQUIRE(offered.size() == 1);
    CHECK(offered.first().first().toUrl() == QUrl { QStringLiteral("https://example.org/") });
}

TEST_CASE("embed: a child cannot write the clipboard", "[contour][embed]")
{
    // With a display attached, because that is the terminal a host shows -- and because a session
    // reaches the clipboard only through its display, so one without would pass whatever the profile
    // said. Declared before the terminal: the session lets go of its display as it is destroyed.
    contour::test::FakeDisplaySurface surface;
    QGuiApplication::clipboard()->setText(QStringLiteral("the host's"));
    Terminal terminal { runtime(), QSize { 80, 24 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    surface.attachedSession = session;
    session->attachDisplay(surface);
    // OSC 52: set the clipboard to base64("stolen"), then a marker to wait on.
    terminal.feed("\033]52;c;c3RvbGVu\033\\done");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("done")); }));
    CHECK(QGuiApplication::clipboard()->text() == QStringLiteral("the host's"));
}

TEST_CASE("embed: a child cannot switch to a profile the host did not choose", "[contour][embed]")
{
    // DCS $ p <name> ST asks the session for another of the configuration's profiles, and no
    // permission stands in its way. Contour's own default profile, "main", is what a child would ask
    // for: it allows the clipboard write the embed profile denies.
    contour::test::FakeDisplaySurface surface;
    // A display runs what a session posts on the GUI thread, so the posts are held and drained here.
    // Run where they are made they would be on the terminal's thread, inside the lock a profile
    // switch takes.
    surface.runPostsImmediately = false;
    QGuiApplication::clipboard()->setText(QStringLiteral("the host's"));
    Terminal terminal { runtime(), QSize { 80, 24 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    surface.attachedSession = session;
    session->attachDisplay(surface);
    surface.drainPosts();
    auto const chosen = session->profileName();

    terminal.feed("\033P$pmain\033\\switched");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("switched")); }));
    surface.drainPosts();
    CHECK(session->profileName() == chosen);
    CHECK(session->profile().permissions.value().writeClipboard == contour::config::Permission::Deny);

    terminal.feed("\033]52;c;c3RvbGVu\033\\done");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("done")); }));
    surface.drainPosts();
    CHECK(QGuiApplication::clipboard()->text() == QStringLiteral("the host's"));
}

TEST_CASE("embed: a child cannot raise a desktop notification", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 80, 24 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    // Asked of the session before anything is fed, and REQUIREd: with the platform's own notifier in
    // that seat the sequences below would be put on whatever desktop is running this test.
    REQUIRE(dynamic_cast<contour::platform::NullNotifier const*>(&session->desktopNotifier()) != nullptr);

    // OSC 777 and OSC 99, the two ways a child asks for one, then a marker to wait on. Neither needs
    // a display or a permission to reach the notifier, so the notifier is all there is to refuse them.
    terminal.feed("\033]777;notify;Build;done\033\\\033]99;i=1;Build done\033\\notified");
    CHECK(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("notified")); }));
}

TEST_CASE("embed: no default key binding survives", "[contour][embed]")
{
    CHECK(runtime().inputMappingCountForTesting() == 0);
}

TEST_CASE("embed: a host's configuration file that is missing is not created, and the built-in stands",
          "[contour][embed]")
{
    QTemporaryDir const directory;
    REQUIRE(directory.isValid());
    auto const missing = std::filesystem::path { directory.path().toStdString() } / "absent" / "contour.yml";

    auto const loaded = loadedFrom(missing);
    CHECK(loaded.inputMappings == 0);
    CHECK(loaded.profile == "embed");
    CHECK(loaded.writeClipboard == contour::config::Permission::Deny);
    CHECK(loaded.reported.contains(missing.string()));
    CHECK(loaded.reported.contains("built-in embed profile"));
    CHECK_FALSE(std::filesystem::exists(missing));
    CHECK_FALSE(std::filesystem::exists(missing.parent_path()));
}

TEST_CASE("embed: a host's configuration file that does not parse leaves the built-in standing",
          "[contour][embed]")
{
    QTemporaryDir const directory;
    REQUIRE(directory.isValid());

    SECTION("not YAML")
    {
        auto const loaded = loadedFrom(writeConfig(directory, "profiles: [unclosed\n\tinput_mapping: {"));
        CHECK(loaded.inputMappings == 0);
        CHECK(loaded.profile == "embed");
        CHECK(loaded.writeClipboard == contour::config::Permission::Deny);
    }

    SECTION("YAML, but not a mapping")
    {
        auto const loaded = loadedFrom(writeConfig(directory, "- one\n- two\n"));
        CHECK(loaded.inputMappings == 0);
        CHECK(loaded.profile == "embed");
        CHECK(loaded.writeClipboard == contour::config::Permission::Deny);
    }

    SECTION("names a default profile it does not define")
    {
        auto const loaded = loadedFrom(writeConfig(directory, "default_profile: nowhere\n"));
        CHECK(loaded.inputMappings == 0);
        CHECK(loaded.profile == "embed");
        CHECK(loaded.writeClipboard == contour::config::Permission::Deny);
    }
}

TEST_CASE("embed: a host's configuration file that parses is the host's to write", "[contour][embed]")
{
    // What such a file says is used as it stands, including a permission the built-in profile denies.
    QTemporaryDir const directory;
    REQUIRE(directory.isValid());
    auto const loaded = loadedFrom(writeConfig(directory,
                                               "default_profile: host\n"
                                               "profiles:\n"
                                               "    host:\n"
                                               "        permissions:\n"
                                               "            write_clipboard: allow\n"
                                               "input_mapping: []\n"));
    CHECK(loaded.inputMappings == 0);
    CHECK(loaded.profile == "host");
    CHECK(loaded.writeClipboard == contour::config::Permission::Allow);
    CHECK(loaded.reported.empty());
}
