// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistwidget.h"
#include "gamelistrefreshthread.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"

#include "core/fullscreen_ui.h"
#include "core/game_list.h"
#include "core/host.h"
#include "core/settings.h"
#include "core/system.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/string_util.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyledItemDelegate>

static constexpr float MIN_SCALE = 0.1f;
static constexpr float MAX_SCALE = 2.0f;

static const char* SUPPORTED_FORMATS_STRING =
  QT_TRANSLATE_NOOP(GameListWidget, ".cue (Cue Sheets)\n"
                                    ".iso/.img (Single Track Image)\n"
                                    ".ecm (Error Code Modeling Image)\n"
                                    ".mds (Media Descriptor Sidecar)\n"
                                    ".chd (Compressed Hunks of Data)\n"
                                    ".pbp (PlayStation Portable, Only Decrypted)");

static constexpr std::array<const char*, GameListModel::Column_Count> s_column_names = {
  {"Icon", "Serial", "Title", "File Title", "Developer", "Publisher", "Genre", "Year", "Players", "Time Played",
   "Last Played", "Size", "File Size", "Region", "Achievements", "Compatibility", "Cover"}};

static constexpr int COVER_ART_WIDTH = 512;
static constexpr int COVER_ART_HEIGHT = 512;
static constexpr int COVER_ART_SPACING = 32;
static constexpr int MIN_COVER_CACHE_SIZE = 256;

static void resizeAndPadImage(QImage* image, int expected_width, int expected_height, bool fill_with_top_left)
{
  const qreal dpr = image->devicePixelRatio();
  const int dpr_expected_width = static_cast<int>(static_cast<qreal>(expected_width) * dpr);
  const int dpr_expected_height = static_cast<int>(static_cast<qreal>(expected_height) * dpr);
  if (image->width() == dpr_expected_width && image->height() == dpr_expected_height)
    return;

  if ((static_cast<float>(image->width()) / static_cast<float>(image->height())) >=
      (static_cast<float>(dpr_expected_width) / static_cast<float>(dpr_expected_height)))
  {
    *image = image->scaledToWidth(dpr_expected_width, Qt::SmoothTransformation);
  }
  else
  {
    *image = image->scaledToHeight(dpr_expected_height, Qt::SmoothTransformation);
  }

  if (image->width() == dpr_expected_width && image->height() == dpr_expected_height)
    return;

  // QPainter works in unscaled coordinates.
  int xoffs = 0;
  int yoffs = 0;
  const int image_width = image->width();
  const int image_height = image->height();
  if (image_width < dpr_expected_width)
    xoffs = static_cast<int>(static_cast<qreal>((dpr_expected_width - image_width) / 2) / dpr);
  if (image_height < dpr_expected_height)
    yoffs = static_cast<int>(static_cast<qreal>((dpr_expected_height - image_height) / 2) / dpr);

  QImage padded_image(dpr_expected_width, dpr_expected_height, QImage::Format_ARGB32);
  padded_image.setDevicePixelRatio(dpr);
  if (fill_with_top_left)
    padded_image.fill(image->pixel(0, 0));
  else
    padded_image.fill(Qt::transparent);

  QPainter painter;
  if (painter.begin(&padded_image))
  {
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(xoffs, yoffs, *image);
    painter.end();
  }

  *image = std::move(padded_image);
}

std::optional<GameListModel::Column> GameListModel::getColumnIdForName(std::string_view name)
{
  for (int column = 0; column < Column_Count; column++)
  {
    if (name == s_column_names[column])
      return static_cast<Column>(column);
  }

  return std::nullopt;
}

const char* GameListModel::getColumnName(Column col)
{
  return s_column_names[static_cast<int>(col)];
}

GameListModel::GameListModel(float cover_scale, bool show_cover_titles, bool show_game_icons,
                             QObject* parent /* = nullptr */)
  : QAbstractTableModel(parent), m_show_titles_for_covers(show_cover_titles), m_show_game_icons(show_game_icons),
    m_memcard_pixmap_cache(MIN_COVER_CACHE_SIZE)
{
  loadCommonImages();
  setCoverScale(cover_scale);
  setColumnDisplayNames();

  if (m_show_game_icons)
    GameList::ReloadMemcardTimestampCache();

  connect(g_emu_thread, &EmuThread::gameListRowsChanged, this, &GameListModel::rowsChanged);
}

GameListModel::~GameListModel() = default;

void GameListModel::setShowGameIcons(bool enabled)
{
  m_show_game_icons = enabled;

  beginResetModel();
  m_memcard_pixmap_cache.Clear();
  if (enabled)
    GameList::ReloadMemcardTimestampCache();
  endResetModel();
}

void GameListModel::setCoverScale(float scale)
{
  if (m_cover_scale == scale)
    return;

  m_cover_pixmap_cache.Clear();
  m_cover_scale = scale;

  const qreal dpr = qApp->devicePixelRatio();

  QImage loading_image;
  if (loading_image.load(QStringLiteral("%1/images/placeholder.png").arg(QtHost::GetResourcesBasePath())))
  {
    loading_image.setDevicePixelRatio(dpr);
    resizeAndPadImage(&loading_image, getCoverArtWidth(), getCoverArtHeight(), false);
  }
  else
  {
    loading_image = QImage(getCoverArtWidth(), getCoverArtHeight(), QImage::Format_RGB32);
    loading_image.setDevicePixelRatio(dpr);
    loading_image.fill(QColor(0, 0, 0, 0));
  }
  m_loading_pixmap = QPixmap::fromImage(loading_image);

  m_placeholder_image = QImage();
  if (m_placeholder_image.load(QStringLiteral("%1/images/cover-placeholder.png").arg(QtHost::GetResourcesBasePath())))
  {
    m_placeholder_image.setDevicePixelRatio(dpr);
    resizeAndPadImage(&m_placeholder_image, getCoverArtWidth(), getCoverArtHeight(), false);
  }
  else
  {
    m_placeholder_image = QImage(getCoverArtWidth(), getCoverArtHeight(), QImage::Format_RGB32);
    m_placeholder_image.setDevicePixelRatio(dpr);
    m_placeholder_image.fill(QColor(0, 0, 0, 0));
  }

  emit coverScaleChanged();
}

void GameListModel::refreshCovers()
{
  m_cover_pixmap_cache.Clear();
  refresh();
}

void GameListModel::updateCacheSize(int width, int height)
{
  // This is a bit conversative, since it doesn't consider padding, but better to be over than under.
  const int cover_width = getCoverArtWidth();
  const int cover_height = getCoverArtHeight();
  const int num_columns = ((width + (cover_width - 1)) / cover_width);
  const int num_rows = ((height + (cover_height - 1)) / cover_height);
  m_cover_pixmap_cache.SetMaxCapacity(static_cast<int>(std::max(num_columns * num_rows, MIN_COVER_CACHE_SIZE)));
}

void GameListModel::reloadThemeSpecificImages()
{
  loadThemeSpecificImages();
  refresh();
}

void GameListModel::loadOrGenerateCover(const GameList::Entry* ge)
{
  QtAsyncTask::create(this, [path = ge->path, serial = ge->serial, title = ge->title,
                             placeholder_image = m_placeholder_image, list = this, width = getCoverArtWidth(),
                             height = getCoverArtHeight(), scale = m_cover_scale,
                             dpr = qApp->devicePixelRatio()]() mutable {
    QImage image;
    loadOrGenerateCover(image, placeholder_image, width, height, scale, dpr, path, serial, title);
    return [path = std::move(path), image = std::move(image), list, scale]() { list->coverLoaded(path, image, scale); };
  });
}

