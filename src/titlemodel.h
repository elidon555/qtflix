#pragma once
#include <QAbstractListModel>
#include <qqml.h>
#include <QDateTime>
#include <QStringList>
#include <QVector>

// One playable file (a movie, or one episode of a series).
struct MediaFile {
    QString path;          // absolute path
    int season = 0;        // 0 for movies
    int episode = 0;       // 0 for movies
    QString episodeTitle;  // parsed from filename after SxxEyy, may be empty
    qint64 durationMs = 0; // from ffprobe (0 if unknown yet)
    int width = 0;
    int height = 0;
    QString videoCodec;    // e.g. "hevc"
    QDateTime modified;
};

// One card on the UI: a movie, or a whole series (grouped episodes).
struct Title {
    QString id;            // stable: sha1 of (series key or movie path), hex
    QString title;         // "Person of Interest"
    int year = 0;
    bool isSeries = false;
    QString category;      // name of the top-level folder the title lives in ("Videos", "Downloads", "Movies"...)
    QString description;   // generated text
    QString rating;        // "TV-MA", "PG-13", "R", "TV-14" (deterministic from id, cosmetic)
    QStringList genres;    // cosmetic tags derived from name/codec/folder (2-3 entries)
    int match = 95;        // 90..99 cosmetic "97% Match"
    QString posterFile;    // explicit artwork found next to file (poster.jpg/folder.jpg/<name>.jpg) else empty
    QString backdropFile;  // fanart.jpg/backdrop.jpg/<name>-fanart.jpg else empty
    QString logoFile;      // title logo PNG (transparent) from external metadata, else empty
    bool hasExternalMeta = false; // true once setExternalMetadata() was applied
    QDateTime added;       // newest file mtime
    QVector<MediaFile> files; // movies: exactly 1; series: all episodes sorted by (season, episode)
};

class Library;

class TitleModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        TitleRole,         // QString
        YearRole,          // int
        PathRole,          // QString: movie file, or first episode (or resume episode if any progress)
        SourceUrlRole,     // QUrl file:// of PathRole
        IsSeriesRole,      // bool
        SeasonCountRole,   // int
        EpisodeCountRole,  // int
        DurationMsRole,    // qint64 of PathRole file
        QualityRole,       // "4K" | "HD" | ""  (height >= 2000 -> 4K, >= 700 -> HD)
        CardImageRole,     // QUrl image://thumbs/<id>/card      (portrait 2:3 artwork)
        BackdropImageRole, // QUrl image://thumbs/<id>/backdrop  (16:9 artwork)
        PositionMsRole,    // qint64 last watched pos of PathRole file
        ProgressRole,      // double 0..1 of PathRole file
        InMyListRole,      // bool
        AddedRole,         // QDateTime
        CategoryRole,      // QString
        DescriptionRole,   // QString
        RatingRole,        // QString
        GenresRole,        // QStringList
        MatchRole,         // int
        RankRole,          // int 1-based row index (used by Top 10 row)
        LogoImageRole,     // QUrl file:// of Title::logoFile, or empty QUrl when none
        HasMetaRole        // bool Title::hasExternalMeta
    };
    Q_ENUM(Roles)

    explicit TitleModel(Library *lib, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replace contents with these title ids (looked up in the Library).
    void setIds(const QStringList &ids);
    QStringList ids() const { return m_ids; }
    // Re-emit dataChanged for a title (progress / my list changed).
    void refresh(const QString &id);
    void refreshAll();

    Q_INVOKABLE QVariantMap get(int row) const; // all roles as a map, keys == roleNames

signals:
    void countChanged();

private:
    Library *m_lib;
    QStringList m_ids;
};

// Row of the home page.
class RowsModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
public:
    enum Roles { NameRole = Qt::UserRole + 1, KindRole, ModelRole };
    struct Row { QString name; QString kind; TitleModel *model; }; // kind: "continue" | "mylist" | "top10" | "normal"
    explicit RowsModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex & = QModelIndex()) const override { return m_rows.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override
    { return {{NameRole, "name"}, {KindRole, "kind"}, {ModelRole, "model"}}; }
    void setRows(const QVector<Row> &rows);
    const QVector<Row> &rows() const { return m_rows; }
private:
    QVector<Row> m_rows;
};
