// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/config/Actions.hpp>
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
#include <QtCore/QThread>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

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

/// How many key, character and mouse bindings the configuration @p session runs on carries.
int inputMappingCount(contour::session::TerminalSession const& session)
{
    auto const& mappings = session.config().inputMappings.value();
    return static_cast<int>(mappings.keyMappings.size() + mappings.charMappings.size()
                            + mappings.mouseMappings.size());
}

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
    return Loaded { .inputMappings = inputMappingCount(*session),
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

TEST_CASE("embed: a terminal fed from another thread shows it, and answers on the GUI thread",
          "[contour][embed][threading]")
{
    Terminal terminal { runtime(), QSize { 80, 24 } };
    auto const* const gui = QThread::currentThread();
    // Counted where the signal is emitted, which a direct connection runs in: on the GUI thread or not.
    auto onGuiThread = std::atomic<int> { 0 };
    auto elsewhere = std::atomic<int> { 0 };
    QObject::connect(
        &terminal,
        &Terminal::input,
        &terminal,
        [&] { ++(QThread::currentThread() == gui ? onGuiThread : elsewhere); },
        Qt::DirectConnection);

    // One byte per call, so the order of the calls is the order of the text, and then a question
    // every terminal answers. The GUI thread meanwhile does what a host's does: runs its event loop.
    static constexpr auto Text = std::string_view { "fed from another thread, one byte at a time" };
    auto feeder = std::jthread { [&terminal] {
        terminal.feed("\033[2J\033[H");
        for (auto const& byte: Text)
            terminal.feed(QByteArrayView { &byte, 1 });
        terminal.feed("\033[c");
    } };

    CHECK(eventually(
        [&] { return terminal.mainPageText().contains(QString::fromUtf8(Text.data(), Text.size())); }));
    REQUIRE(eventually([&] { return onGuiThread.load() + elsewhere.load() > 0; }));
    feeder.join();
    CHECK(onGuiThread.load() > 0);
    CHECK(elsewhere.load() == 0);
}

TEST_CASE("embed: the page is the host's, and a display attaching does not resize it", "[contour][embed]")
{
    // A surface whose pixels would fit a different page. Attaching it reports the terminal's pixel
    // size to the device, which used to reach the host as a request for the page it already had.
    contour::test::FakeDisplaySurface surface;
    surface.pixelSizeValue = { vtbackend::Width(400), vtbackend::Height(160) };
    Terminal terminal { runtime(), QSize { 80, 24 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    QSignalSpy const requested { &terminal, &Terminal::pageSizeRequested };

    surface.attachedSession = session;
    session->attachDisplay(surface);
    QCoreApplication::processEvents();
    CHECK(terminal.pageSize() == QSize { 80, 24 });
    CHECK(requested.isEmpty());

    // What the resize path says would fit is a request, and resizes nothing.
    session->announcePageFit({ vtbackend::LineCount(10), vtbackend::ColumnCount(50) });
    REQUIRE(requested.size() == 1);
    CHECK(requested.front().front().toSize() == QSize { 50, 10 });
    CHECK(terminal.pageSize() == QSize { 80, 24 });

    // The same fit again is not a second request.
    session->announcePageFit({ vtbackend::LineCount(10), vtbackend::ColumnCount(50) });
    CHECK(requested.size() == 1);
}

TEST_CASE("embed: setPageSize resizes the page and refits the display", "[contour][embed]")
{
    contour::test::FakeDisplaySurface surface;
    Terminal terminal { runtime(), QSize { 80, 24 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    surface.attachedSession = session;
    session->attachDisplay(surface);
    auto const refitsBefore = surface.terminalResizeRequests;

    terminal.setPageSize(QSize { 100, 30 });
    CHECK(terminal.pageSize() == QSize { 100, 30 });
    // The renderer holds the old page until the display's resize path runs again.
    REQUIRE(eventually([&] { return surface.terminalResizeRequests > refitsBefore; }));

    // A page of the new width holds a line the old one would have wrapped.
    terminal.feed(QByteArray(100, 'w'));
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QString(100, QLatin1Char('w'))); }));
}

TEST_CASE("embed: a cell says what it holds and how it is drawn", "[contour][embed]")
{
    using contour::embed::CellAttribute;
    using contour::embed::ColorKind;
    Terminal terminal { runtime(), QSize { 20, 4 } };
    // Bold red on indexed 20, a direct colour with a curly underline, a combining mark, a wide
    // character, and a hidden cell; then a marker to wait on.
    terminal.feed("\033[1;31;48;5;20mA\033[0m"
                  "\033[38;2;1;2;3;4:3mB\033[0m"
                  "e\xCC\x81"
                  "\xE4\xB8\x96"
                  "\033[8;7mH\033[0m"
                  "\r\ndone");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("done")); }));

    auto const a = terminal.cell(0, 0);
    CHECK(a.text == QStringLiteral("A"));
    CHECK(a.width == 1);
    CHECK(a.attributes == contour::embed::CellAttributes { CellAttribute::Bold });
    CHECK(a.foreground.kind == ColorKind::Indexed);
    CHECK(a.foreground.index == 1);
    CHECK(a.background.kind == ColorKind::Indexed);
    CHECK(a.background.index == 20);
    // Entry 20 of the 256-colour cube is the same in every palette: (0, 0, 215).
    CHECK(a.background.rgb == qRgb(0, 0, 215));

    auto const b = terminal.cell(0, 1);
    CHECK(b.text == QStringLiteral("B"));
    CHECK(b.attributes == contour::embed::CellAttributes { CellAttribute::CurlyUnderline });
    CHECK(b.foreground.kind == ColorKind::Rgb);
    CHECK(b.foreground.rgb == qRgb(1, 2, 3));
    CHECK(b.background.kind == ColorKind::Default);

    CHECK(terminal.cell(0, 2).text == QString::fromUtf8("e\xCC\x81"));

    auto const wide = terminal.cell(0, 3);
    CHECK(wide.text == QString::fromUtf8("\xE4\xB8\x96"));
    CHECK(wide.width == 2);
    auto const covered = terminal.cell(0, 4);
    CHECK(covered.text.isEmpty());
    CHECK(covered.width == 0);

    auto const hidden = terminal.cell(0, 5);
    CHECK(hidden.text == QStringLiteral("H"));
    CHECK(hidden.attributes
          == (contour::embed::CellAttributes { CellAttribute::Hidden } | CellAttribute::Inverse));

    // A cell nothing was written to, and a place the page does not have.
    CHECK(terminal.cell(0, 6).text.isEmpty());
    CHECK(terminal.cell(0, 6).width == 1);
    CHECK(terminal.cell(4, 0).text.isEmpty());
    CHECK(terminal.cell(0, 20).text.isEmpty());
    CHECK(terminal.cell(-1, 0).text.isEmpty());

    auto const cursor = terminal.cursor();
    CHECK(cursor.line == 1);
    CHECK(cursor.column == 4);
    CHECK(cursor.visible);
}