void GameListModel::createPlaceholderImage(QImage& image, const QImage& placeholder_image, int width, int height,
                                           float scale, const std::string& title)
{
  image = placeholder_image.copy();
  if (image.isNull())
    return;

  resizeAndPadImage(&image, width, height, false);

  QPainter painter;
  if (painter.begin(&image))
  {
    QFont font;
    font.setPointSize(std::max(static_cast<int>(32.0f * scale), 1));
    painter.setFont(font);
    painter.setPen(Qt::white);

    const QRect text_rc(0, 0, static_cast<int>(static_cast<float>(width)),
                        static_cast<int>(static_cast<float>(height)));
    painter.drawText(text_rc, Qt::AlignCenter | Qt::TextWordWrap, QString::fromStdString(title));
    painter.end();
  }
}

void GameListModel::loadOrGenerateCover(QImage& image, const QImage& placeholder_image, int width, int height,
                                        float scale, float dpr, const std::string& path, const std::string& serial,
                                        const std::string& title)
{
  const std::string cover_path(GameList::GetCoverImagePath(path, serial, title));
  if (!cover_path.empty())
  {
    image.load(QString::fromStdString(cover_path));
    if (!image.isNull())
    {
      image.setDevicePixelRatio(dpr);
      resizeAndPadImage(&image, width, height, false);
    }
  }

  if (image.isNull())
    createPlaceholderImage(image, placeholder_image, width, height, scale, title);
}

void GameListModel::coverLoaded(const std::string& path, const QImage& image, float scale)
{
  // old request before cover scale change?
  if (m_cover_scale != scale)
    return;

  if (!image.isNull())
    m_cover_pixmap_cache.Insert(path, QPixmap::fromImage(image));
  else
    m_cover_pixmap_cache.Insert(path, QPixmap());

  invalidateCoverForPath(path);
}

void GameListModel::rowsChanged(const QList<int>& rows)
{
  const QList<int> roles_changed = {Qt::DisplayRole};

  // try to collapse multiples
  size_t start = 0;
  size_t idx = 0;
  const size_t size = rows.size();
  for (; idx < size;)
  {
    if ((idx + 1) < size && rows[idx + 1] == (rows[idx] + 1))
    {
      idx++;
    }
    else
    {
      emit dataChanged(createIndex(rows[start], 0), createIndex(rows[idx], Column_Count - 1), roles_changed);
      start = ++idx;
    }
  }
}

void GameListModel::invalidateCoverForPath(const std::string& path)
{
  std::optional<u32> row;
  if (hasTakenGameList())
  {
    for (u32 i = 0; i < static_cast<u32>(m_taken_entries->size()); i++)
    {
      if (path == m_taken_entries.value()[i].path)
      {
        row = i;
        break;
      }
    }
  }
  else
  {
    // This isn't ideal, but not sure how else we can get the row, when it might change while scanning...
    auto lock = GameList::GetLock();
    const u32 count = GameList::GetEntryCount();
    for (u32 i = 0; i < count; i++)
    {
      if (GameList::GetEntryByIndex(i)->path == path)
      {
        row = i;
        break;
      }
    }
  }

  if (!row.has_value())
  {
    // Game removed?
    return;
  }

  const QModelIndex mi(index(static_cast<int>(row.value()), Column_Cover));
  emit dataChanged(mi, mi, {Qt::DecorationRole});
}

QString GameListModel::formatTimespan(time_t timespan)
{
  // avoid an extra string conversion
  const u32 hours = static_cast<u32>(timespan / 3600);
  const u32 minutes = static_cast<u32>((timespan % 3600) / 60);
  if (hours > 0)
    return qApp->translate("GameList", "%n hours", "", hours);
  else
    return qApp->translate("GameList", "%n minutes", "", minutes);
}

const QPixmap& GameListModel::getIconPixmapForEntry(const GameList::Entry* ge) const
{
  // We only do this for discs/disc sets for now.
  if (m_show_game_icons && (!ge->serial.empty() && (ge->IsDisc() || ge->IsDiscSet())))
  {
    QPixmap* item = m_memcard_pixmap_cache.Lookup(ge->serial);
    if (item)
      return *item;

    // Assumes game list lock is held.
    const std::string path = GameList::GetGameIconPath(ge->serial, ge->path);
    QPixmap pm;
    if (!path.empty() && pm.load(QString::fromStdString(path)))
    {
      fixIconPixmapSize(pm);
      return *m_memcard_pixmap_cache.Insert(ge->serial, std::move(pm));
    }

    return *m_memcard_pixmap_cache.Insert(ge->serial, m_type_pixmaps[static_cast<u32>(ge->type)]);
  }

  return m_type_pixmaps[static_cast<u32>(ge->type)];
}

const QPixmap& GameListModel::getFlagPixmapForEntry(const GameList::Entry* ge) const
{
  static constexpr u32 FLAG_PIXMAP_WIDTH = 30;
  static constexpr u32 FLAG_PIXMAP_HEIGHT = 20;

  const std::string_view name = ge->GetLanguageIcon();
  auto it = m_flag_pixmap_cache.find(name);
  if (it != m_flag_pixmap_cache.end())
    return it->second;

  const QIcon icon(QString::fromStdString(QtHost::GetResourcePath(ge->GetLanguageIconName(), true)));
  it = m_flag_pixmap_cache.emplace(name, icon.pixmap(FLAG_PIXMAP_WIDTH, FLAG_PIXMAP_HEIGHT)).first;
  return it->second;
}

QIcon GameListModel::getIconForGame(const QString& path)
{
  QIcon ret;

  if (m_show_game_icons)
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = GameList::GetEntryForPath(path.toStdString());

    // See above.
    if (entry && !entry->serial.empty() && (entry->IsDisc() || entry->IsDiscSet()))
    {
      const std::string icon_path = GameList::GetGameIconPath(entry->serial, entry->path);
      if (!icon_path.empty())
      {
        QPixmap newpm;
        if (!icon_path.empty() && newpm.load(QString::fromStdString(icon_path)))
        {
          fixIconPixmapSize(newpm);
          ret = QIcon(*m_memcard_pixmap_cache.Insert(entry->serial, std::move(newpm)));
        }
      }
    }
  }

  return ret;
}

void GameListModel::fixIconPixmapSize(QPixmap& pm)
{
  const qreal dpr = pm.devicePixelRatio();
  const int width = static_cast<int>(static_cast<float>(pm.width()) * dpr);
  const int height = static_cast<int>(static_cast<float>(pm.height()) * dpr);
  const int max_dim = std::max(width, height);
  if (max_dim == 16)
    return;

  const float wanted_dpr = qApp->devicePixelRatio();
  pm.setDevicePixelRatio(wanted_dpr);

  const float scale = static_cast<float>(max_dim) / 16.0f / wanted_dpr;
  const int new_width = static_cast<int>(static_cast<float>(width) / scale);
  const int new_height = static_cast<int>(static_cast<float>(height) / scale);
  pm = pm.scaled(new_width, new_height);
}

int GameListModel::getCoverArtWidth() const
{
  return std::max(static_cast<int>(static_cast<float>(COVER_ART_WIDTH) * m_cover_scale), 1);
}

int GameListModel::getCoverArtHeight() const
{
  return std::max(static_cast<int>(static_cast<float>(COVER_ART_HEIGHT) * m_cover_scale), 1);
}

int GameListModel::getCoverArtSpacing() const
{
  return std::max(static_cast<int>(static_cast<float>(COVER_ART_SPACING) * m_cover_scale), 1);
}

