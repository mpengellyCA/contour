// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/command/CommandHistoryStore.hpp>
#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/LayoutStore.hpp>
#include <contour/display/ShaderConfig.hpp>
#include <contour/display/TerminalAccessible.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/embed/BuiltinConfig.hpp>
#include <contour/embed/Embed.hpp>
#include <contour/platform/ExternalLauncher.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/platform/SpeechSynthesizer.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>

#include <vtbackend/input/vi/Selector.hpp>

#include <vtpty/ChannelPty.hpp>

#include <core/Assert.hpp>
#include <core/Environment.hpp>

#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QTemporaryDir>
#include <QtQml/qqml.h>

#include <array>
#include <chrono>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace contour::embed
{

namespace
{

    /// A launcher that announces and never launches.
    ///
    /// The bytes a terminal shows are a child's, so a link in them is an address the child chose. A
    /// host that embeds the terminal decides what opening one means; this library does not.
    class AnnouncingLauncher final: public platform::ExternalLauncher
    {
      public:
        /// @param runtime Whom to announce through.
        explicit AnnouncingLauncher(Runtime& runtime): _runtime { runtime } {}

        [[nodiscard]] std::expected<void, platform::LaunchError> openUrl(QUrl const& url) override
        {
            if (!platform::isOpenable(url))
                return std::unexpected(platform::LaunchError::InvalidUrl);
            emit _runtime.openUrlRequested(url);
            return {};
        }

        [[nodiscard]] std::expected<void, platform::SpawnError> runDetached(QString const&,
                                                                            QStringList const&) override
        {
            return std::unexpected(platform::SpawnError::StartFailed);
        }

        [[nodiscard]] std::expected<int, platform::SpawnError> execute(QString const&,
                                                                       QStringList const&) override
        {
            return std::unexpected(platform::SpawnError::StartFailed);
        }

      private:
        Runtime& _runtime;
    };

    /// A layout store that holds nothing and keeps nothing.
    ///
    /// The application's own store is a file beside the configuration file. A host's configuration
    /// lives in the host's directory, and the built-in one in a temporary directory that is gone
    /// by the time anything is saved; neither is a place for this library to write.
    class NoLayoutStore final: public config::LayoutStore
    {
      public:
        [[nodiscard]] std::expected<config::LayoutMap, std::string> load(
            std::filesystem::path const& /*path*/) const override
        {
            return config::LayoutMap {};
        }

        [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& /*path*/,
                                                            config::LayoutMap const& /*layouts*/) override
        {
            return std::unexpected(std::string { "an embedded terminal keeps no layouts" });
        }
    };

    /// A command history that holds nothing and keeps nothing, for NoLayoutStore's reason.
    class NoCommandHistoryStore final: public command::CommandHistoryStore
    {
      public:
        [[nodiscard]] std::expected<std::vector<std::string>, std::string> load(
            std::filesystem::path const& /*path*/) const override
        {
            return std::vector<std::string> {};
        }

        [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& /*path*/,
                                                            std::span<std::string const> /*ids*/) override
        {
            return std::unexpected(std::string { "an embedded terminal keeps no command history" });
        }
    };

    /// A ChannelPty that says when the terminal has finished with what it was last given.
    ///
    /// A session hands the terminal's replies to the device only through its display: the display is
    /// what posts the flush onto the GUI thread. A terminal with no display therefore keeps every
    /// reply queued, and a child that asked a question waits for good. The terminal's thread comes
    /// back to read() once it has parsed the previous chunk, which is exactly when a reply to that
    /// chunk can be pending, so that is where the host is told.
    class HostPty final: public vtpty::ChannelPty
    {
      public:
        /// Called on the terminal's thread, each time it returns to read.
        using Drained = std::function<void()>;

        /// @param pageSize The initial page size.
        /// @param drained  Whom to tell; fixed for the device's lifetime.
        HostPty(vtpty::PageSize pageSize, Drained drained):
            vtpty::ChannelPty { pageSize }, _drained { std::move(drained) }
        {
        }

        [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                     std::optional<std::chrono::milliseconds> timeout,
                                                     size_t size) override
        {
            _drained();
            return vtpty::ChannelPty::read(storage, timeout, size);
        }

      private:
        Drained _drained;
    };

    /// Loads the configuration a host gets when it supplies none, or none that can be used.
    /// @param config What to load it into.
    void loadBuiltinConfig(config::Config& config)
    {
        // A directory of its own, not a bare temporary file: loading a configuration also reads the
        // files beside it (layouts.yml, settings.yml, profiles/), and what happens to lie in the
        // system's temporary directory is nobody's configuration.
        QTemporaryDir const directory;
        Require(directory.isValid());
        QFile builtin { directory.filePath(QStringLiteral("contour.yml")) };
        Require(builtin.open(QIODevice::WriteOnly));
        builtin.write(BuiltinConfig.data(), static_cast<qint64>(BuiltinConfig.size()));
        builtin.close();
        config::loadConfigFromFile(config, builtin.fileName().toStdString());
    }

    /// Says whether a host's configuration file is one the loader can be given.
    ///
    /// The loader is forgiving in ways a host cannot afford: it writes Contour's default
    /// configuration to a path that does not exist, and it carries on with Contour's defaults over a
    /// document it could not parse. Either way the terminal would run on a configuration nobody
    /// chose, so both are found here, before the loader sees the file.
    /// @param file The host's file.
    /// @return Nothing, or why the file cannot be used.
    [[nodiscard]] std::expected<void, std::string> usableConfigFile(std::filesystem::path const& file)
    {
        auto ec = std::error_code {};
        if (!std::filesystem::is_regular_file(file, ec))
            return std::unexpected(std::string { "it is not a file" });
        try
        {
            auto stream = std::ifstream { file, std::ios::binary };
            if (!stream.good())
                return std::unexpected(std::string { "it cannot be read" });
            if (!YAML::Load(stream).IsMap())
                return std::unexpected(std::string { "it is not a YAML mapping" });
        }
        catch (std::exception const& e)
        {
            return std::unexpected(std::string { e.what() });
        }
        return {};
    }

    /// Loads a host's configuration file, refusing one that would leave Contour's defaults in force.
    /// @param config What to load it into.
    /// @param file   The host's file.
    /// @param app    The application, which says which profile a session would start in.
    /// @return Nothing, or why the file was not used; @p config is then in no usable state.
    [[nodiscard]] std::expected<void, std::string> loadHostConfig(config::Config& config,
                                                                  std::filesystem::path const& file,
                                                                  ContourGuiApp const& app)
    {
        return usableConfigFile(file).and_then([&]() -> std::expected<void, std::string> {
            try
            {
                config::loadConfigFromFile(config, file);
            }
            catch (std::exception const& e)
            {
                return std::unexpected(std::string { e.what() });
            }
            // The loader stops at the first top-level entry it cannot read and leaves it, and every
            // entry it had not reached yet, at Contour's default. The key bindings are read last, so
            // one mistyped scalar anywhere above them is Contour's whole default keymap.
            if (config.loadFailure)
                return std::unexpected(
                    std::format("an entry in it could not be read ({})", *config.loadFailure));
            if (config.findProfile(app.profileName()) == nullptr)
                return std::unexpected(std::format("it defines no profile named '{}'", app.profileName()));
            return {};
        });
    }

    /// Leaves @p config with one profile, and no way back to the others.
    ///
    /// A child can ask a session for any profile the configuration holds by name (DCS $ p), and no
    /// permission is consulted. Loading always leaves Contour's own default profile, "main", beside
    /// whatever the file defines, and "main" allows what the embed profile denies. So every profile
    /// but the chosen one is removed, and live reload is switched off: a reload reads the file again,
    /// and with it the profiles removed here.
    /// @param config The loaded configuration.
    /// @param chosen The name of the profile sessions start in.
    void keepOnlyProfile(config::Config& config, std::string const& chosen)
    {
        std::erase_if(config.profiles.value(), [&](auto const& profile) { return profile.first != chosen; });
        config.live.value() = false;
    }

    /// Takes the actions that read or rewrite the configuration file out of every binding.
    ///
    /// ReloadConfig loads the file afresh, and with it every profile keepOnlyProfile() removed --
    /// Contour's own "main" among them, back within reach of a child's DCS $ p. ResetConfig
    /// overwrites the host's file with Contour's defaults. A host's file may bind keys, and that is
    /// honoured; these two undo what this library promises about any configuration, so a binding
    /// to them is not. A binding left with nothing to do is removed.
    /// @param mappings The loaded bindings.
    void dropConfigurationActions(config::InputMappings& mappings)
    {
        auto const drop = [](auto& bindings) {
            for (auto& binding: bindings)
                std::erase_if(binding.binding, [](actions::Action const& action) {
                    return std::holds_alternative<actions::ReloadConfig>(action)
                           || std::holds_alternative<actions::ResetConfig>(action);
                });
            std::erase_if(bindings, [](auto const& binding) { return binding.binding.empty(); });
        };
        drop(mappings.keyMappings);
        drop(mappings.charMappings);
        drop(mappings.mouseMappings);
    }

} // namespace

struct Runtime::Impl
{
    RuntimeOptions options; ///< What the runtime was built from.
    core::LiveEnvironment environment;
    std::unique_ptr<ContourGuiApp> app;
};

Runtime::Runtime(RuntimeOptions options, QObject* parent):
    QObject { parent }, _impl { std::make_unique<Impl>() }
{
    _impl->options = std::move(options);

    auto launcher = std::make_unique<AnnouncingLauncher>(*this);
    // Stores that keep nothing and a synthesizer that cannot speak. Left to their defaults the
    // application writes layouts.yml and command-history.yml beside the configuration file and
    // opens the platform's speech service, none of which a host asked a terminal display to do.
    _impl->app = std::make_unique<ContourGuiApp>(_impl->environment,
                                                 nullptr,
                                                 std::move(launcher),
                                                 std::make_unique<NoLayoutStore>(),
                                                 std::make_unique<NoCommandHistoryStore>(),
                                                 std::make_unique<platform::NullSpeechSynthesizer>());

    // The terminal verb's parameters are what a session reads its defaults from. A host has no
    // command line of Contour's, so one is supplied.
    auto argv = std::array<char const*, 2> { "contour", "terminal" };
    Require(_impl->app->parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));

    auto& config = _impl->app->config();
    auto const& hostFile = _impl->options.configFile;
    if (hostFile.empty())
        loadBuiltinConfig(config);
    else if (auto const loaded = loadHostConfig(config, hostFile, *_impl->app); !loaded)
    {
        // Closed, not open: a host that named a file wanted something other than Contour's defaults,
        // and the built-in profile is the only other configuration there is. The constructor cannot
        // refuse, so it says so where a host that looks will find it.
        errorLog()("Not using the configuration file {}: {}. Using the built-in embed profile instead.",
                   hostFile.string(),
                   loaded.error());
        config = config::Config {};
        loadBuiltinConfig(config);
    }

    keepOnlyProfile(config, _impl->app->profileName());
    dropConfigurationActions(config.inputMappings.value());

    display::TerminalAccessible::installFactory();
    qmlRegisterType<display::TerminalDisplay>("Contour.Terminal", 1, 0, "ContourTerminal");
    qmlRegisterUncreatableType<session::TerminalSession>(
        "Contour.Terminal", 1, 0, "TerminalSession", "Made by the host through contour::embed::Terminal.");
}

