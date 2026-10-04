// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/config/Config.hpp>
#include <contour/display/ShaderConfig.hpp>
#include <contour/display/TerminalAccessible.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/embed/Embed.hpp>
#include <contour/embed/EmbedConfig.yml.in.hpp>
#include <contour/platform/ExternalLauncher.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>

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
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

} // namespace

struct Runtime::Impl
{
    RuntimeOptions options; ///< What the runtime was built from.
    core::LiveEnvironment environment;
    AnnouncingLauncher* launcher = nullptr; ///< Owned by the app; observed here for the test seam.
    std::unique_ptr<ContourGuiApp> app;
};

Runtime::Runtime(RuntimeOptions options, QObject* parent):
    QObject { parent }, _impl { std::make_unique<Impl>() }
{
    _impl->options = std::move(options);

    auto launcher = std::make_unique<AnnouncingLauncher>(*this);
    _impl->launcher = launcher.get();
    _impl->app = std::make_unique<ContourGuiApp>(_impl->environment, nullptr, std::move(launcher));

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

bool Runtime::offerUrlForTesting(QUrl const& url)
{
    return _impl->launcher->openUrl(url).has_value();
}

int Runtime::inputMappingCountForTesting() const
{
    auto const& mappings = _impl->app->config().inputMappings.value();
    return static_cast<int>(mappings.keyMappings.size() + mappings.charMappings.size()
                            + mappings.mouseMappings.size());
}

struct Terminal::Impl
{
    vtpty::ChannelPty* pty = nullptr; ///< Owned by the session's terminal.
    std::unique_ptr<session::TerminalSession> session;
    /// The same session, for the terminal's thread: written once, before that thread starts.
    session::TerminalSession* running = nullptr;
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

    // Both sinks run on the terminal's own threads; the signals are the host's, on the GUI thread.
    _impl->pty->setWriteSink([this](std::string_view bytes) {
        QMetaObject::invokeMethod(
            this,
            [this, copy = QByteArray { bytes.data(), static_cast<qsizetype>(bytes.size()) }] {
                emit input(copy);
            },
            Qt::QueuedConnection);
    });
    _impl->pty->setResizeSink([this](vtpty::PageSize requested, std::optional<vtpty::ImageSize>) {
        auto const asked =
            QSize { static_cast<int>(unbox(requested.columns)), static_cast<int>(unbox(requested.lines)) };
        QMetaObject::invokeMethod(
            this, [this, asked] { emit pageSizeRequested(asked); }, Qt::QueuedConnection);
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
                                                                std::make_unique<platform::NullNotifier>());

    // A display starts the session when one is attached. A host may feed a terminal that has no
    // display yet, so it is started here; start() is a no-op the second time.
    _impl->running = _impl->session.get();
    _impl->session->start();
}

Terminal::~Terminal()
{
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

void Terminal::feed(QByteArrayView bytes)
{
    _impl->pty->feed(std::string_view { bytes.data(), static_cast<std::size_t>(bytes.size()) });
}

QString Terminal::mainPageText() const
{
    // The terminal's own thread is writing the page this reads.
    auto const lock = std::scoped_lock { _impl->session->terminal() };
    return QString::fromStdString(_impl->session->terminal().primaryScreen().renderMainPageText());
}

} // namespace contour::embed
