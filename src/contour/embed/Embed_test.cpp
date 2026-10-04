// SPDX-License-Identifier: Apache-2.0
#include <contour/embed/Embed.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/FakeDisplaySurface.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <catch2/catch_test_macros.hpp>

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
