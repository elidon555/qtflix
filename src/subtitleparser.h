#pragma once
#include <QObject>
#include <QVector>
#include <QUrl>
#include <qqml.h>

// External subtitle file loader for the player (QML type `SubtitleTrack` in module QtFlix).
// Supports .srt, .vtt and basic .ass/.ssa (Dialogue lines, tags stripped). Encoding: UTF-8 with
// fallback to Latin-1.
//
//   SubtitleTrack { id: subs; source: "file:///.../movie.en.srt"; positionMs: player.position }
//   Text { text: subs.currentText }
class SubtitleTrack : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs WRITE setPositionMs NOTIFY positionMsChanged)
    Q_PROPERTY(qint64 offsetMs READ offsetMs WRITE setOffsetMs NOTIFY offsetMsChanged)
    Q_PROPERTY(QString currentText READ currentText NOTIFY currentTextChanged)  // may contain <i>/<b> (Text.RichText safe)
    Q_PROPERTY(int count READ count NOTIFY sourceChanged)
    Q_PROPERTY(bool valid READ valid NOTIFY sourceChanged)
public:
    struct Cue { qint64 start; qint64 end; QString text; };
    explicit SubtitleTrack(QObject *parent = nullptr);
    QUrl source() const { return m_source; }
    void setSource(const QUrl &u);
    qint64 positionMs() const { return m_pos; }
    void setPositionMs(qint64 ms);
    qint64 offsetMs() const { return m_offset; }
    void setOffsetMs(qint64 ms);
    QString currentText() const { return m_current; }
    int count() const { return m_cues.size(); }
    bool valid() const { return !m_cues.isEmpty(); }
signals:
    void sourceChanged();
    void positionMsChanged();
    void offsetMsChanged();
    void currentTextChanged();
private:
    void update();
    QUrl m_source;
    qint64 m_pos = 0, m_offset = 0;
    QString m_current;
    QVector<Cue> m_cues;
    int m_lastIdx = 0;
};
