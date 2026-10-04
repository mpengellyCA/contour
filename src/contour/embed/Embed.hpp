// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The terminal display, for a Qt Quick application that is not Contour.
///
/// A host constructs one Runtime after its Qt application, then one Terminal per terminal it shows.
/// A Terminal has no child process: the host feed()s it the bytes a child wrote and receives, as
/// signals, the bytes the terminal would have written back and the page size it would like. In QML
/// the host imports `Contour.Terminal` and gives a `ContourTerminal` item the Terminal's `session`.
///
/// Nothing in this header names a Contour type, so a host compiles against Qt alone.

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtGui/QSurfaceFormat>

#include <filesystem>
#include <memory>

#ifdef CONTOUR_EMBED_BUILD
    #define CONTOUR_EMBED_API Q_DECL_EXPORT
#else
    #define CONTOUR_EMBED_API Q_DECL_IMPORT
#endif

namespace contour::embed
{

/// What a Runtime is built from. Fixed for its lifetime.
struct RuntimeOptions
{
    /// A Contour configuration file. Empty means the built-in embed profile: no status line, no
    /// key bindings, no scrollbar, and every child-requested permission denied.
    ///
    /// A file that is named is read once, as a whole Contour configuration: what it does not say is
    /// Contour's default, not the built-in embed profile's. A file that parses may therefore bind
    /// keys or allow a permission the built-in profile denies, by saying so or by leaving it out;
    /// that is the host's choice and is honoured.
    ///
    /// A file that cannot be used is not replaced by Contour's defaults. If the path is missing, is
    /// not a readable file, is not a YAML mapping, or names a default profile it does not define, the
    /// Runtime uses the built-in embed profile instead and creates nothing at the path. It reports
    /// why on Contour's `error` logging category, which a process that has not turned Contour's
    /// logging on does not print.
    ///
    /// Whichever configuration is loaded, only the profile sessions start in is kept and
    /// `live_config` is not honoured: a child can ask for a profile by name, so none is left for it
    /// to ask for.
    std::filesystem::path configFile;
};

/// The process-wide part of the terminal: configuration, fonts, and the QML registration.
///
/// One per process, constructed after the Qt application and before the first QML engine loads a
/// file that imports `Contour.Terminal`. It must outlive every Terminal.
class CONTOUR_EMBED_API Runtime final: public QObject
{
    Q_OBJECT

  public:
    /// @param options Where the configuration comes from.
    /// @param parent  The usual QObject parent.
    explicit Runtime(RuntimeOptions options, QObject* parent = nullptr);
    ~Runtime() override;

    /// The surface format the display renders with. A host sets it as the default format before it
    /// creates its first window.
    /// @return The format.
    [[nodiscard]] static QSurfaceFormat surfaceFormat();

    /// Test seam: asks the runtime to open @p url exactly as a clicked hyperlink would.
    /// @param url The address.
    /// @return Whether the request was accepted (it is then announced, never opened).
    [[nodiscard]] bool offerUrlForTesting(QUrl const& url);

    /// Test seam: how many key and mouse bindings the loaded configuration carries.
    /// @return The count.
    [[nodiscard]] int inputMappingCountForTesting() const;

  signals:
    /// A terminal asked for @p url to be opened. The library never opens it; the host decides.
    void openUrlRequested(QUrl const& url);

  private:
    friend class Terminal;
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

/// One terminal: an emulator and its screen, with no child process behind it.
class CONTOUR_EMBED_API Terminal final: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* session READ session CONSTANT)

  public:
    /// @param runtime The runtime; must outlive this object.
    /// @param page    The initial page: width is columns, height is lines.
    /// @param parent  The usual QObject parent.
    Terminal(Runtime& runtime, QSize page, QObject* parent = nullptr);
    ~Terminal() override;

    /// What a `ContourTerminal` item's `session` property takes.
    /// @return The session object; owned by this Terminal.
    [[nodiscard]] QObject* session() const noexcept;

    /// Gives the terminal bytes a child wrote. Callable from any thread.
    /// @param bytes The bytes, in order.
    void feed(QByteArrayView bytes);

    /// The main page as text, one line per row. For a host's own tests.
    /// @return The text.
    [[nodiscard]] QString mainPageText() const;

  signals:
    /// Bytes for the child: keys, pastes, mouse reports and replies. Emitted on the GUI thread.
    void input(QByteArray const& bytes);

    /// The display would like the page to be @p page (width columns, height lines). Emitted on the
    /// GUI thread. The host decides whether the child is told.
    void pageSizeRequested(QSize page);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace contour::embed