TEST_CASE("embed: the cursor, the screen that is up and the scrollback can be asked for", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 20, 3 } };
    terminal.feed("one\r\ntwo\r\nthree\r\nfour\033[?25l");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("four")); }));
    CHECK(terminal.historyLineCount() == 1);
    CHECK(terminal.cell(-1, 0).text == QStringLiteral("o"));
    CHECK(terminal.cell(0, 0).text == QStringLiteral("t"));
    CHECK_FALSE(terminal.cursor().visible);
    CHECK_FALSE(terminal.alternateScreen());

    // The alternate screen is the one that is up once it is asked for, and keeps no history.
    terminal.feed("\033[?1049h\033[Halt\033[5n");
    REQUIRE(eventually([&] { return terminal.alternateScreen() && terminal.cell(0, 2).text == u"t"; }));
    CHECK(terminal.cell(0, 0).text == QStringLiteral("a"));
    CHECK(terminal.historyLineCount() == 0);
    CHECK(terminal.cell(-1, 0).text.isEmpty());
}

TEST_CASE("embed: a host selects, reads what it selected, and nothing is copied for it", "[contour][embed]")
{
    QGuiApplication::clipboard()->setText(QStringLiteral("the host's"));
    Terminal terminal { runtime(), QSize { 10, 3 } };
    terminal.feed("zero\r\nalpha beta\r\ngamma\r\ndone");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("done")); }));
    CHECK_FALSE(terminal.hasSelection());
    CHECK(terminal.selectionText().isEmpty());

    // Across a line end, and given backwards: the text between is what was crossed.
    terminal.select(QPoint { 2, 1 }, QPoint { 6, 0 });
    CHECK(terminal.hasSelection());
    CHECK(terminal.selectionText() == QStringLiteral("beta\ngam"));

    // Into the scrollback, which a negative line names.
    terminal.select(QPoint { 0, -1 }, QPoint { 4, 0 });
    CHECK(terminal.selectionText() == QStringLiteral("zero\nalpha"));

    terminal.selectAll();
    CHECK(terminal.selectionText().startsWith(QStringLiteral("zero\nalpha beta\ngamma\ndone")));

    terminal.clearSelection();
    CHECK_FALSE(terminal.hasSelection());
    CHECK(terminal.selectionText().isEmpty());

    QCoreApplication::processEvents();
    CHECK(QGuiApplication::clipboard()->text() == QStringLiteral("the host's"));
}