int GameListModel::rowCount(const QModelIndex& parent) const
{
  if (parent.isValid()) [[unlikely]]
    return 0;

  if (m_taken_entries.has_value())
    return static_cast<int>(m_taken_entries->size());

  const auto lock = GameList::GetLock();
  return static_cast<int>(GameList::GetEntryCount());
}

int GameListModel::columnCount(const QModelIndex& parent) const
{
  if (parent.isValid())
    return 0;

  return Column_Count;
}

QVariant GameListModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid()) [[unlikely]]
    return {};

  const int row = index.row();
  DebugAssert(row >= 0);

  if (m_taken_entries.has_value()) [[unlikely]]
  {
    if (static_cast<u32>(row) >= m_taken_entries->size())
      return {};

    return data(index, role, &m_taken_entries.value()[row]);
  }
  else
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* ge = GameList::GetEntryByIndex(static_cast<u32>(row));
    if (!ge)
      return {};

    return data(index, role, ge);
  }
}

QVariant GameListModel::data(const QModelIndex& index, int role, const GameList::Entry* ge) const
{
  switch (role)
  {
    case Qt::DisplayRole:
    {
      switch (index.column())
      {
        case Column_Serial:
          return QtUtils::StringViewToQString(ge->serial);

        case Column_Title:
          return QtUtils::StringViewToQString(ge->title);

        case Column_FileTitle:
          return QtUtils::StringViewToQString(Path::GetFileTitle(ge->path));

        case Column_Developer:
          return ge->dbentry ? QtUtils::StringViewToQString(ge->dbentry->developer) : QString();

        case Column_Publisher:
          return ge->dbentry ? QtUtils::StringViewToQString(ge->dbentry->publisher) : QString();

        case Column_Genre:
          return ge->dbentry ? QtUtils::StringViewToQString(ge->dbentry->genre) : QString();

        case Column_Year:
        {
          if (ge->dbentry && ge->dbentry->release_date != 0)
          {
            return QStringLiteral("%1").arg(
              QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ge->dbentry->release_date), QTimeZone::utc())
                .date()
                .year());
          }
          else
          {
            return QString();
          }
        }

        case Column_Players:
        {
          if (!ge->dbentry || ge->dbentry->min_players == 0)
            return QString();
          else if (ge->dbentry->min_players == ge->dbentry->max_players)
            return QStringLiteral("%1").arg(ge->dbentry->min_players);
          else
            return QStringLiteral("%1-%2").arg(ge->dbentry->min_players).arg(ge->dbentry->max_players);
        }

        case Column_FileSize:
          return (ge->file_size >= 0) ?
                   QStringLiteral("%1 MB").arg(static_cast<double>(ge->file_size) / 1048576.0, 0, 'f', 2) :
                   tr("Unknown");

        case Column_UncompressedSize:
          return QStringLiteral("%1 MB").arg(static_cast<double>(ge->uncompressed_size) / 1048576.0, 0, 'f', 2);

        case Column_Achievements:
          return {};

        case Column_TimePlayed:
        {
          if (ge->total_played_time == 0)
            return {};
          else
            return formatTimespan(ge->total_played_time);
        }

        case Column_LastPlayed:
          return QtUtils::StringViewToQString(GameList::FormatTimestamp(ge->last_played_time));

        case Column_Cover:
        {
          if (m_show_titles_for_covers)
            return QString::fromStdString(ge->title);
          else
            return {};
        }

        default:
          return {};
      }
    }

    case Qt::InitialSortOrderRole:
    {
      const int column = index.column();
      if (column == Column_TimePlayed || column == Column_LastPlayed)
        return Qt::DescendingOrder;
      else
        return Qt::AscendingOrder;
    }

    case Qt::DecorationRole:
    {
      switch (index.column())
      {
        case Column_Icon:
        {
          return getIconPixmapForEntry(ge);
        }

        case Column_Region:
        {
          return getFlagPixmapForEntry(ge);
        }

        case Column_Compatibility:
        {
          return m_compatibility_pixmaps[static_cast<u32>(ge->dbentry ? ge->dbentry->compatibility :
                                                                        GameDatabase::CompatibilityRating::Unknown)];
        }

        case Column_Cover:
        {
          QPixmap* pm = m_cover_pixmap_cache.Lookup(ge->path);
          if (pm)
            return *pm;

          // We insert the placeholder into the cache, so that we don't repeatedly
          // queue loading jobs for this game.
          const_cast<GameListModel*>(this)->loadOrGenerateCover(ge);
          return *m_cover_pixmap_cache.Insert(ge->path, m_loading_pixmap);
        }
        break;

        default:
          return {};
      }

      default:
        return {};
    }
  }
}

QVariant GameListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 || section >= Column_Count)
    return {};

  return m_column_display_names[section];
}

const GameList::Entry* GameListModel::getTakenGameListEntry(u32 index) const
{
  return (m_taken_entries.has_value() && index < m_taken_entries->size()) ? &m_taken_entries.value()[index] : nullptr;
}

bool GameListModel::hasTakenGameList() const
{
  return m_taken_entries.has_value();
}

void GameListModel::takeGameList()
{
  const auto lock = GameList::GetLock();
  m_taken_entries = GameList::TakeEntryList();

  // If it's empty (e.g. first boot), don't use it.
  if (m_taken_entries->empty())
    m_taken_entries.reset();
}

void GameListModel::refresh()
{
  beginResetModel();

  m_taken_entries.reset();

  // Invalidate memcard LRU cache, forcing a re-query of the memcard timestamps.
  m_memcard_pixmap_cache.Clear();

  endResetModel();
}

bool GameListModel::titlesLessThan(const GameList::Entry* left, const GameList::Entry* right) const
{
  return (StringUtil::Strcasecmp(left->title.c_str(), right->title.c_str()) < 0);
}

bool GameListModel::lessThan(const QModelIndex& left_index, const QModelIndex& right_index, int column) const
{
  if (!left_index.isValid() || !right_index.isValid())
    return false;

  const int left_row = left_index.row();
  const int right_row = right_index.row();

  if (m_taken_entries.has_value()) [[unlikely]]
  {
    const GameList::Entry* left =
      (static_cast<u32>(left_row) < m_taken_entries->size()) ? &m_taken_entries.value()[left_row] : nullptr;
    const GameList::Entry* right =
      (static_cast<u32>(right_row) < m_taken_entries->size()) ? &m_taken_entries.value()[right_row] : nullptr;
    if (!left || !right)
      return false;

    return lessThan(left, right, column);
  }
  else
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* left = GameList::GetEntryByIndex(left_row);
    const GameList::Entry* right = GameList::GetEntryByIndex(right_row);
    if (!left || !right)
      return false;

    return lessThan(left, right, column);
  }
}

