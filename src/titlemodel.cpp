#include "titlemodel.h"
#include "library.h"

#include <QSet>

// ---------------------------------------------------------------- TitleModel

TitleModel::TitleModel(Library *lib, QObject *parent)
    : QAbstractListModel(parent), m_lib(lib)
{
}

int TitleModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_ids.size());
}

QVariant TitleModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_ids.size())
        return {};
    const Title *t = m_lib->findTitle(m_ids.at(index.row()));
    if (!t)
        return {};
    return m_lib->roleData(*t, role, index.row() + 1);
}

QHash<int, QByteArray> TitleModel::roleNames() const
{
    static const QHash<int, QByteArray> names = {
        {IdRole, "id"},
        {TitleRole, "title"},
        {YearRole, "year"},
        {PathRole, "path"},
        {SourceUrlRole, "sourceUrl"},
        {IsSeriesRole, "isSeries"},
        {SeasonCountRole, "seasonCount"},
        {EpisodeCountRole, "episodeCount"},
        {DurationMsRole, "durationMs"},
        {QualityRole, "quality"},
        {CardImageRole, "cardImage"},
        {BackdropImageRole, "backdropImage"},
        {PositionMsRole, "positionMs"},
        {ProgressRole, "progress"},
        {InMyListRole, "inMyList"},
        {AddedRole, "added"},
        {CategoryRole, "category"},
        {DescriptionRole, "description"},
        {RatingRole, "rating"},
        {GenresRole, "genres"},
        {MatchRole, "match"},
        {RankRole, "rank"},
        {LogoImageRole, "logoImage"},
        {HasMetaRole, "hasMeta"},
        {IsRecentRole, "isRecent"},
    };
    return names;
}

namespace {
// Fenwick tree over 0/1 flags: how many of the first k old rows are still waiting to be placed.
struct Fenwick {
    QVector<int> t;
    explicit Fenwick(int n) : t(n + 1, 0)
    {
        for (int i = 1; i <= n; ++i) { // all ones, built in O(n)
            t[i] += 1;
            const int j = i + (i & -i);
            if (j <= n) t[j] += t[i];
        }
    }
    void clear(int k) { for (int i = k + 1; i < t.size(); i += i & -i) --t[i]; }
    int prefix(int k) const { int s = 0; for (int i = k; i > 0; i -= i & -i) s += t[i]; return s; } // flags [0, k)
};
} // namespace

// Applies the new id list as row removals / moves / insertions instead of a model reset, so views keep
// their delegates and scroll positions during background rescans (identical lists are a no-op).
void TitleModel::setIds(const QStringList &ids)
{
    if (ids == m_ids)
        return;
    const auto oldCount = m_ids.size();
    const QSet<QString> target(ids.cbegin(), ids.cend());
    const QSet<QString> current(m_ids.cbegin(), m_ids.cend());
    if (target.size() != ids.size() || current.size() != m_ids.size()) { // duplicates: no sane diff
        beginResetModel();
        m_ids = ids;
        endResetModel();
    } else if (ids.size() > m_ids.size() && std::equal(m_ids.cbegin(), m_ids.cend(), ids.cbegin())) {
        beginInsertRows(QModelIndex(), int(m_ids.size()), int(ids.size()) - 1); // pure append
        m_ids = ids;
        endInsertRows();
    } else {
        // 1) remove rows that are gone (contiguous runs, back to front)
        for (int i = int(m_ids.size()) - 1; i >= 0; --i) {
            if (target.contains(m_ids.at(i)))
                continue;
            int first = i;
            while (first > 0 && !target.contains(m_ids.at(first - 1)))
                --first;
            beginRemoveRows(QModelIndex(), first, i);
            m_ids.remove(first, i - first + 1);
            endRemoveRows();
            i = first;
        }
        // 2) walk the target order: move rows that exist further down, insert new ones. Rows [0, i) are final;
        //    the old rows not placed yet follow in their old relative order, so an old row's current position is
        //    i + (number of unplaced old rows before it) -- a Fenwick query instead of an indexOf() per move.
        QHash<QString, int> oldIndex;
        oldIndex.reserve(m_ids.size());
        for (int k = 0; k < m_ids.size(); ++k)
            oldIndex.insert(m_ids.at(k), k);
        Fenwick waiting(int(m_ids.size()));
        for (int i = 0; i < ids.size(); ++i) {
            const auto old = oldIndex.constFind(ids.at(i));
            if (old != oldIndex.cend()) {
                const int j = i + waiting.prefix(old.value());
                waiting.clear(old.value());
                if (j == i)
                    continue;
                beginMoveRows(QModelIndex(), j, j, QModelIndex(), i);
                m_ids.move(j, i);
                endMoveRows();
            } else {
                int last = i; // insert a run of new ids in one go
                while (last + 1 < ids.size() && !oldIndex.contains(ids.at(last + 1)))
                    ++last;
                beginInsertRows(QModelIndex(), i, last);
                for (int k = i; k <= last; ++k)
                    m_ids.insert(k, ids.at(k));
                endInsertRows();
                i = last;
            }
        }
        Q_ASSERT(m_ids == ids);
        if (m_ids != ids) { // should not happen; never leave the view inconsistent
            beginResetModel();
            m_ids = ids;
            endResetModel();
        }
    }
    reindex();
    if (!m_ids.isEmpty()) // RankRole is positional
        emit dataChanged(index(0), index(int(m_ids.size()) - 1), {RankRole});
    if (oldCount != m_ids.size())
        emit countChanged();
}