TEST_CASE("embed: a host scrolls the view, and is told when it moved", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 20, 4 } };
    QSignalSpy moved { &terminal, &Terminal::scrollChanged };
    // Twelve lines on a page of four: eight have left the top.
    for (auto const line: std::views::iota(1, 13))
        terminal.feed(QByteArray("line ") + QByteArray::number(line) + (line == 12 ? "" : "\r\n"));
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("line 12")); }));
    REQUIRE(terminal.historyLineCount() == 8);
    CHECK(terminal.scrollOffset() == 0);

    moved.clear();
    terminal.setScrollOffset(3);
    CHECK(terminal.scrollOffset() == 3);
    REQUIRE(eventually([&] { return moved.size() == 1; }));

    // Past either end is the end.
    terminal.setScrollOffset(500);
    CHECK(terminal.scrollOffset() == 8);
    terminal.setScrollOffset(-2);
    CHECK(terminal.scrollOffset() == 0);
}

TEST_CASE("embed: a host searches, steps between the matches and ends the search", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 20, 4 } };
    // Three lines hold the word, one of them in capitals and one of them above the page.
    terminal.feed("needle one\r\nhay\r\nhay\r\nNEEDLE two\r\nhay\r\nneedle three\r\nlast");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("last")); }));
    CHECK(terminal.searchState().pattern.isEmpty());

    QSignalSpy changed { &terminal, &Terminal::searchChanged };
    terminal.setSearchPattern(QStringLiteral("needle"));
    CHECK(changed.isEmpty()); // told from the event loop, not from inside the call
    CHECK(eventually([&] { return !changed.isEmpty(); }));
    auto state = terminal.searchState();
    CHECK(state.pattern == QStringLiteral("needle"));
    CHECK(state.matches == 3);
    CHECK_FALSE(state.capped);
    // The nearest match above the cursor is the newest of the three.
    CHECK(state.current == 3);

    terminal.searchPrevious();
    CHECK(terminal.searchState().current == 2);
    terminal.searchPrevious();
    CHECK(terminal.searchState().current == 1);
    // The first match is the first line there is, so standing on it scrolled the view all the way.
    CHECK(terminal.scrollOffset() == terminal.historyLineCount());
    terminal.searchNext();
    CHECK(terminal.searchState().current == 2);
    // Stepping wraps: before the first is the last.
    terminal.searchPrevious();
    terminal.searchPrevious();
    CHECK(terminal.searchState().current == 3);

    // A capital in the pattern makes the case count.
    terminal.setSearchPattern(QStringLiteral("NEEDLE"));
    CHECK(terminal.searchState().matches == 1);

    terminal.setSearchPattern(QStringLiteral("absent"));
    state = terminal.searchState();
    CHECK(state.matches == 0);
    CHECK(state.current == 0);

    terminal.setSearchPattern({});
    CHECK(terminal.searchState().pattern.isEmpty());
    // With nothing searched for there is nothing to step between.
    terminal.searchNext();
    CHECK(terminal.searchState().matches == 0);
}