Runtime::~Runtime() = default;

QSurfaceFormat Runtime::surfaceFormat()
{
    return display::createSurfaceFormat();
}

struct Terminal::Impl
{
    vtpty::ChannelPty* pty = nullptr; ///< Owned by the session's terminal.
    std::unique_ptr<session::TerminalSession> session;
    /// The same session, for the terminal's thread: written once, before that thread starts.
    session::TerminalSession* running = nullptr;
    /// Whether a search is under way. The session is told once when one starts, not per keystroke:
    /// starting moves where the search counts from.
    bool searching = false;
};

Terminal::Terminal(Runtime& runtime, QSize page, QObject* parent):
    QObject { parent }, _impl { std::make_unique<Impl>() }
{
    auto const pageSize =
        vtpty::PageSize { vtpty::LineCount(page.height()), vtpty::ColumnCount(page.width()) };
    auto pty = std::make_unique<HostPty>(pageSize, [this] {
        if (!_impl->running->terminal().hasInput())
            return;
        // Writing to the device is the GUI thread's, as it is when a display posts the flush.
        QMetaObject::invokeMethod(
            this, [this] { _impl->session->terminal().flushInput(); }, Qt::QueuedConnection);
    });
    _impl->pty = pty.get();

    // The sink runs on the terminal's own thread; the signal is the host's, on the GUI thread.
    _impl->pty->setWriteSink([this](std::string_view bytes) {
        QMetaObject::invokeMethod(
            this,
            [this, copy = QByteArray { bytes.data(), static_cast<qsizetype>(bytes.size()) }] {
                emit input(copy);
            },
            Qt::QueuedConnection);
    });

    auto& app = *runtime._impl->app;
    // A notifier that raises nothing. Left to its default a session takes the platform's own, and a
    // child's OSC 99 or OSC 777 then goes straight to the desktop: no permission governs it and no
    // display is needed for it, so this is the only place it can be refused.
    _impl->session = std::make_unique<session::TerminalSession>(&app.sessionsManager(),
                                                                std::move(pty),
                                                                app,
                                                                std::string {},
                                                                pageSize,
                                                                std::nullopt,
                                                                std::make_unique<platform::NullNotifier>(),
                                                                session::PageSizeAuthority::Host);

    // What the device is told about its size is not a request: it is the terminal reporting a resize
    // that has already happened, and it also fires when a display merely attaches. The request is
    // the display's fit, which resizes nothing.
    connect(_impl->session.get(),
            &session::TerminalSession::pageFitChanged,
            this,
            [this](int columns, int lines) { emit pageSizeRequested(QSize { columns, lines }); });

    // Queued, all three. The session announces a view that moved and a search that changed from
    // inside the call that did it, with the terminal locked: its own properties are written to be
    // read there without the lock. A host's slot asks this object, whose answers take the lock, so
    // it is told from the event loop instead, once the call has returned.
    connect(_impl->session.get(),
            &session::TerminalSession::scrollOffsetChanged,
            this,
            &Terminal::scrollChanged,
            Qt::QueuedConnection);
    connect(_impl->session.get(),
            &session::TerminalSession::historyLineCountChanged,
            this,
            &Terminal::scrollChanged,
            Qt::QueuedConnection);
    connect(_impl->session.get(),
            &session::TerminalSession::searchStateChanged,
            this,
            &Terminal::searchChanged,
            Qt::QueuedConnection);

    // A display starts the session when one is attached. A host may feed a terminal that has no
    // display yet, so it is started here; start() is a no-op the second time.
    _impl->running = _impl->session.get();
    _impl->session->start();
}