bool GameListModel::lessThan(const GameList::Entry* left, const GameList::Entry* right, int column) const
{
  switch (column)
  {
    case Column_Icon:
    {
      const GameList::EntryType lst = left->GetSortType();
      const GameList::EntryType rst = right->GetSortType();
      if (lst == rst)
        return titlesLessThan(left, right);

      return (static_cast<int>(lst) < static_cast<int>(rst));
    }

    case Column_Serial:
    {
      if (left->serial == right->serial)
        return titlesLessThan(left, right);
      return (StringUtil::Strcasecmp(left->serial.c_str(), right->serial.c_str()) < 0);
    }

    case Column_Title:
    {
      return titlesLessThan(left, right);
    }

    case Column_FileTitle:
    {
      const std::string_view file_title_left = Path::GetFileTitle(left->path);
      const std::string_view file_title_right = Path::GetFileTitle(right->path);
      if (file_title_left == file_title_right)
        return titlesLessThan(left, right);

      const std::size_t smallest = std::min(file_title_left.size(), file_title_right.size());
      return (StringUtil::Strncasecmp(file_title_left.data(), file_title_right.data(), smallest) < 0);
    }

    case Column_Region:
    {
      if (left->region == right->region)
        return titlesLessThan(left, right);
      return (static_cast<int>(left->region) < static_cast<int>(right->region));
    }

    case Column_Compatibility:
    {
      const GameDatabase::CompatibilityRating left_compatibility =
        left->dbentry ? left->dbentry->compatibility : GameDatabase::CompatibilityRating::Unknown;
      const GameDatabase::CompatibilityRating right_compatibility =
        right->dbentry ? right->dbentry->compatibility : GameDatabase::CompatibilityRating::Unknown;
      if (left_compatibility == right_compatibility)
        return titlesLessThan(left, right);

      return (static_cast<int>(left_compatibility) < static_cast<int>(right_compatibility));
    }

    case Column_FileSize:
    {
      if (left->file_size == right->file_size)
        return titlesLessThan(left, right);

      return (left->file_size < right->file_size);
    }

    case Column_UncompressedSize:
    {
      if (left->uncompressed_size == right->uncompressed_size)
        return titlesLessThan(left, right);

      return (left->uncompressed_size < right->uncompressed_size);
    }

    case Column_Genre:
    {
      const int compres =
        StringUtil::CompareNoCase(left->dbentry ? std::string_view(left->dbentry->genre) : std::string_view(),
                                  right->dbentry ? std::string_view(right->dbentry->genre) : std::string_view());
      return (compres == 0) ? titlesLessThan(left, right) : (compres < 0);
    }

    case Column_Developer:
    {
      const int compres =
        StringUtil::CompareNoCase(left->dbentry ? std::string_view(left->dbentry->developer) : std::string_view(),
                                  right->dbentry ? std::string_view(right->dbentry->developer) : std::string_view());
      return (compres == 0) ? titlesLessThan(left, right) : (compres < 0);
    }

    case Column_Publisher:
    {
      const int compres =
        StringUtil::CompareNoCase(left->dbentry ? std::string_view(left->dbentry->publisher) : std::string_view(),
                                  right->dbentry ? std::string_view(right->dbentry->publisher) : std::string_view());
      return (compres == 0) ? titlesLessThan(left, right) : (compres < 0);
    }

    case Column_Year:
    {
      const u64 ldate = left->dbentry ? left->dbentry->release_date : 0;
      const u64 rdate = right->dbentry ? right->dbentry->release_date : 0;
      if (ldate == rdate)
        return titlesLessThan(left, right);

      return (ldate < rdate);
    }

    case Column_TimePlayed:
    {
      if (left->total_played_time == right->total_played_time)
        return titlesLessThan(left, right);

      return (left->total_played_time < right->total_played_time);
    }

    case Column_LastPlayed:
    {
      if (left->last_played_time == right->last_played_time)
        return titlesLessThan(left, right);

      return (left->last_played_time < right->last_played_time);
    }

    case Column_Players:
    {
      const u8 left_players = left->dbentry ? ((left->dbentry->min_players << 4) + left->dbentry->max_players) : 0;
      const u8 right_players = right->dbentry ? ((right->dbentry->min_players << 4) + right->dbentry->max_players) : 0;
      if (left_players == right_players)
        return titlesLessThan(left, right);

      return (left_players < right_players);
    }

    case Column_Achievements:
    {
      // sort by unlock percentage
      const float unlock_left =
        (left->num_achievements > 0) ?
          (static_cast<float>(std::max(left->unlocked_achievements, left->unlocked_achievements_hc)) /
           static_cast<float>(left->num_achievements)) :
          0;
      const float unlock_right =
        (right->num_achievements > 0) ?
          (static_cast<float>(std::max(right->unlocked_achievements, right->unlocked_achievements_hc)) /
           static_cast<float>(right->num_achievements)) :
          0;
      if (std::abs(unlock_left - unlock_right) < 0.0001f)
      {
        // order by achievement count
        if (left->num_achievements == right->num_achievements)
          return titlesLessThan(left, right);

        return (left->num_achievements < right->num_achievements);
      }

      return (unlock_left < unlock_right);
    }

    default:
      return false;
  }
}

void GameListModel::loadThemeSpecificImages()
{
  for (u32 i = 0; i < static_cast<u32>(GameList::EntryType::MaxCount); i++)
    m_type_pixmaps[i] = QtUtils::GetIconForEntryType(static_cast<GameList::EntryType>(i)).pixmap(QSize(24, 24));
}

void GameListModel::loadCommonImages()
{
  loadThemeSpecificImages();

  for (int i = 0; i < static_cast<int>(GameDatabase::CompatibilityRating::Count); i++)
  {
    m_compatibility_pixmaps[i] =
      QtUtils::GetIconForCompatibility(static_cast<GameDatabase::CompatibilityRating>(i)).pixmap(96, 24);
  }

  constexpr int ACHIEVEMENT_ICON_SIZE = 16;
  m_no_achievements_pixmap = QIcon(QString::fromStdString(QtHost::GetResourcePath("images/trophy-icon-gray.svg", true)))
                               .pixmap(ACHIEVEMENT_ICON_SIZE);
  m_has_achievements_pixmap = QIcon(QString::fromStdString(QtHost::GetResourcePath("images/trophy-icon.svg", true)))
                                .pixmap(ACHIEVEMENT_ICON_SIZE);
  m_mastered_achievements_pixmap =
    QIcon(QString::fromStdString(QtHost::GetResourcePath("images/trophy-icon-star.svg", true)))
      .pixmap(ACHIEVEMENT_ICON_SIZE);
}

void GameListModel::setColumnDisplayNames()
{
  m_column_display_names[Column_Icon] = tr("Icon");
  m_column_display_names[Column_Serial] = tr("Serial");
  m_column_display_names[Column_Title] = tr("Title");
  m_column_display_names[Column_FileTitle] = tr("File Title");
  m_column_display_names[Column_Developer] = tr("Developer");
  m_column_display_names[Column_Publisher] = tr("Publisher");
  m_column_display_names[Column_Genre] = tr("Genre");
  m_column_display_names[Column_Year] = tr("Year");
  m_column_display_names[Column_Players] = tr("Players");
  m_column_display_names[Column_Achievements] = tr("Achievements");
  m_column_display_names[Column_TimePlayed] = tr("Time Played");
  m_column_display_names[Column_LastPlayed] = tr("Last Played");
  m_column_display_names[Column_FileSize] = tr("Size");
  m_column_display_names[Column_UncompressedSize] = tr("Raw Size");
  m_column_display_names[Column_Region] = tr("Region");
  m_column_display_names[Column_Compatibility] = tr("Compatibility");
}

class GameListSortModel final : public QSortFilterProxyModel
{
public:
  explicit GameListSortModel(GameListModel* parent) : QSortFilterProxyModel(parent), m_model(parent) {}

  bool isMergingDiscSets() const { return m_merge_disc_sets; }

  void setMergeDiscSets(bool enabled)
  {
    m_merge_disc_sets = enabled;
    invalidateRowsFilter();
  }

  void setFilterType(GameList::EntryType type)
  {
    m_filter_type = type;
    invalidateRowsFilter();
  }
  void setFilterRegion(DiscRegion region)
  {
    m_filter_region = region;
    invalidateRowsFilter();
  }
  void setFilterName(const QString& name)
  {
    m_filter_name = name;
    invalidateRowsFilter();
  }

  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = m_model->hasTakenGameList() ?
                                     m_model->getTakenGameListEntry(static_cast<u32>(source_row)) :
                                     GameList::GetEntryByIndex(static_cast<u32>(source_row));
    if (!entry)
      return false;