TEST_CASE("embed: a host scrolls from mark to mark", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 20, 4 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    // Twelve lines on a page of four, with a mark set on the third and on the seventh.
    for (auto const line: std::views::iota(1, 13))
    {
        if (line == 3 || line == 7)
            terminal.feed("\033]133;A\033\\");
        terminal.feed(QByteArray("line ") + QByteArray::number(line) + (line == 12 ? "" : "\r\n"));
    }
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("line 12")); }));
    CHECK(session->scrollOffset() == 0);

    // Lines 9 to 12 are shown, so eight lines are above. The seventh is two lines up.
    REQUIRE(terminal.scrollToPreviousMark());
    CHECK(session->scrollOffset() == 2);
    REQUIRE(terminal.scrollToPreviousMark());
    CHECK(session->scrollOffset() == 6);
    CHECK_FALSE(terminal.scrollToPreviousMark());
    CHECK(session->scrollOffset() == 6);

    REQUIRE(terminal.scrollToNextMark());
    CHECK(session->scrollOffset() == 2);
    // No mark below: the bottom.
    REQUIRE(terminal.scrollToNextMark());
    CHECK(session->scrollOffset() == 0);
}

TEST_CASE("embed: a host sets the font size, and the page is still its own", "[contour][embed]")
{
    contour::test::FakeDisplaySurface surface;
    Terminal terminal { runtime(), QSize { 80, 24 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);

    // With no display there is nothing to load the size into; it is kept for the one that comes.
    terminal.setFontSize(15.0);
    CHECK(terminal.fontSize() == 15.0);

    surface.attachedSession = session;
    session->attachDisplay(surface);
    terminal.setFontSize(9.0);
    REQUIRE_FALSE(surface.fontSizesRequested.empty());
    CHECK(surface.fontSizesRequested.back().pt == 9.0);
    CHECK(terminal.fontSize() == 9.0);

    // A size the display could not load is not recorded as the size in force.
    surface.fontSizeApplies = false;
    terminal.setFontSize(400.0);
    CHECK(terminal.fontSize() == 9.0);

    CHECK(terminal.pageSize() == QSize { 80, 24 });
}

TEST_CASE("embed: a host chooses which of a profile's two palettes is drawn with", "[contour][embed]")
{
    QTemporaryDir const directory;
    REQUIRE(directory.isValid());
    Runtime host { RuntimeOptions { .configFile = writeConfig(directory,
                                                              "default_profile: host\n"
                                                              "color_schemes:\n"
                                                              "    night:\n"
                                                              "        default:\n"
                                                              "            background: '#010203'\n"
                                                              "            foreground: '#a0a1a2'\n"
                                                              "    day:\n"
                                                              "        default:\n"
                                                              "            background: '#f0f1f2'\n"
                                                              "            foreground: '#101112'\n"
                                                              "profiles:\n"
                                                              "    host:\n"
                                                              "        colors:\n"
                                                              "            dark: night\n"
                                                              "            light: day\n"
                                                              "input_mapping: []\n") } };
    Terminal terminal { host, QSize { 20, 2 } };
    terminal.feed("x");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("x")); }));
    CHECK(terminal.cell(0, 0).background.rgb == qRgb(0x01, 0x02, 0x03));
    CHECK(terminal.cell(0, 0).foreground.rgb == qRgb(0xa0, 0xa1, 0xa2));

    terminal.setColorScheme(Qt::ColorScheme::Light);
    CHECK(terminal.cell(0, 0).background.rgb == qRgb(0xf0, 0xf1, 0xf2));
    CHECK(terminal.cell(0, 0).foreground.rgb == qRgb(0x10, 0x11, 0x12));

    terminal.setColorScheme(Qt::ColorScheme::Dark);
    CHECK(terminal.cell(0, 0).background.rgb == qRgb(0x01, 0x02, 0x03));
}