Terminal::~Terminal()
{
    // The manager remembers which session has the focus, to tell it when it loses it. It forgets a
    // session it made itself when that session closes; this one it did not make, so it is told here.
    _impl->session->app().sessionsManager().clearFocusIfCurrent(_impl->session.get());

    _impl->session->terminate();

    // Destroyed here, by hand, rather than left to _impl's own destructor. The session's destructor
    // is what joins the terminal's thread, and until it has, that thread may still come back to
    // read() and reach _impl->running through this object. That has to find _impl in place -- which
    // the body of this destructor guarantees and the destruction of the member does not: a
    // unique_ptr may already be null while its deleter runs.
    _impl->session.reset();
}

QObject* Terminal::session() const noexcept
{
    return _impl->session.get();
}

QSize Terminal::pageSize() const
{
    auto const page = _impl->session->terminal().totalPageSize();
    return QSize { page.columns.as<int>(), page.lines.as<int>() };
}

void Terminal::setPageSize(QSize page)
{
    _impl->session->resizePage(
        vtbackend::PageSize { vtbackend::LineCount(page.height()), vtbackend::ColumnCount(page.width()) });
}

double Terminal::fontSize() const
{
    return _impl->session->profile().fonts.value().size.pt;
}

void Terminal::setFontSize(double points)
{
    _impl->session->setFontSize(text::FontSize { points });
}