    if (m_merge_disc_sets)
    {
      if (entry->disc_set_member)
        return false;
    }
    else
    {
      if (entry->IsDiscSet())
        return false;
    }

    if (m_filter_type != GameList::EntryType::MaxCount && entry->type != m_filter_type)
      return false;

    if (m_filter_region != DiscRegion::Count && entry->region != m_filter_region)
      return false;

    if (!m_filter_name.isEmpty() && !QString::fromStdString(entry->title).contains(m_filter_name, Qt::CaseInsensitive))
      return false;

    return QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent);
  }

  bool lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const override
  {
    return m_model->lessThan(source_left, source_right, source_left.column());
  }

private:
  GameListModel* m_model;
  GameList::EntryType m_filter_type = GameList::EntryType::MaxCount;
  DiscRegion m_filter_region = DiscRegion::Count;
  QString m_filter_name;
  bool m_merge_disc_sets = true;
};

namespace {
class GameListCenterIconStyleDelegate final : public QStyledItemDelegate
{
public:
  GameListCenterIconStyleDelegate(QWidget* parent) : QStyledItemDelegate(parent) {}
  ~GameListCenterIconStyleDelegate() = default;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    // https://stackoverflow.com/questions/32216568/how-to-set-icon-center-in-qtableview
    Q_ASSERT(index.isValid());

    const QRect& r = option.rect;
    const QPixmap pix = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
    const int pix_width = static_cast<int>(pix.width() / pix.devicePixelRatio());
    const int pix_height = static_cast<int>(pix.height() / pix.devicePixelRatio());

    // draw pixmap at center of item
    const QPoint p = QPoint((r.width() - pix_width) / 2, (r.height() - pix_height) / 2);
    painter->drawPixmap(r.topLeft() + p, pix);
  }
};

class GameListAchievementsStyleDelegate : public QStyledItemDelegate
{
public:
  GameListAchievementsStyleDelegate(QWidget* parent, GameListModel* model, GameListSortModel* sort_model)
    : QStyledItemDelegate(parent), m_model(model), m_sort_model(sort_model)
  {
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    Q_ASSERT(index.isValid());

    u32 num_achievements = 0;
    u32 num_unlocked = 0;
    u32 num_unlocked_hardcore = 0;
    bool mastered = false;

    const auto get_data_from_entry = [&num_achievements, &num_unlocked, &num_unlocked_hardcore,
                                      &mastered](const GameList::Entry* entry) {
      if (!entry)
        return;

      num_achievements = entry->num_achievements;
      num_unlocked = entry->unlocked_achievements;
      num_unlocked_hardcore = entry->unlocked_achievements_hc;
      mastered = entry->AreAchievementsMastered();
    };

    const QModelIndex source_index = m_sort_model->mapToSource(index);
    if (m_model->hasTakenGameList()) [[unlikely]]
    {
      get_data_from_entry(m_model->getTakenGameListEntry(static_cast<u32>(source_index.row())));
    }
    else
    {
      const auto lock = GameList::GetLock();
      get_data_from_entry(GameList::GetEntryByIndex(static_cast<u32>(source_index.row())));
    }

    QRect r = option.rect;

    const QPixmap& icon = (num_achievements > 0) ? (mastered ? m_model->getMasteredAchievementsPixmap() :
                                                               m_model->getHasAchievementsPixmap()) :
                                                   m_model->getNoAchievementsPixmap();
    const int icon_width = static_cast<int>(icon.width() / icon.devicePixelRatio());
    const int icon_height = icon_width;
    painter->drawPixmap(r.topLeft() + QPoint(4, (r.height() - icon_height) / 2), icon);
    r.setLeft(r.left() + 12 + icon_width);

    const QPalette& palette = static_cast<QWidget*>(parent())->palette();
    const QColor& text_color =
      palette.color((option.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text);

    if (num_achievements > 0)
    {
      const QFontMetrics fm(painter->fontMetrics());

      // display hardcore in parenthesis only if there are actually hc unlocks
      const bool display_hardcore = (num_unlocked_hardcore > 0);
      const bool display_hardcore_only =
        (display_hardcore && (num_unlocked == 0 || num_unlocked == num_unlocked_hardcore));
      const QString first = QStringLiteral("%1").arg(display_hardcore_only ? num_unlocked_hardcore : num_unlocked);
      const QString total = QStringLiteral("/%3").arg(num_achievements);

      const QColor hc_color = QColor(44, 151, 250);

      painter->setPen(display_hardcore_only ? hc_color : text_color);
      painter->drawText(r, Qt::AlignVCenter, first);
      r.setLeft(r.left() + fm.size(Qt::TextSingleLine, first).width());

      if (display_hardcore && !display_hardcore_only)
      {
        const QString hc = QStringLiteral("(%2)").arg(num_unlocked_hardcore);
        painter->setPen(hc_color);
        painter->drawText(r, Qt::AlignVCenter, hc);
        r.setLeft(r.left() + fm.size(Qt::TextSingleLine, hc).width());
      }

      painter->setPen(text_color);
      painter->drawText(r, Qt::AlignVCenter, total);
    }
    else
    {
      painter->setPen(text_color);
      painter->drawText(r, Qt::AlignVCenter, QStringLiteral("N/A"));
    }
  }

private:
  GameListModel* m_model;
  GameListSortModel* m_sort_model;
};

} // namespace

GameListWidget::GameListWidget(QWidget* parent /* = nullptr */) : QWidget(parent)
{
}

GameListWidget::~GameListWidget() = default;

