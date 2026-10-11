// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The terminal display, for a Qt Quick application that is not Contour.
///
/// A host constructs one Runtime after its Qt application, then one Terminal per terminal it shows.
/// A Terminal has no child process: the host feed()s it the bytes a child wrote and receives, as
/// signals, the bytes the terminal would have written back and the page size its display would fit.
/// The page is the host's: nothing but setPageSize() changes it. In QML the host imports
/// `Contour.Terminal` and gives a `ContourTerminal` item the Terminal's `session`.
///
/// Nothing in this header names a Contour type, so a host compiles against Qt alone.

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QFlags>
#include <QtCore/QObject>
#include <QtCore/QPoint>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/Qt>
#include <QtGui/QRgb>
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

/// How a cell's colour was asked for.
enum class ColorKind : quint8
{
    Default, ///< The terminal's own foreground or background.
    Indexed, ///< One of the 256 palette entries; the bright eight are 8 to 15.
    Rgb,     ///< A direct colour.
};

/// One of a cell's two colours.
struct CellColor
{
    ColorKind kind = ColorKind::Default; ///< How it was asked for.
    int index = -1;                      ///< The palette entry, when kind is Indexed; otherwise -1.
    /// What the palette in force makes of it. Not what is painted: an inverse, faint or hidden cell
    /// is painted otherwise, and those are attributes.
    QRgb rgb = 0;
};

/// What a cell is drawn with, beside its colours.
enum class CellAttribute : quint16
{
    Bold = 1U << 0U,
    Faint = 1U << 1U,
    Italic = 1U << 2U,
    Underline = 1U << 3U,
    DoubleUnderline = 1U << 4U,
    CurlyUnderline = 1U << 5U,
    DottedUnderline = 1U << 6U,
    DashedUnderline = 1U << 7U,
    Blink = 1U << 8U,
    Inverse = 1U << 9U,
    Hidden = 1U << 10U,
    CrossedOut = 1U << 11U,
};
Q_DECLARE_FLAGS(CellAttributes, CellAttribute)
Q_DECLARE_OPERATORS_FOR_FLAGS(CellAttributes)

/// One cell of a screen.
struct Cell
{
    /// The cell's characters: a base and whatever combines with it. Empty for a cell nothing was
    /// written to, and for the second column of a wide character.
    QString text;
    /// The columns the cell's character takes: 1, 2 for a wide one, and 0 for the column a wide one
    /// to its left covers.
    int width = 1;
    CellAttributes attributes; ///< How it is drawn.
    CellColor foreground;      ///< The colour of its character.
    CellColor background;      ///< The colour behind it.
};

/// Where the cursor is, and whether it is shown.
struct Cursor
{
    int line = 0;        ///< Counted from the top of the page, from 0.
    int column = 0;      ///< Counted from the left, from 0.
    bool visible = true; ///< Whether the child has it shown (DECTCEM).
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

    /// The page: width is columns, height is lines.
    /// @return The size the terminal has now.
    [[nodiscard]] QSize pageSize() const;

    /// Resizes the page. The only thing that does: a display shows the page it is given, with
    /// background beside a page smaller than its pixels and the edge cutting off a larger one.
    /// @param page The page wanted: width is columns, height is lines.
    void setPageSize(QSize page);

    /// The size text is drawn at, in points.
    /// @return The size in force: the profile's until setFontSize() changes it.
    [[nodiscard]] double fontSize() const;

    /// Changes the size text is drawn at. The page is not resized: a display then fits a different
    /// one, and says so through pageSizeRequested.
    /// @param points The size wanted. One the display cannot load leaves the size as it was.
    void setFontSize(double points);

    /// Says which of a profile's two palettes to draw with, for a profile that names one for a dark
    /// scheme and one for a light. A profile with one palette is unaffected.
    /// @param scheme Qt::ColorScheme::Light for the light palette; anything else is the dark one,
    ///               which is what a terminal starts with.
    void setColorScheme(Qt::ColorScheme scheme);

    /// Names the terminal for assistive technology. Without one a display is named by the window
    /// title, which the child sets; a host that shows the terminal inside its own interface names
    /// it itself. The name is read when it is asked for and never announced.
    /// @param name The name; an empty one returns to the title.
    void setAccessibleName(QString const& name);

    /// Gives the terminal bytes a child wrote. Callable from any thread.
    /// @param bytes The bytes, in order.
    void feed(QByteArrayView bytes);

    /// The main page as text, one line per row. For a host's own tests.
    /// @return The text.
    [[nodiscard]] QString mainPageText() const;

    /// One cell of the screen that is up, primary or alternate.
    /// @param line   The line, from 0 at the top of the page. A negative line is scrollback: -1 is
    ///               the line that most recently left the top.
    /// @param column The column, from 0 at the left.
    /// @return The cell, or a default-constructed one for a place the screen does not have.
    [[nodiscard]] Cell cell(int line, int column) const;

    /// The cursor of the screen that is up.
    /// @return Its place and whether it is shown.
    [[nodiscard]] Cursor cursor() const;

    /// Which screen is up.
    /// @return True for the alternate screen, false for the primary one.
    [[nodiscard]] bool alternateScreen() const;

    /// How many lines have left the top of the screen that is up and are kept.
    /// @return The count; 0 on the alternate screen, which keeps none.
    [[nodiscard]] int historyLineCount() const;

    /// Selects the text between two cells, in reading order, replacing any selection.
    ///
    /// A point's x is a column and its y a line, as cell() counts them, so a negative line reaches
    /// into the scrollback. Either end may come first.
    /// @param from One end.
    /// @param to   The other end; selected too.
    void select(QPoint from, QPoint to);

    /// Selects everything the terminal holds: the scrollback and the page.
    void selectAll();

    /// Drops the selection, if there is one.
    void clearSelection();

    /// Whether anything is selected, by the host or by the pointer.
    /// @return True when there is a selection.
    [[nodiscard]] bool hasSelection() const;

    /// The selected text. The library puts it nowhere: what is copied, and to where, is the host's.
    /// @return The text, lines separated by a line feed; empty when nothing is selected.
    [[nodiscard]] QString selectionText() const;

    /// Scrolls back to the nearest mark above the top of what is shown. A mark is a line a shell
    /// said its prompt starts on (OSC 133 ; A).
    /// @return Whether the view moved; false when there is no mark above.
    bool scrollToPreviousMark();

    /// Scrolls forward to the nearest mark below the top of what is shown, or to the bottom when
    /// there is none.
    /// @return Whether the view moved.
    bool scrollToNextMark();

  signals:
    /// Bytes for the child: keys, pastes, mouse reports and replies. Emitted on the GUI thread.
    void input(QByteArray const& bytes);

    /// The display's pixels would fit a page of @p page (width columns, height lines). Emitted on the
    /// GUI thread when a display is attached, resized or changes its font, and only when the answer
    /// differs from the last one given. The terminal is not resized: the host decides whether to
    /// call setPageSize(), and whether the child is told.
    void pageSizeRequested(QSize page);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace contour::embed