void Terminal::setColorScheme(Qt::ColorScheme scheme)
{
    _impl->session->updateColorPreference(scheme == Qt::ColorScheme::Light
                                              ? vtbackend::ColorPreference::Light
                                              : vtbackend::ColorPreference::Dark);
    _impl->session->scheduleRedraw();
}

void Terminal::setAccessibleName(QString const& name)
{
    _impl->session->setAccessibleName(name);
}

void Terminal::feed(QByteArrayView bytes)
{
    _impl->pty->feed(std::string_view { bytes.data(), static_cast<std::size_t>(bytes.size()) });
}

namespace
{

    /// One of a cell's colours, as the public header describes one.
    /// @param palette The palette in force.
    /// @param color   The cell's colour.
    /// @param target  Which of the two it is, which decides what Default means.
    /// @return The colour, with what the palette makes of it.
    [[nodiscard]] CellColor describeColor(vtbackend::ColorPalette const& palette,
                                          vtbackend::Color color,
                                          vtbackend::ColorTarget target)
    {
        auto const resolved = vtbackend::apply(palette, color, target, vtbackend::ColorMode::Normal);
        auto const rgb = qRgb(resolved.red, resolved.green, resolved.blue);
        if (vtbackend::isIndexedColor(color))
            return { .kind = ColorKind::Indexed, .index = vtbackend::getIndexedColor(color), .rgb = rgb };
        if (vtbackend::isBrightColor(color))
            return { .kind = ColorKind::Indexed, .index = 8 + vtbackend::getBrightColor(color), .rgb = rgb };
        if (vtbackend::isRGBColor(color))
            return { .kind = ColorKind::Rgb, .index = -1, .rgb = rgb };
        return { .kind = ColorKind::Default, .index = -1, .rgb = rgb };
    }