void GameListWidget::initialize()
{
  const float cover_scale = Host::GetBaseFloatSettingValue("UI", "GameListCoverArtScale", 0.45f);
  const bool show_cover_titles = Host::GetBaseBoolSettingValue("UI", "GameListShowCoverTitles", true);
  const bool merge_disc_sets = Host::GetBaseBoolSettingValue("UI", "GameListMergeDiscSets", true);
  const bool show_game_icons = Host::GetBaseBoolSettingValue("UI", "GameListShowGameIcons", true);
  m_model = new GameListModel(cover_scale, show_cover_titles, show_game_icons, this);
  m_model->updateCacheSize(width(), height());

  m_sort_model = new GameListSortModel(m_model);
  m_sort_model->setSourceModel(m_model);
  m_sort_model->setMergeDiscSets(merge_disc_sets);

  m_ui.setupUi(this);
  for (u32 type = 0; type < static_cast<u32>(GameList::EntryType::MaxCount); type++)
  {
    m_ui.filterType->addItem(
      QtUtils::GetIconForEntryType(static_cast<GameList::EntryType>(type)),
      qApp->translate("GameList", GameList::GetEntryTypeDisplayName(static_cast<GameList::EntryType>(type))));
  }
  for (u32 region = 0; region < static_cast<u32>(DiscRegion::Count); region++)
  {
    m_ui.filterRegion->addItem(QtUtils::GetIconForRegion(static_cast<DiscRegion>(region)),
                               QString::fromUtf8(Settings::GetDiscRegionName(static_cast<DiscRegion>(region))));
  }

  connect(m_ui.viewGameList, &QPushButton::clicked, this, &GameListWidget::showGameList);
  connect(m_ui.viewGameGrid, &QPushButton::clicked, this, &GameListWidget::showGameGrid);
  connect(m_ui.gridScale, &QSlider::valueChanged, this, &GameListWidget::gridIntScale);
  connect(m_ui.viewGridTitles, &QPushButton::toggled, this, &GameListWidget::setShowCoverTitles);
  connect(m_ui.viewMergeDiscSets, &QPushButton::toggled, this, &GameListWidget::setMergeDiscSets);
  connect(m_ui.filterType, &QComboBox::currentIndexChanged, this, [this](int index) {
    m_sort_model->setFilterType((index == 0) ? GameList::EntryType::MaxCount :
                                               static_cast<GameList::EntryType>(index - 1));
  });
  connect(m_ui.filterRegion, &QComboBox::currentIndexChanged, this, [this](int index) {
    m_sort_model->setFilterRegion((index == 0) ? DiscRegion::Count : static_cast<DiscRegion>(index - 1));
  });
  connect(m_ui.searchText, &QLineEdit::textChanged, this,
          [this](const QString& text) { m_sort_model->setFilterName(text); });
  connect(m_ui.searchText, &QLineEdit::returnPressed, this, &GameListWidget::onSearchReturnPressed);

  GameListCenterIconStyleDelegate* center_icon_delegate = new GameListCenterIconStyleDelegate(this);
  m_table_view = new QTableView(m_ui.stack);
  m_table_view->setModel(m_sort_model);
  m_table_view->setSortingEnabled(true);
  m_table_view->setSelectionMode(QAbstractItemView::SingleSelection);
  m_table_view->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table_view->setContextMenuPolicy(Qt::CustomContextMenu);
  m_table_view->setAlternatingRowColors(true);
  m_table_view->setShowGrid(false);
  m_table_view->setCurrentIndex({});
  m_table_view->horizontalHeader()->setHighlightSections(false);
  m_table_view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
  m_table_view->verticalHeader()->hide();
  m_table_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  m_table_view->setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);
  m_table_view->setItemDelegateForColumn(GameListModel::Column_Icon, center_icon_delegate);
  m_table_view->setItemDelegateForColumn(GameListModel::Column_Region, center_icon_delegate);
  m_table_view->setItemDelegateForColumn(GameListModel::Column_Achievements,
                                         new GameListAchievementsStyleDelegate(this, m_model, m_sort_model));

  loadTableViewColumnVisibilitySettings();
  loadTableViewColumnSortSettings();

  connect(m_table_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &GameListWidget::onSelectionModelCurrentChanged);
  connect(m_table_view, &QTableView::activated, this, &GameListWidget::onTableViewItemActivated);
  connect(m_table_view, &QTableView::customContextMenuRequested, this,
          &GameListWidget::onTableViewContextMenuRequested);
  connect(m_table_view->horizontalHeader(), &QHeaderView::customContextMenuRequested, this,
          &GameListWidget::onTableViewHeaderContextMenuRequested);
  connect(m_table_view->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
          &GameListWidget::onTableViewHeaderSortIndicatorChanged);

  m_ui.stack->insertWidget(0, m_table_view);

  m_list_view = new GameListGridListView(m_ui.stack);
  m_list_view->setModel(m_sort_model);
  m_list_view->setModelColumn(GameListModel::Column_Cover);
  m_list_view->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list_view->setViewMode(QListView::IconMode);
  m_list_view->setResizeMode(QListView::Adjust);
  m_list_view->setUniformItemSizes(true);
  m_list_view->setItemAlignment(Qt::AlignHCenter);
  m_list_view->setContextMenuPolicy(Qt::CustomContextMenu);
  m_list_view->setFrameStyle(QFrame::NoFrame);
  m_list_view->setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);
  m_list_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  m_list_view->verticalScrollBar()->setSingleStep(15);

  onCoverScaleChanged();

  connect(m_list_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &GameListWidget::onSelectionModelCurrentChanged);
  connect(m_list_view, &GameListGridListView::zoomIn, this, &GameListWidget::gridZoomIn);
  connect(m_list_view, &GameListGridListView::zoomOut, this, &GameListWidget::gridZoomOut);
  connect(m_list_view, &QListView::activated, this, &GameListWidget::onListViewItemActivated);
  connect(m_list_view, &QListView::customContextMenuRequested, this, &GameListWidget::onListViewContextMenuRequested);
  connect(m_model, &GameListModel::coverScaleChanged, this, &GameListWidget::onCoverScaleChanged);

  m_ui.stack->insertWidget(1, m_list_view);

  m_empty_widget = new QWidget(m_ui.stack);
  m_empty_ui.setupUi(m_empty_widget);
  m_empty_ui.supportedFormats->setText(qApp->translate("GameListWidget", SUPPORTED_FORMATS_STRING));
  connect(m_empty_ui.addGameDirectory, &QPushButton::clicked, this, [this]() { emit addGameDirectoryRequested(); });
  connect(m_empty_ui.scanForNewGames, &QPushButton::clicked, this, [this]() { refresh(false); });
  m_ui.stack->insertWidget(2, m_empty_widget);

  const bool grid_view = Host::GetBaseBoolSettingValue("UI", "GameListGridView", false);
  if (grid_view)
    m_ui.stack->setCurrentIndex(1);
  else
    m_ui.stack->setCurrentIndex(0);
  setFocusProxy(grid_view ? static_cast<QWidget*>(m_list_view) : static_cast<QWidget*>(m_table_view));

  updateToolbar();
  resizeTableViewColumnsToFit();
  updateBackground(true);
}

bool GameListWidget::isShowingGameList() const
{
  return m_ui.stack->currentIndex() == 0;
}

bool GameListWidget::isShowingGameGrid() const
{
  return m_ui.stack->currentIndex() == 1;
}

bool GameListWidget::isShowingGridCoverTitles() const
{
  return m_model->getShowCoverTitles();
}

bool GameListWidget::isMergingDiscSets() const
{
  return m_sort_model->isMergingDiscSets();
}

bool GameListWidget::isShowingGameIcons() const
{
  return m_model->getShowGameIcons();
}

void GameListWidget::refresh(bool invalidate_cache)
{
  cancelRefresh();

  if (!invalidate_cache)
    m_model->takeGameList();

  m_refresh_thread = new GameListRefreshThread(invalidate_cache);
  connect(m_refresh_thread, &GameListRefreshThread::refreshProgress, this, &GameListWidget::onRefreshProgress,
          Qt::QueuedConnection);
  connect(m_refresh_thread, &GameListRefreshThread::refreshComplete, this, &GameListWidget::onRefreshComplete,
          Qt::QueuedConnection);
  m_refresh_thread->start();
}

void GameListWidget::refreshModel()
{
  m_model->refresh();
}

void GameListWidget::cancelRefresh()
{
  if (!m_refresh_thread)
    return;

  m_refresh_thread->cancel();
  m_refresh_thread->wait();
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  AssertMsg(!m_refresh_thread, "Game list thread should be unreferenced by now");
}

void GameListWidget::reloadThemeSpecificImages()
{
  m_model->reloadThemeSpecificImages();
}

void GameListWidget::updateBackground(bool reload_image)
{
  std::string path = Host::GetBaseStringSettingValue("UI", "GameListBackgroundPath");
  if (!Path::IsAbsolute(path))
    path = Path::Combine(EmuFolders::DataRoot, path);

  if (reload_image)
  {
    m_background_image = QImage();
    if (!path.empty() && m_background_image.load(path.c_str()))
      m_background_image.setDevicePixelRatio(devicePixelRatio());
  }

  if (m_background_image.isNull())
  {
    m_ui.stack->setPalette(palette());
    m_table_view->setAlternatingRowColors(true);
    return;
  }

  QtAsyncTask::create(this, [image = m_background_image, this, widget_width = m_ui.stack->width(),
                             widget_height = m_ui.stack->height()]() mutable {
    resizeAndPadImage(&image, widget_width, widget_height, true);
    return [image = std::move(image), this, widget_width, widget_height]() {
      // check for dimensions change
      if (widget_width != m_ui.stack->width() || widget_height != m_ui.stack->height())
        return;

      QPalette new_palette(m_ui.stack->palette());
      new_palette.setBrush(QPalette::Base, QPixmap::fromImage(image));
      m_ui.stack->setPalette(new_palette);
      m_table_view->setAlternatingRowColors(false);
    };
  });
}