TEST_CASE("embed: a host's name for the terminal is not the child's to change", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 20, 2 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);

    // Unnamed, a display is called what the window is, and the child decides that.
    terminal.feed("\033]2;from the child\033\\one");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("one")); }));
    CHECK(session->accessibleName().contains(QStringLiteral("from the child")));

    terminal.setAccessibleName(QStringLiteral("Console for build"));
    CHECK(session->accessibleName() == QStringLiteral("Console for build"));
    terminal.feed("\033]2;renamed by the child\033\\two");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("two")); }));
    CHECK(session->accessibleName() == QStringLiteral("Console for build"));

    terminal.setAccessibleName(QString {});
    CHECK(session->accessibleName().contains(QStringLiteral("renamed by the child")));
}

TEST_CASE("embed: a child cannot play a sound", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 20, 2 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    // DECPS: volume 7, an eighth of a second, one note. Then a marker to wait on.
    terminal.feed("\033[7;4;10,~played");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("played")); }));
    CHECK_FALSE(session->hasAudioOutput());
}

TEST_CASE("embed: a child cannot resize the window, nor the page through it", "[contour][embed]")
{
    contour::test::FakeDisplaySurface surface;
    Terminal terminal { runtime(), QSize { 80, 24 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    surface.attachedSession = session;
    session->attachDisplay(surface);

    // CSI 8 ; lines ; columns t asks for a page, CSI 4 ; height ; width t for pixels.
    terminal.feed("\033[8;10;40t\033[4;300;400tasked");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("asked")); }));
    QCoreApplication::processEvents();
    CHECK(surface.pageResizeRequests.empty());
    CHECK(surface.pixelResizeRequests.empty());
    CHECK(terminal.pageSize() == QSize { 80, 24 });
}

TEST_CASE("images.display: off, an image is read to its end and not drawn", "[contour][embed][images]")
{
    // A sixel of one red band, then a marker to wait on.
    auto constexpr Sixel = "\033Pq#0;2;100;0;0#0~~~~~~~~~~~~-~~~~~~~~~~~~\033\\\r\nafter";
    auto const firstCellHoldsAnImage = [&](QByteArrayView images) {
        QTemporaryDir const directory;
        REQUIRE(directory.isValid());
        Runtime host { RuntimeOptions {
            .configFile = writeConfig(directory,
                                      QByteArray("default_profile: host\nprofiles:\n    host: {}\n") + images
                                          + "input_mapping: []\n") } };
        Terminal terminal { host, QSize { 20, 6 } };
        auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
        REQUIRE(session != nullptr);
        {
            // No display is attached, so the terminal has not been told how large a cell is, and an
            // image cannot be laid over cells of no size.
            auto const lock = std::scoped_lock { session->terminal() };
            session->terminal().setCellPixelSize(
                vtbackend::ImageSize { vtbackend::Width(8), vtbackend::Height(16) });
        }
        terminal.feed(Sixel);
        REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("after")); }));
        auto const lock = std::scoped_lock { session->terminal() };
        return session->terminal()
                   .primaryScreen()
                   .at(vtbackend::LineOffset(0), vtbackend::ColumnOffset(0))
                   .imageFragment()
               != nullptr;
    };

    CHECK(firstCellHoldsAnImage(""));
    CHECK_FALSE(firstCellHoldsAnImage("images:\n    display: false\n"));
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