    /// The attributes a cell's flags amount to.
    /// @param flags The cell's flags.
    /// @return The same, in the public header's terms.
    [[nodiscard]] CellAttributes describeFlags(vtbackend::CellFlags flags)
    {
        using vtbackend::CellFlag;
        auto constexpr Pairs = std::array {
            std::pair { CellFlag::Bold, CellAttribute::Bold },
            std::pair { CellFlag::Faint, CellAttribute::Faint },
            std::pair { CellFlag::Italic, CellAttribute::Italic },
            std::pair { CellFlag::Underline, CellAttribute::Underline },
            std::pair { CellFlag::DoublyUnderlined, CellAttribute::DoubleUnderline },
            std::pair { CellFlag::CurlyUnderlined, CellAttribute::CurlyUnderline },
            std::pair { CellFlag::DottedUnderline, CellAttribute::DottedUnderline },
            std::pair { CellFlag::DashedUnderline, CellAttribute::DashedUnderline },
            std::pair { CellFlag::Blinking, CellAttribute::Blink },
            std::pair { CellFlag::RapidBlinking, CellAttribute::Blink },
            std::pair { CellFlag::Inverse, CellAttribute::Inverse },
            std::pair { CellFlag::Hidden, CellAttribute::Hidden },
            std::pair { CellFlag::CrossedOut, CellAttribute::CrossedOut },
        };
        auto attributes = CellAttributes {};
        for (auto const& [flag, attribute]: Pairs)
            if (flags.contains(flag))
                attributes |= attribute;
        return attributes;
    }

} // namespace

Cell Terminal::cell(int line, int column) const
{
    auto const& terminal = _impl->session->terminal();
    auto const lock = std::scoped_lock { terminal };
    auto const& screen = terminal.currentScreen();
    auto const page = terminal.pageSize();
    if (column < 0 || column >= page.columns.as<int>() || line >= page.lines.as<int>()
        || line < -screen.historyLineCount().as<int>())
        return {};

    auto const at = screen.at(vtbackend::LineOffset(line), vtbackend::ColumnOffset(column));
    auto const flags = at.flags();
    auto const& palette = terminal.colorPalette();
    return Cell {
        .text = QString::fromStdU32String(at.codepoints()),
        .width = flags.contains(vtbackend::CellFlag::WideCharContinuation) ? 0 : at.width(),
        .attributes = describeFlags(flags),
        .foreground = describeColor(palette, at.foregroundColor(), vtbackend::ColorTarget::Foreground),
        .background = describeColor(palette, at.backgroundColor(), vtbackend::ColorTarget::Background),
    };
}

Cursor Terminal::cursor() const
{
    auto const& terminal = _impl->session->terminal();
    auto const lock = std::scoped_lock { terminal };
    auto const position = terminal.currentScreen().cursor().position;
    return Cursor { .line = position.line.as<int>(),
                    .column = position.column.as<int>(),
                    .visible = terminal.isModeEnabled(vtbackend::DECMode::VisibleCursor) };
}

bool Terminal::alternateScreen() const
{
    auto const& terminal = _impl->session->terminal();
    auto const lock = std::scoped_lock { terminal };
    return terminal.isAlternateScreen();
}