void GameListWidget::onRefreshProgress(const QString& status, int current, int total, float time)
{
  // Avoid spamming the UI on very short refresh (e.g. game exit).
  static constexpr float SHORT_REFRESH_TIME = 0.5f;
  if (!m_model->hasTakenGameList())
    m_model->refresh();

  // switch away from the placeholder while we scan, in case we find anything
  if (m_ui.stack->currentIndex() == 2)
  {
    const bool grid_view = Host::GetBaseBoolSettingValue("UI", "GameListGridView", false);
    m_ui.stack->setCurrentIndex(grid_view ? 1 : 0);
    setFocusProxy(grid_view ? static_cast<QWidget*>(m_list_view) : static_cast<QWidget*>(m_table_view));
  }

  if (!m_model->hasTakenGameList() || time >= SHORT_REFRESH_TIME)
    emit refreshProgress(status, current, total);
}

void GameListWidget::onRefreshComplete()
{
  m_model->refresh();
  emit refreshComplete();

  AssertMsg(m_refresh_thread, "Has a refresh thread");
  m_refresh_thread->wait();
  delete m_refresh_thread;
  m_refresh_thread = nullptr;

  // if we still had no games, switch to the helper widget
  if (m_model->rowCount() == 0)
  {
    m_ui.stack->setCurrentIndex(2);
    setFocusProxy(nullptr);
  }
}

void GameListWidget::onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous)
{
  const QModelIndex source_index = m_sort_model->mapToSource(current);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
    return;

  emit selectionChanged();
}

void GameListWidget::onTableViewItemActivated(const QModelIndex& index)
{
  const QModelIndex source_index = m_sort_model->mapToSource(index);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
    return;

  if (qApp->keyboardModifiers().testFlag(Qt::AltModifier))
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = GameList::GetEntryByIndex(static_cast<u32>(source_index.row()));
    if (entry)
      SettingsWindow::openGamePropertiesDialog(entry->path, entry->title, entry->serial, entry->hash, entry->region);
  }
  else
  {
    emit entryActivated();
  }
}

void GameListWidget::onTableViewContextMenuRequested(const QPoint& point)
{
  emit entryContextMenuRequested(m_table_view->mapToGlobal(point));
}

void GameListWidget::onListViewItemActivated(const QModelIndex& index)
{
  const QModelIndex source_index = m_sort_model->mapToSource(index);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
    return;

  emit entryActivated();
}

void GameListWidget::onListViewContextMenuRequested(const QPoint& point)
{
  emit entryContextMenuRequested(m_list_view->mapToGlobal(point));
}

void GameListWidget::onTableViewHeaderContextMenuRequested(const QPoint& point)
{
  QMenu menu;

  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    if (column == GameListModel::Column_Cover)
      continue;

    QAction* action = menu.addAction(m_model->getColumnDisplayName(column));
    action->setCheckable(true);
    action->setChecked(!m_table_view->isColumnHidden(column));
    connect(action, &QAction::toggled, [this, column](bool enabled) {
      m_table_view->setColumnHidden(column, !enabled);
      saveTableViewColumnVisibilitySettings(column);
      resizeTableViewColumnsToFit();
    });
  }

  menu.exec(m_table_view->mapToGlobal(point));
}

void GameListWidget::onTableViewHeaderSortIndicatorChanged(int, Qt::SortOrder)
{
  saveTableViewColumnSortSettings();
}

void GameListWidget::onCoverScaleChanged()
{
  m_model->updateCacheSize(width(), height());

  m_list_view->setSpacing(m_model->getCoverArtSpacing());
  m_list_view->setIconSize(QSize(m_model->getCoverArtWidth(), m_model->getCoverArtHeight()));

  QFont font;
  font.setPointSizeF(20.0f * m_model->getCoverScale());
  m_list_view->setFont(font);

  m_list_view->updateLayout();
}

void GameListWidget::listZoom(float delta)
{
  const float new_scale = std::clamp(m_model->getCoverScale() + delta, MIN_SCALE, MAX_SCALE);
  Host::SetBaseFloatSettingValue("UI", "GameListCoverArtScale", new_scale);
  Host::CommitBaseSettingChanges();
  m_model->setCoverScale(new_scale);
  updateToolbar();

  m_model->refresh();
}

void GameListWidget::gridZoomIn()
{
  listZoom(0.05f);
}

void GameListWidget::gridZoomOut()
{
  listZoom(-0.05f);
}

void GameListWidget::gridIntScale(int int_scale)
{
  const float new_scale = std::clamp(static_cast<float>(int_scale) / 100.0f, MIN_SCALE, MAX_SCALE);

  Host::SetBaseFloatSettingValue("UI", "GameListCoverArtScale", new_scale);
  Host::CommitBaseSettingChanges();
  m_model->setCoverScale(new_scale);
  updateToolbar();

  m_model->refresh();
}

void GameListWidget::refreshGridCovers()
{
  m_model->refreshCovers();
  Host::RunOnCPUThread(&FullscreenUI::InvalidateCoverCache);
}

void GameListWidget::focusSearchWidget()
{
  m_ui.searchText->setFocus(Qt::ShortcutFocusReason);
}

void GameListWidget::onSearchReturnPressed()
{
  // Anything to switch focus to?
  const int rows = m_sort_model->rowCount();
  if (rows == 0)
    return;

  QAbstractItemView* const target =
    isShowingGameGrid() ? static_cast<QAbstractItemView*>(m_list_view) : static_cast<QAbstractItemView*>(m_table_view);
  target->selectionModel()->select(m_sort_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  target->setFocus(Qt::ShortcutFocusReason);
}

void GameListWidget::showGameList()
{
  if (m_ui.stack->currentIndex() == 0 || m_model->rowCount() == 0)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListGridView", false);
  Host::CommitBaseSettingChanges();
  m_ui.stack->setCurrentIndex(0);
  setFocusProxy(m_table_view);
  resizeTableViewColumnsToFit();
  updateToolbar();
  emit layoutChanged();
}

void GameListWidget::showGameGrid()
{
  if (m_ui.stack->currentIndex() == 1 || m_model->rowCount() == 0)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListGridView", true);
  Host::CommitBaseSettingChanges();
  m_ui.stack->setCurrentIndex(1);
  setFocusProxy(m_list_view);
  updateToolbar();
  emit layoutChanged();
}

void GameListWidget::setShowCoverTitles(bool enabled)
{
  if (m_model->getShowCoverTitles() == enabled)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListShowCoverTitles", enabled);
  Host::CommitBaseSettingChanges();
  m_model->setShowCoverTitles(enabled);
  if (isShowingGameGrid())
    m_model->refresh();
  updateToolbar();
  emit layoutChanged();
}

void GameListWidget::setMergeDiscSets(bool enabled)
{
  if (m_sort_model->isMergingDiscSets() == enabled)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListMergeDiscSets", enabled);
  Host::CommitBaseSettingChanges();
  m_sort_model->setMergeDiscSets(enabled);
  updateToolbar();
  emit layoutChanged();
}

void GameListWidget::setShowGameIcons(bool enabled)
{
  if (m_model->getShowGameIcons() == enabled)
    return;

  Host::SetBaseBoolSettingValue("UI", "GameListShowGameIcons", enabled);
  Host::CommitBaseSettingChanges();
  m_model->setShowGameIcons(enabled);
}

void GameListWidget::updateToolbar()
{
  const bool grid_view = isShowingGameGrid();
  {
    QSignalBlocker sb(m_ui.viewGameGrid);
    m_ui.viewGameGrid->setChecked(grid_view);
  }
  {
    QSignalBlocker sb(m_ui.viewGameList);
    m_ui.viewGameList->setChecked(!grid_view);
  }
  {
    QSignalBlocker sb(m_ui.viewGridTitles);
    m_ui.viewGridTitles->setChecked(m_model->getShowCoverTitles());
  }
  {
    QSignalBlocker sb(m_ui.viewMergeDiscSets);
    m_ui.viewMergeDiscSets->setChecked(m_sort_model->isMergingDiscSets());
  }
  {
    QSignalBlocker sb(m_ui.gridScale);
    m_ui.gridScale->setValue(static_cast<int>(m_model->getCoverScale() * 100.0f));
  }

  m_ui.viewGridTitles->setEnabled(grid_view);
  m_ui.gridScale->setEnabled(grid_view);
}

void GameListWidget::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  resizeTableViewColumnsToFit();
  updateBackground(false);
}