TEST_CASE("embed: the built-in profile reserves no fold gutter", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 80, 24 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    // The gutter is a column of the item left of the grid that answers a click by folding output.
    // A host that lays its own chrome around the item has a page of exactly the columns it asked
    // for, and a pointer over the first of them is over the first cell.
    CHECK_FALSE(session->terminal().settings().foldMarkers);
}

TEST_CASE("embed: a hyperlink is offered, not opened", "[contour][embed]")
{
    // Through the launcher a session reaches for when a hyperlink is clicked, which is the one the
    // Runtime put there.
    Terminal const terminal { runtime(), QSize { 80, 24 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    QSignalSpy offered { &runtime(), &Runtime::openUrlRequested };
    CHECK(session->app()
              .externalLauncher()
              .openUrl(QUrl { QStringLiteral("https://example.org/") })
              .has_value());
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

TEST_CASE("embed: a bell raises no window alert", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 20, 2 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    QSignalSpy const alerts { session, &contour::session::TerminalSession::onAlert };
    QSignalSpy const bells { session, &contour::session::TerminalSession::onBell };
    terminal.feed("\arung");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("rung")); }));
    QCoreApplication::processEvents();
    // The control: the bell was heard by the session, so an alert had its chance.
    CHECK(bells.size() == 1);
    CHECK(alerts.isEmpty());
}

TEST_CASE("embed: a child cannot take the mouse or the page width", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 80, 24 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    // Every mouse protocol, then 132 columns with the mode that allows it; ?2004 is the control,
    // a mode nothing freezes.
    terminal.feed("\033[?9h\033[?1000h\033[?1001h\033[?1002h\033[?1003h\033[?2029h"
                  "\033[?40h\033[?3h\033[?2004h"
                  "asked");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("asked")); }));

    auto const lock = std::scoped_lock { session->terminal() };
    using vtbackend::DECMode;
    CHECK(session->terminal().isModeEnabled(DECMode::BracketedPaste));
    for (auto const mode: { DECMode::MouseProtocolX10,
                            DECMode::MouseProtocolNormalTracking,
                            DECMode::MouseProtocolHighlightTracking,
                            DECMode::MouseProtocolButtonTracking,
                            DECMode::MouseProtocolAnyEventTracking,
                            DECMode::MousePassiveTracking,
                            DECMode::Columns132 })
    {
        CAPTURE(mode);
        CHECK_FALSE(session->terminal().isModeEnabled(mode));
    }
    CHECK(session->terminal().pageSize().columns == vtbackend::ColumnCount(80));
}

TEST_CASE("embed: nothing is written beside a host's configuration file, and nothing is spoken",
          "[contour][embed]")
{
    QTemporaryDir const directory;
    REQUIRE(directory.isValid());
    Runtime host { RuntimeOptions { .configFile = writeConfig(directory,
                                                              "default_profile: host\n"
                                                              "profiles:\n"
                                                              "    host: {}\n"
                                                              "input_mapping: []\n") } };
    Terminal const terminal { host, QSize { 20, 2 } };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);

    // What the command palette does when a command is run from it, and what SaveLayout does.
    session->app().sessionsManager().recordCommand("ScrollPageUp");
    (void) (*session)(contour::actions::SaveLayout { .name = "kept" });
    QCoreApplication::processEvents();

    auto const beside = std::filesystem::path { directory.path().toStdString() };
    CHECK_FALSE(std::filesystem::exists(beside / "command-history.yml"));
    CHECK_FALSE(std::filesystem::exists(beside / "layouts.yml"));
    auto entries = 0;
    for ([[maybe_unused]] auto const& entry: std::filesystem::directory_iterator { beside })
        ++entries;
    CHECK(entries == 1); // contour.yml, as written above

    CHECK_FALSE(session->app().speechSynthesizer().available());
}