int Terminal::historyLineCount() const
{
    auto const& terminal = _impl->session->terminal();
    auto const lock = std::scoped_lock { terminal };
    return terminal.currentScreen().historyLineCount().as<int>();
}

void Terminal::select(QPoint from, QPoint to)
{
    auto& terminal = _impl->session->terminal();
    {
        auto const lock = std::scoped_lock { terminal };
        auto const place = [&](QPoint point) {
            auto const page = terminal.pageSize();
            auto const top = -terminal.currentScreen().historyLineCount().as<int>();
            return vtbackend::CellLocation {
                .line = vtbackend::LineOffset(std::clamp(point.y(), top, page.lines.as<int>() - 1)),
                .column = vtbackend::ColumnOffset(std::clamp(point.x(), 0, page.columns.as<int>() - 1)),
            };
        };
        terminal.setSelector(std::make_unique<vtbackend::LinearSelection>(
            terminal.selectionHelper(), place(from), terminal.selectionUpdatedHelper()));
        (void) terminal.selector()->extend(place(to));
        terminal.selector()->complete();
        terminal.onSelectionUpdated();
    }
    _impl->session->scheduleRedraw();
}

void Terminal::selectAll()
{
    auto& terminal = _impl->session->terminal();
    {
        auto const lock = std::scoped_lock { terminal };
        terminal.selectAll();
    }
    _impl->session->scheduleRedraw();
}

void Terminal::clearSelection()
{
    auto& terminal = _impl->session->terminal();
    {
        auto const lock = std::scoped_lock { terminal };
        terminal.clearSelection();
    }
    _impl->session->scheduleRedraw();
}

bool Terminal::hasSelection() const
{
    auto const& terminal = _impl->session->terminal();
    auto const lock = std::scoped_lock { terminal };
    return terminal.selectionAvailable();
}

QString Terminal::selectionText() const
{
    auto const& terminal = _impl->session->terminal();
    auto const lock = std::scoped_lock { terminal };
    if (!terminal.selectionAvailable())
        return {};
    return QString::fromStdString(terminal.extractSelectionText());
}

int Terminal::scrollOffset() const
{
    return _impl->session->scrollOffset();
}

void Terminal::setScrollOffset(int lines)
{
    _impl->session->setScrollOffset(std::clamp(lines, 0, historyLineCount()));
}

void Terminal::setSearchPattern(QString const& pattern)
{
    auto& session = *_impl->session;
    if (pattern.isEmpty())
    {
        session.setSearchPattern({});
        if (std::exchange(_impl->searching, false))
            session.searchBarClosed();
        return;
    }
    // The session counts matches only while it believes something shows the count.
    if (!std::exchange(_impl->searching, true))
        session.searchBarOpened();
    session.setSearchPattern(pattern);
}

SearchState Terminal::searchState() const
{
    auto& terminal = _impl->session->terminal();
    auto const lock = std::scoped_lock { terminal };
    auto const& pattern = terminal.search().pattern;
    if (pattern.empty())
        return {};
    auto const tally = terminal.tallySearchMatches(terminal.normalModeCursorPosition());
    return SearchState { .pattern = QString::fromStdU32String(pattern),
                         .matches = static_cast<int>(tally.total),
                         .current = static_cast<int>(tally.ordinal),
                         .capped = tally.exactness == vtbackend::TallyExactness::Capped };
}

void Terminal::searchNext()
{
    if (_impl->searching)
        _impl->session->searchNext();
}

void Terminal::searchPrevious()
{
    if (_impl->searching)
        _impl->session->searchPrevious();
}

bool Terminal::scrollToPreviousMark()
{
    auto& terminal = _impl->session->terminal();
    auto const lock = std::scoped_lock { terminal };
    return terminal.viewport().scrollMarkUp();
}

bool Terminal::scrollToNextMark()
{
    auto& terminal = _impl->session->terminal();
    auto const lock = std::scoped_lock { terminal };
    return terminal.viewport().scrollMarkDown();
}

QString Terminal::mainPageText() const
{
    // The terminal's own thread is writing the page this reads.
    auto const lock = std::scoped_lock { _impl->session->terminal() };
    return QString::fromStdString(_impl->session->terminal().primaryScreen().renderMainPageText());
}

} // namespace contour::embed