void TitleModel::reindex()
{
    m_rowOf.clear();
    m_rowOf.reserve(m_ids.size());
    for (int i = 0; i < m_ids.size(); ++i)
        m_rowOf.insert(m_ids.at(i), i);
    if (m_rowOf.size() != m_ids.size())
        m_rowOf.clear(); // duplicates: refresh() falls back to a linear search
}

void TitleModel::refresh(const QString &id, const QList<int> &roles)
{
    if (m_ids.isEmpty())
        return;
    if (!m_rowOf.isEmpty()) {
        const auto it = m_rowOf.constFind(id);
        if (it != m_rowOf.cend()) {
            const QModelIndex idx = index(it.value());
            emit dataChanged(idx, idx, roles);
        }
        return;
    }
    for (int i = 0; i < m_ids.size(); ++i) {
        if (m_ids.at(i) == id) {
            const QModelIndex idx = index(i);
            emit dataChanged(idx, idx, roles);
        }
    }
}

void TitleModel::refreshAll(const QList<int> &roles)
{
    if (m_ids.isEmpty())
        return;
    emit dataChanged(index(0), index(int(m_ids.size()) - 1), roles);
}

QVariantMap TitleModel::get(int row) const
{
    if (row < 0 || row >= m_ids.size())
        return {};
    const Title *t = m_lib->findTitle(m_ids.at(row));
    if (!t)
        return {};
    return m_lib->titleToMap(*t, row + 1);
}

// ---------------------------------------------------------------- RowsModel

QVariant RowsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &r = m_rows.at(index.row());
    switch (role) {
    case NameRole: return r.name;
    case KindRole: return r.kind;
    case ModelRole: return QVariant::fromValue(static_cast<QObject *>(r.model));
    default: return {};
    }
}

// Row TitleModels are owned by this RowsModel (parented to it) unless they already have a parent
// (e.g. Library::continueWatching reused as the home "Continue Watching" row). They are never deleted
// while the RowsModel lives: Library reuses one TitleModel per (kind, name) so a QML delegate never
// holds a dangling model pointer. Structural changes are applied as row inserts/removals when possible
// so the home page keeps its delegates (and scroll positions) when e.g. "Continue Watching" appears.
void RowsModel::setRows(const QVector<Row> &rows)
{
    for (const Row &r : rows)
        if (r.model && !r.model->parent())
            r.model->setParent(this);

    auto same = [](const Row &a, const Row &b) {
        return a.model == b.model && a.name == b.name && a.kind == b.kind;
    };
    auto contains = [&](const QVector<Row> &list, const Row &r, int from = 0) {
        for (int i = from; i < list.size(); ++i)
            if (same(list.at(i), r))
                return true;
        return false;
    };

    if (rows.size() == m_rows.size()) {
        bool identical = true;
        for (int i = 0; i < rows.size() && identical; ++i)
            identical = same(rows.at(i), m_rows.at(i));
        if (identical)
            return;
    }

    // 1) remove rows that are gone
    for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
        if (!contains(rows, m_rows.at(i))) {
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.removeAt(i);
            endRemoveRows();
        }
    }
    // 2) insert new rows in place; fall back to a reset if the order of kept rows changed
    for (int i = 0; i < rows.size(); ++i) {
        if (i < m_rows.size() && same(m_rows.at(i), rows.at(i)))
            continue;
        if (contains(m_rows, rows.at(i), i)) {
            beginResetModel();
            m_rows = rows;
            endResetModel();
            return;
        }
        beginInsertRows(QModelIndex(), i, i);
        m_rows.insert(i, rows.at(i));
        endInsertRows();
    }
    if (m_rows.size() != rows.size()) { // duplicates or other oddities
        beginResetModel();
        m_rows = rows;
        endResetModel();
    }
}