TEST_CASE("embed: a host told the view moved may ask the terminal anything", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 20, 4 } };
    terminal.feed("needle\r\n");
    for (auto const line: std::views::iota(1, 13))
        terminal.feed(QByteArray("line ") + QByteArray::number(line) + (line == 12 ? "" : "\r\n"));
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("line 12")); }));

    // A host draws its scrollbar and its search field from what it is told, by asking. Told from
    // inside the call that moved the view, it would be asking a terminal that is still busy with
    // that call -- so it is told afterwards, from the event loop.
    auto inside = false;
    auto toldInside = 0;
    auto told = 0;
    auto read = -1;
    auto const ask = [&] {
        if (inside)
        {
            ++toldInside;
            return;
        }
        ++told;
        read = terminal.historyLineCount() + terminal.scrollOffset() + terminal.searchState().matches;
    };
    QObject::connect(&terminal, &Terminal::scrollChanged, &terminal, ask);
    QObject::connect(&terminal, &Terminal::searchChanged, &terminal, ask);

    inside = true;
    terminal.setSearchPattern(QStringLiteral("needle")); // scrolls to the match, far above the page
    terminal.setScrollOffset(2);
    inside = false;

    REQUIRE(eventually([&] { return told >= 2; }));
    CHECK(toldInside == 0);
    CHECK(read >= 0);
    terminal.setSearchPattern({});
}

TEST_CASE("embed: a terminal destroyed with the focus does not keep it", "[contour][embed]")
{
    auto first = std::make_unique<Terminal>(runtime(), QSize { 20, 2 });
    Terminal const second { runtime(), QSize { 20, 2 } };
    auto* const firstSession = dynamic_cast<contour::session::TerminalSession*>(first->session());
    auto* const secondSession = dynamic_cast<contour::session::TerminalSession*>(second.session());
    REQUIRE(firstSession != nullptr);
    REQUIRE(secondSession != nullptr);
    auto& manager = firstSession->app().sessionsManager();

    // What a display does when it is given the keyboard.
    manager.setFocusedSession(firstSession);
    REQUIRE(manager.focusedSession() == firstSession);

    // A host closes the terminal that has the focus. The next one to be focused is told so after
    // the last one is told it lost it -- which must not be a session that no longer exists.
    first.reset();
    CHECK(manager.focusedSession() == nullptr);
    manager.setFocusedSession(secondSession);
    CHECK(manager.focusedSession() == secondSession);
    manager.setFocusedSession(nullptr);
}

TEST_CASE("embed: a Runtime takes no signal of its host's and makes no crash directory", "[contour][embed]")
{
    QTemporaryDir const state;
    REQUIRE(state.isValid());
    auto const stateBefore = qgetenv("XDG_STATE_HOME");
    auto const stateWasSet = qEnvironmentVariableIsSet("XDG_STATE_HOME");
    qputenv("XDG_STATE_HOME", state.path().toUtf8());

    // The host's own handler, whatever it does with an abort: a crash reporter, a test framework's.
    auto const hosts = +[](int) {
    };
    auto const before = std::signal(SIGABRT, hosts);
    {
        Runtime host { RuntimeOptions {} };
        Terminal const terminal { host, QSize { 20, 2 } };
        QCoreApplication::processEvents();
    }
    auto const after = std::signal(SIGABRT, before);

    if (stateWasSet)
        qputenv("XDG_STATE_HOME", stateBefore);
    else
        qunsetenv("XDG_STATE_HOME");

    CHECK(after == hosts);
    auto entries = 0;
    for ([[maybe_unused]] auto const& entry:
         std::filesystem::directory_iterator { std::filesystem::path { state.path().toStdString() } })
        ++entries;
    CHECK(entries == 0);
}