void GameListWidget::resizeTableViewColumnsToFit()
{
  QtUtils::ResizeColumnsForTableView(m_table_view, {
                                                     45,  // type
                                                     80,  // code
                                                     -1,  // title
                                                     -1,  // file title
                                                     200, // developer
                                                     200, // publisher
                                                     200, // genre
                                                     50,  // year
                                                     100, // players
                                                     85,  // time played
                                                     85,  // last played
                                                     80,  // file size
                                                     80,  // size
                                                     55,  // region
                                                     100, // achievements
                                                     100  // compatibility
                                                   });
}

static TinyString getColumnVisibilitySettingsKeyName(int column)
{
  return TinyString::from_format("Show{}", GameListModel::getColumnName(static_cast<GameListModel::Column>(column)));
}

void GameListWidget::loadTableViewColumnVisibilitySettings()
{
  static constexpr std::array<bool, GameListModel::Column_Count> DEFAULT_VISIBILITY = {{
    true,  // type
    true,  // code
    true,  // title
    false, // file title
    false, // developer
    false, // publisher
    false, // genre
    false, // year
    false, // players
    true,  // time played
    true,  // last played
    true,  // file size
    false, // size
    true,  // region
    false, // achievements
    false  // compatibility
  }};

  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    const bool visible = Host::GetBaseBoolSettingValue("GameListTableView", getColumnVisibilitySettingsKeyName(column),
                                                       DEFAULT_VISIBILITY[column]);
    m_table_view->setColumnHidden(column, !visible);
  }
}

void GameListWidget::saveTableViewColumnVisibilitySettings()
{
  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    const bool visible = !m_table_view->isColumnHidden(column);
    Host::SetBaseBoolSettingValue("GameListTableView", getColumnVisibilitySettingsKeyName(column), visible);
    Host::CommitBaseSettingChanges();
  }
}

void GameListWidget::saveTableViewColumnVisibilitySettings(int column)
{
  const bool visible = !m_table_view->isColumnHidden(column);
  Host::SetBaseBoolSettingValue("GameListTableView", getColumnVisibilitySettingsKeyName(column), visible);
  Host::CommitBaseSettingChanges();
}

void GameListWidget::loadTableViewColumnSortSettings()
{
  const GameListModel::Column DEFAULT_SORT_COLUMN = GameListModel::Column_Icon;
  const bool DEFAULT_SORT_DESCENDING = false;

  const GameListModel::Column sort_column =
    GameListModel::getColumnIdForName(Host::GetBaseStringSettingValue("GameListTableView", "SortColumn"))
      .value_or(DEFAULT_SORT_COLUMN);
  const bool sort_descending =
    Host::GetBaseBoolSettingValue("GameListTableView", "SortDescending", DEFAULT_SORT_DESCENDING);
  const Qt::SortOrder sort_order = sort_descending ? Qt::DescendingOrder : Qt::AscendingOrder;
  m_sort_model->sort(sort_column, sort_order);
  if (QHeaderView* hv = m_table_view->horizontalHeader())
    hv->setSortIndicator(sort_column, sort_order);
}

void GameListWidget::saveTableViewColumnSortSettings()
{
  const int sort_column = m_table_view->horizontalHeader()->sortIndicatorSection();
  const bool sort_descending = (m_table_view->horizontalHeader()->sortIndicatorOrder() == Qt::DescendingOrder);

  if (sort_column >= 0 && sort_column < GameListModel::Column_Count)
  {
    Host::SetBaseStringSettingValue("GameListTableView", "SortColumn",
                                    GameListModel::getColumnName(static_cast<GameListModel::Column>(sort_column)));
  }

  Host::SetBaseBoolSettingValue("GameListTableView", "SortDescending", sort_descending);
  Host::CommitBaseSettingChanges();
}

void GameListWidget::setTableViewColumnHidden(int column, bool hidden)
{
  DebugAssert(column < GameListModel::Column_Count);
  if (m_table_view->isColumnHidden(column) == hidden)
    return;

  m_table_view->setColumnHidden(column, hidden);
  saveTableViewColumnVisibilitySettings(column);
  resizeTableViewColumnsToFit();
}

const GameList::Entry* GameListWidget::getSelectedEntry() const
{
  if (m_ui.stack->currentIndex() == 0)
  {
    const QItemSelectionModel* selection_model = m_table_view->selectionModel();
    if (!selection_model->hasSelection())
      return nullptr;

    const QModelIndexList selected_rows = selection_model->selectedRows();
    if (selected_rows.empty())
      return nullptr;

    const QModelIndex source_index = m_sort_model->mapToSource(selected_rows[0]);
    if (!source_index.isValid())
      return nullptr;

    return GameList::GetEntryByIndex(source_index.row());
  }
  else
  {
    const QItemSelectionModel* selection_model = m_list_view->selectionModel();
    if (!selection_model->hasSelection())
      return nullptr;

    const QModelIndex source_index = m_sort_model->mapToSource(selection_model->currentIndex());
    if (!source_index.isValid())
      return nullptr;

    return GameList::GetEntryByIndex(source_index.row());
  }
}

GameListGridListView::GameListGridListView(QWidget* parent /*= nullptr*/) : QListView(parent)
{
}

void GameListGridListView::wheelEvent(QWheelEvent* e)
{
  if (e->modifiers() & Qt::ControlModifier)
  {
    int dy = e->angleDelta().y();
    if (dy != 0)
    {
      if (dy < 0)
        zoomOut();
      else
        zoomIn();

      return;
    }
  }

  QListView::wheelEvent(e);
}

void GameListGridListView::resizeEvent(QResizeEvent* e)
{
  updateLayout();
  QListView::resizeEvent(e);
}

void GameListGridListView::updateLayout()
{
  const int scrollbar_width = verticalScrollBar()->width();
  const int item_spacing = spacing();
  const int icon_margin = style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, this) * 2;

  // I hate this +2. Not sure what's not being accounted for, but without it the calculation is off by 2 pixels...
  const int icon_width = iconSize().width() + icon_margin + item_spacing + 2;
  const int available_width = width() - scrollbar_width - item_spacing - icon_margin;
  const int items_per_row = available_width / icon_width;
  const int margin = (available_width - (items_per_row * icon_width)) / 2;

  m_horizontal_offset = margin;
}

int GameListGridListView::horizontalOffset() const
{
  return QListView::horizontalOffset() - m_horizontal_offset;
}