TEST_CASE("embed: a binding cannot reload or reset the configuration", "[contour][embed]")
{
    // ReloadConfig reads the file again, and with it the profiles a Runtime removes; ResetConfig
    // overwrites the file. A binding to either is dropped, alone or beside another action, and a
    // binding that is neither is kept.
    QTemporaryDir const directory;
    REQUIRE(directory.isValid());
    auto const file = writeConfig(directory,
                                  "default_profile: host\n"
                                  "profiles:\n"
                                  "    host: {}\n"
                                  "input_mapping:\n"
                                  "    - { mods: [Control], key: F5, action: ReloadConfig }\n"
                                  "    - { mods: [Control], key: F6, action: ResetConfig }\n"
                                  "    - { mods: [Control], key: F7, action: ScrollPageUp }\n");
    Runtime host { RuntimeOptions { .configFile = file } };
    Terminal const terminal { host, QSize { 20, 2 } };
    auto const* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);

    auto const& keys = session->config().inputMappings.value().keyMappings;
    REQUIRE(keys.size() == 1);
    REQUIRE(keys.front().binding.size() == 1);
    CHECK(std::holds_alternative<contour::actions::ScrollPageUp>(keys.front().binding.front()));
    CHECK(session->config().profiles.value().size() == 1);
}

TEST_CASE("embed: no default key binding survives", "[contour][embed]")
{
    Terminal const terminal { runtime(), QSize { 80, 24 } };
    auto const* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    CHECK(inputMappingCount(*session) == 0);
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

TEST_CASE("embed: a host's configuration file with an entry of the wrong type leaves the built-in standing",
          "[contour][embed]")
{
    // The loader stops at the entry it cannot read. The profile and the key bindings come after it,
    // so without the refusal this file -- which binds no keys and names its own profile -- would
    // run on Contour's default profile's settings for everything unread and Contour's keymap.
    QTemporaryDir const directory;
    REQUIRE(directory.isValid());
    auto const loaded = loadedFrom(writeConfig(directory,
                                               "default_profile: host\n"
                                               "read_buffer_size: [not, a, number]\n"
                                               "profiles:\n"
                                               "    host:\n"
                                               "        permissions:\n"
                                               "            write_clipboard: allow\n"
                                               "input_mapping: []\n"));
    CHECK(loaded.inputMappings == 0);
    CHECK(loaded.profile == "embed");
    CHECK(loaded.writeClipboard == contour::config::Permission::Deny);
    CHECK(loaded.reported.contains("could not be read"));
    CHECK(loaded.reported.contains("built-in embed profile"));
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

TEST_CASE("embed: a terminal whose replies are withheld writes only what the user does", "[contour][embed]")
{
    Terminal terminal { runtime(), QSize { 80, 24 }, contour::embed::Replies::Withheld };
    auto* const session = dynamic_cast<contour::session::TerminalSession*>(terminal.session());
    REQUIRE(session != nullptr);
    QSignalSpy written { &terminal, &Terminal::input };

    // Where the cursor is, what the terminal is and what its background holds, then text: once the
    // text is on the page the questions before it have been read.
    terminal.feed("\033[6n\033[c\033]11;?\033\\asked");
    REQUIRE(eventually([&] { return terminal.mainPageText().contains(QStringLiteral("asked")); }));
    // A reply would be queued behind the page by now; this gives it the two seconds every other
    // test here gives one to arrive.
    CHECK_FALSE(eventually([&] { return !written.isEmpty(); }));

    // A paste is the user's, and leaves as it always did.
    {
        auto const lock = std::scoped_lock { session->terminal() };
        session->terminal().sendPaste("typed");
    }
    REQUIRE(eventually([&] { return !written.isEmpty(); }));
    CHECK(written.first().first().toByteArray() == "typed");
}
