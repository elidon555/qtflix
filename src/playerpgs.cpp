#include "playerpgs.h"
#include "playertrickplay.h" // setBackgroundPriority()

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>
#include <QTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <algorithm>

namespace {

constexpr quint8 kPDS = 0x14, kODS = 0x15, kPCS = 0x16, kWDS = 0x17, kEND = 0x80;
constexpr qint64 kLastCueMs = 10000; // a final cue without a clearing display set stays this long

inline quint16 be16(const uchar *p) { return quint16((p[0] << 8) | p[1]); }
inline quint32 be24(const uchar *p) { return quint32((p[0] << 16) | (p[1] << 8) | p[2]); }
inline quint32 be32(const uchar *p) { return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3]; }

// YCbCr (limited range) + alpha -> premultiplied ARGB. BT.709 for HD canvases, BT.601 for SD.
QRgb ycbcrToRgb(int y, int cr, int cb, int a, bool hd)
{
    const double Y = 1.164 * (y - 16), Cb = cb - 128, Cr = cr - 128;
    double r, g, b;
    if (hd) {
        r = Y + 1.793 * Cr;
        g = Y - 0.213 * Cb - 0.533 * Cr;
        b = Y + 2.112 * Cb;
    } else {
        r = Y + 1.596 * Cr;
        g = Y - 0.392 * Cb - 0.813 * Cr;
        b = Y + 2.017 * Cb;
    }
    auto c = [](double v) { return std::clamp(int(v + 0.5), 0, 255); };
    return qPremultiply(qRgba(c(r), c(g), c(b), a));
}

struct ObjectData {
    int width = 0, height = 0;
    QByteArray rle;
};

struct PendingComposition {
    bool valid = false;
    qint64 ptsMs = 0;
    int canvasW = 1920, canvasH = 1080;
    int paletteId = 0;
    struct Ref { int objectId; int x, y; bool cropped; QRect crop; };
    QVector<Ref> refs;
};

} // namespace

// ------------------------------------------------------------------------------------------ parsing

// Incremental .sup parser: feed() takes the stream in arbitrary chunks (a file that is still being written)
// and keeps an incomplete trailing segment for the next call.
class PgsParser
{
public:
    void feed(const QByteArray &chunk);
    QVector<PgsCue> cues() const;

private:
    QByteArray m_buf;          // unparsed tail (an incomplete segment)
    QVector<PgsCue> m_sets;    // every display set, including the ones that clear the screen
    QHash<int, QVector<QRgb>> m_palettes;
    QHash<int, ObjectData> m_objects;
    PendingComposition m_pcs;
};

void PgsParser::feed(const QByteArray &chunk)
{
    m_buf.append(chunk);
    QVector<PgsCue> &sets = m_sets;
    QHash<int, QVector<QRgb>> &palettes = m_palettes;
    QHash<int, ObjectData> &objects = m_objects;
    PendingComposition &pcs = m_pcs;

    const uchar *base = reinterpret_cast<const uchar *>(m_buf.constData());
    const qsizetype n = m_buf.size();
    qsizetype off = 0;
    while (off + 13 <= n) {
        if (base[off] != 'P' || base[off + 1] != 'G') { // resync on garbage
            ++off;
            continue;
        }
        const quint32 pts = be32(base + off + 2);
        const quint8 type = base[off + 10];
        const quint16 size = be16(base + off + 11);
        const uchar *p = base + off + 13;
        if (off + 13 + size > n)
            break;
        off += 13 + size;

        switch (type) {
        case kPCS: {
            if (size < 11)
                break;
            pcs = PendingComposition();
            pcs.valid = true;
            pcs.ptsMs = qint64(pts) / 90;
            pcs.canvasW = be16(p);
            pcs.canvasH = be16(p + 2);
            const quint8 state = p[7];
            pcs.paletteId = p[9];
            const int count = p[10];
            if (state & 0x80) { // epoch start: earlier objects and palettes are no longer valid
                objects.clear();
                palettes.clear();
            }
            const uchar *q = p + 11;
            const uchar *end = p + size;
            for (int i = 0; i < count && q + 8 <= end; ++i) {
                PendingComposition::Ref r;
                r.objectId = be16(q);
                const quint8 flags = q[3];
                r.x = be16(q + 4);
                r.y = be16(q + 6);
                q += 8;
                r.cropped = flags & 0x80;
                if (r.cropped) {
                    if (q + 8 > end)
                        break;
                    r.crop = QRect(be16(q), be16(q + 2), be16(q + 4), be16(q + 6));
                    q += 8;
                }
                pcs.refs.append(r);
            }
            break;
        }
        case kPDS: {
            if (size < 2)
                break;
            const int id = p[0];
            const bool hd = pcs.canvasH > 576;
            QVector<QRgb> &pal = palettes[id];
            if (pal.size() != 256)
                pal = QVector<QRgb>(256, 0);
            for (int i = 2; i + 5 <= size; i += 5)
                pal[p[i]] = ycbcrToRgb(p[i + 1], p[i + 2], p[i + 3], p[i + 4], hd);
            break;
        }
        case kODS: {
            if (size < 4)
                break;
            const int id = be16(p);
            const quint8 seq = p[3];
            if (seq & 0x80) { // first fragment: data length (3 bytes), width, height, then RLE
                if (size < 11)
                    break;
                ObjectData od;
                od.width = be16(p + 7);
                od.height = be16(p + 9);
                od.rle = QByteArray(reinterpret_cast<const char *>(p + 11), size - 11);
                objects.insert(id, od);
            } else {
                auto it = objects.find(id);
                if (it != objects.end())
                    it->rle.append(reinterpret_cast<const char *>(p + 4), size - 4);
            }
            break;
        }
        case kEND: {
            if (!pcs.valid)
                break;
            PgsCue cue;
            cue.startMs = pcs.ptsMs;
            cue.canvasWidth = pcs.canvasW > 0 ? pcs.canvasW : 1920;
            cue.canvasHeight = pcs.canvasH > 0 ? pcs.canvasH : 1080;
            cue.palette = palettes.value(pcs.paletteId);
            if (cue.palette.size() != 256)
                cue.palette = QVector<QRgb>(256, 0);
            for (const auto &r : std::as_const(pcs.refs)) {
                const auto it = objects.constFind(r.objectId);
                if (it == objects.cend() || it->width <= 0 || it->height <= 0)
                    continue;
                PgsObject o;
                o.x = r.x;
                o.y = r.y;
                o.width = it->width;
                o.height = it->height;
                o.cropped = r.cropped;
                o.crop = r.crop;
                o.rle = it->rle;
                cue.objects.append(o);
            }
            sets.append(cue);
            pcs.valid = false;
            break;
        }
        case kWDS:
        default:
            break;
        }
    }
    m_buf.remove(0, off);
}

QVector<PgsCue> PgsParser::cues() const
{
    QVector<PgsCue> sets = m_sets;
    std::stable_sort(sets.begin(), sets.end(), [](const PgsCue &a, const PgsCue &b) { return a.startMs < b.startMs; });
    QVector<PgsCue> cues;
    for (int i = 0; i < sets.size(); ++i) {
        if (sets[i].objects.isEmpty())
            continue; // clears the screen; only marks the previous cue's end
        PgsCue c = sets[i];
        c.endMs = i + 1 < sets.size() ? sets[i + 1].startMs : c.startMs + kLastCueMs;
        if (c.endMs <= c.startMs)
            continue; // replaced in the same instant
        cues.append(std::move(c));
    }
    return cues;
}

QVector<PgsCue> PgsSubtitles::parseSup(const QByteArray &data)
{
    PgsParser p;
    p.feed(data);
    return p.cues();
}

QImage PgsSubtitles::decodeObject(const PgsObject &obj, const QVector<QRgb> &palette)
{
    if (obj.width <= 0 || obj.height <= 0 || obj.width > 8192 || obj.height > 8192 || palette.size() != 256)
        return {};
    QImage img(obj.width, obj.height, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    const uchar *p = reinterpret_cast<const uchar *>(obj.rle.constData());
    const uchar *end = p + obj.rle.size();
    int x = 0, y = 0;
    QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(0));
    auto put = [&](int len, int color) {
        const QRgb c = palette[color];
        const int stop = std::min(obj.width, x + len);
        if (c != 0)
            for (int i = x; i < stop; ++i)
                line[i] = c;
        x += len;
    };
    while (p < end && y < obj.height) {
        const uchar b = *p++;
        if (b != 0) {
            put(1, b);
            continue;
        }
        if (p >= end)
            break;
        const uchar b2 = *p++;
        if (b2 == 0) { // end of line
            x = 0;
            if (++y < obj.height)
                line = reinterpret_cast<QRgb *>(img.scanLine(y));
            continue;
        }
        int len = b2 & 0x3F;
        if (b2 & 0x40) {
            if (p >= end)
                break;
            len = (len << 8) | *p++;
        }
        int color = 0;
        if (b2 & 0x80) {
            if (p >= end)
                break;
            color = *p++;
        }
        put(len, color);
    }
    if (obj.cropped && obj.crop.isValid())
        return img.copy(obj.crop);
    return img;
}


// ------------------------------------------------------------------------------------------ item

PgsSubtitles::PgsSubtitles(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_poll = new QTimer(this);
    m_poll->setInterval(500);
    connect(m_poll, &QTimer::timeout, this, &PgsSubtitles::parseMore);
}

PgsSubtitles::~PgsSubtitles()
{
    stopProcesses();
}

// Kills the helpers without waiting for them: each one deletes itself (and the .part files it was
// writing) once it has exited.
void PgsSubtitles::stopProcesses()
{
    m_poll->stop();
    QStringList parts;
    for (const QString &path : std::as_const(m_extracting))
        parts << path + QStringLiteral(".part");
    m_extracting.clear();
    for (QPointer<QProcess> *pp : {&m_probe, &m_extract}) {
        if (QProcess *proc = pp->data()) {
            const QStringList tmp = pp == &m_extract ? parts : QStringList();
            auto cleanUp = [proc, tmp]() {
                for (const QString &t : tmp)
                    QFile::remove(t);
                proc->deleteLater();
            };
            proc->disconnect(this);
            proc->setParent(nullptr); // outlives this item until it is gone
            if (proc->state() == QProcess::NotRunning) {
                cleanUp();
            } else {
                connect(proc, &QProcess::finished, proc, cleanUp);
                proc->kill();
            }
        }
        *pp = nullptr;
    }
}

void PgsSubtitles::resetCues()
{
    m_poll->stop();
    m_parser.reset();
    m_parsePath.clear();
    m_parseOffset = 0;
    m_parseAgain = false;
    m_cues.clear();
    m_current = -1;
    m_currentStart = -1;
    m_images.clear();
    m_imagesDirty = true;
    m_loading = false;
    update();
}

void PgsSubtitles::setFile(const QString &f)
{
    if (f == m_file)
        return;
    m_file = f;
    ++m_gen;
    stopProcesses();
    m_streams.clear();
    m_streamsKnown = false;
    resetCues();
    emit fileChanged();
    emit streamsChanged();
    emit stateChanged();
    probe(); // load() follows once the streams are known
}

void PgsSubtitles::setStream(int s)
{
    if (s < 0)
        s = -1;
    if (s == m_stream)
        return;
    m_stream = s;
    ++m_gen;
    resetCues(); // a running extraction pass keeps going: it writes every PGS stream of the file
    emit streamChanged();
    emit stateChanged();
    load();
}

void PgsSubtitles::setPositionMs(qint64 ms)
{
    if (ms == m_pos)
        return;
    m_pos = ms;
    emit positionMsChanged();
    updateCurrent();
}

void PgsSubtitles::setVideoRect(const QRectF &r)
{
    if (r == m_videoRect)
        return;
    m_videoRect = r;
    emit videoRectChanged();
    if (!m_images.isEmpty())
        update();
}

void PgsSubtitles::setLift(qreal l)
{
    if (qFuzzyCompare(l + 1, m_lift + 1))
        return;
    m_lift = l;
    emit liftChanged();
    if (!m_images.isEmpty())
        update();
}

void PgsSubtitles::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (!m_images.isEmpty())
        update();
}

bool PgsSubtitles::isPgs(int ordinal) const
{
    if (ordinal < 0 || ordinal >= m_streams.size())
        return false;
    return m_streams.at(ordinal).toMap().value(QStringLiteral("pgs")).toBool();
}

void PgsSubtitles::probe()
{
    if (m_file.isEmpty())
        return;
    auto *proc = new QProcess(this);
    m_probe = proc;
    // stopProcesses() disconnects us on a file change, so whatever arrives here belongs to m_file
    auto done = [this, proc](bool ok) {
        proc->deleteLater();
        m_probe = nullptr;
        QVariantList list;
        if (ok) {
            const QJsonArray arr = QJsonDocument::fromJson(proc->readAllStandardOutput()).object()
                                       .value(QStringLiteral("streams")).toArray();
            for (const QJsonValue &v : arr) {
                const QJsonObject s = v.toObject();
                const QJsonObject tags = s.value(QStringLiteral("tags")).toObject();
                const QString codec = s.value(QStringLiteral("codec_name")).toString();
                list.append(QVariantMap{
                    {QStringLiteral("codec"), codec},
                    {QStringLiteral("language"), tags.value(QStringLiteral("language")).toString()},
                    {QStringLiteral("title"), tags.value(QStringLiteral("title")).toString()},
                    {QStringLiteral("pgs"), codec == QLatin1String("hdmv_pgs_subtitle")},
                });
            }
        }
        m_streams = list;
        m_streamsKnown = true;
        emit streamsChanged();
        if (m_stream >= 0 && !m_parser)
            load();
    };
    connect(proc, &QProcess::finished, this, [done](int code, QProcess::ExitStatus status) {
        done(status == QProcess::NormalExit && code == 0);
    });
    connect(proc, &QProcess::errorOccurred, this, [done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            done(false);
    });
    proc->start(QStringLiteral("ffprobe"),
                {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("s"),
                 QStringLiteral("-show_entries"), QStringLiteral("stream=codec_name:stream_tags=language,title"),
                 QStringLiteral("-of"), QStringLiteral("json"), m_file});
}

QString PgsSubtitles::cachePathFor(int ordinal) const
{
    const QFileInfo fi(m_file);
    const QByteArray key = (fi.absoluteFilePath() + QLatin1Char('|') + QString::number(fi.size()) + QLatin1Char('|')
                            + QString::number(fi.lastModified().toMSecsSinceEpoch())).toUtf8();
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(20));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/pgs");
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + hash + QLatin1Char('-') + QString::number(ordinal) + QStringLiteral(".sup");
}

void PgsSubtitles::load()
{
    if (m_file.isEmpty() || m_stream < 0 || !m_streamsKnown || !isPgs(m_stream))
        return;
    const QString path = cachePathFor(m_stream);
    if (!m_extracting.contains(m_stream) && QFileInfo(path).size() <= 0) {
        // first time for this file: copy out every PGS stream that is not cached yet, in one pass
        QList<int> missing;
        for (int i = 0; i < m_streams.size(); ++i)
            if (isPgs(i) && (i == m_stream || QFileInfo(cachePathFor(i)).size() <= 0))
                missing << i;
        extract(missing);
        if (!m_extracting.contains(m_stream))
            return;
    }
    m_parser = std::make_shared<PgsParser>();
    m_parseOffset = 0;
    m_loading = true;
    emit stateChanged();
    if (m_extracting.contains(m_stream)) {
        m_parsePath = path + QStringLiteral(".part"); // still being written: follow it
        m_poll->start();
    } else {
        m_parsePath = path;
    }
    parseMore();
}

void PgsSubtitles::extract(const QList<int> &ordinals)
{
    if (ordinals.isEmpty() || m_extract)
        return;
    QStringList args {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
                      QStringLiteral("-i"), m_file};
    QHash<int, QString> targets;
    for (int ordinal : ordinals) {
        const QString path = cachePathFor(ordinal);
        targets.insert(ordinal, path);
        args << QStringLiteral("-map") << QStringLiteral("0:s:%1").arg(ordinal) << QStringLiteral("-c")
             << QStringLiteral("copy") << QStringLiteral("-f") << QStringLiteral("sup") << path + QStringLiteral(".part");
    }
    auto *proc = new QProcess(this);
    setBackgroundPriority(proc);
    m_extract = proc;
    m_extracting = targets;
    // stopProcesses() disconnects us on a file change, so whatever arrives here belongs to m_file
    auto done = [this, proc, targets](bool ok) {
        proc->deleteLater();
        m_extract = nullptr;
        m_extracting.clear();
        if (!ok)
            qWarning("PGS: could not extract subtitle streams: %s", proc->readAllStandardError().constData());
        for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
            const QString part = it.value() + QStringLiteral(".part");
            if (ok && QFileInfo(part).size() > 0) {
                QFile::remove(it.value());
                QFile::rename(part, it.value());
            } else {
                QFile::remove(part);
            }
        }
        if (!m_parser || !targets.contains(m_stream))
            return;
        m_poll->stop();
        if (ok && QFileInfo(targets.value(m_stream)).size() > 0) {
            m_parsePath = targets.value(m_stream); // same bytes: the parse continues at m_parseOffset
            parseMore();
        } else {
            m_parsePath.clear();
            m_loading = false;
            emit stateChanged();
        }
    };
    connect(proc, &QProcess::finished, this, [done](int code, QProcess::ExitStatus st) {
        done(st == QProcess::NormalExit && code == 0);
    });
    connect(proc, &QProcess::errorOccurred, this, [done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            done(false);
    });
    proc->start(QStringLiteral("ffmpeg"), args);
}

// Feeds whatever was appended to m_parsePath since the last call to the selected stream's parser, on a
// worker thread (one task at a time; a request while one runs is queued).
void PgsSubtitles::parseMore()
{
    if (m_parseBusy) {
        m_parseAgain = true;
        return;
    }
    m_parseAgain = false;
    if (!m_parser || m_parsePath.isEmpty())
        return;
    struct Result { qint64 offset; QVector<PgsCue> cues; bool grew; };
    const std::shared_ptr<PgsParser> parser = m_parser;
    const QString path = m_parsePath;
    const qint64 offset = m_parseOffset;
    const quint64 gen = m_gen;
    m_parseBusy = true;
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, gen, path]() {
        watcher->deleteLater();
        m_parseBusy = false;
        if (gen == m_gen) {
            Result r = watcher->result();
            m_parseOffset = r.offset;
            if (r.grew)
                setCues(std::move(r.cues));
            if (!m_parseAgain && path == m_parsePath && !path.endsWith(QLatin1String(".part"))) {
                m_loading = false; // the complete stream is in
                emit stateChanged();
            }
        }
        if (m_parseAgain)
            parseMore();
    });
    watcher->setFuture(QtConcurrent::run([parser, path, offset]() {
        Result r {offset, {}, false};
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly) || f.size() <= offset || !f.seek(offset))
            return r;
        const QByteArray data = f.readAll();
        parser->feed(data);
        r.offset = offset + data.size();
        r.cues = parser->cues();
        r.grew = true;
        return r;
    }));
}

void PgsSubtitles::setCues(QVector<PgsCue> cues)
{
    m_cues = std::move(cues);
    m_current = -1; // re-resolved below; the decoded images stay when it is still the same cue
    emit stateChanged();
    updateCurrent();
}

void PgsSubtitles::updateCurrent()
{
    int idx = -1;
    if (!m_cues.isEmpty()) {
        // last cue starting at or before the position
        auto it = std::upper_bound(m_cues.cbegin(), m_cues.cend(), m_pos,
                                   [](qint64 pos, const PgsCue &c) { return pos < c.startMs; });
        if (it != m_cues.cbegin()) {
            const int i = int(std::distance(m_cues.cbegin(), it)) - 1;
            if (m_pos < m_cues.at(i).endMs)
                idx = i;
        }
    }
    if (idx == m_current)
        return;
    m_current = idx;
    const qint64 start = idx >= 0 ? m_cues.at(idx).startMs : -1;
    if (start == m_currentStart)
        return; // the same cue in a re-parsed list
    m_currentStart = start;
    m_images.clear();
    if (idx >= 0) {
        const PgsCue &c = m_cues.at(idx);
        m_canvas = QSize(c.canvasWidth, c.canvasHeight);
        for (const PgsObject &o : c.objects) {
            const QImage img = decodeObject(o, c.palette);
            if (!img.isNull())
                m_images.append({QRect(QPoint(o.x, o.y), img.size()), img});
        }
    }
    m_imagesDirty = true;
    update();
}

QSGNode *PgsSubtitles::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    // A plain container with one textured quad per object of the current cue, each texture the size of
    // its object; the container stays empty (draws nothing) while no cue is shown.
    QSGNode *root = oldNode;
    if (m_imagesDirty) {
        m_imagesDirty = false;
        if (root) {
            while (QSGNode *child = root->firstChild()) {
                root->removeChildNode(child);
                delete child; // owns its texture
            }
        }
        if (!m_images.isEmpty()) {
            if (!root)
                root = new QSGNode;
            for (const auto &it : std::as_const(m_images)) {
                QSGImageNode *node = window()->createImageNode();
                QSGTexture *tex = window()->createTextureFromImage(it.second);
                node->setTexture(tex);
                node->setOwnsTexture(true);
                node->setFiltering(QSGTexture::Linear);
                node->setSourceRect(QRectF(QPointF(0, 0), tex->textureSize()));
                root->appendChildNode(node);
            }
        }
    }
    if (!root || m_images.isEmpty() || m_canvas.isEmpty())
        return root;

    const QRectF vr = m_videoRect.isEmpty() ? boundingRect() : m_videoRect;
    // Fit the canvas to the video width, centred on the video (letterboxed films keep their subtitles in
    // the bars, as on a Blu-ray player); shrink if the window is too short for it.
    qreal s = vr.width() / m_canvas.width();
    if (m_canvas.height() * s > height())
        s = height() / m_canvas.height();
    const QSizeF cs(m_canvas.width() * s, m_canvas.height() * s);
    QPointF origin(vr.center().x() - cs.width() / 2, vr.center().y() - cs.height() / 2);
    origin.setY(std::clamp(origin.y(), 0.0, std::max(0.0, height() - cs.height())));

    QRectF bounds;
    for (const auto &it : std::as_const(m_images))
        bounds |= QRectF(origin + QPointF(it.first.x() * s, it.first.y() * s), QSizeF(it.first.size()) * s);
    qreal dy = 0;
    if (m_lift > 0 && bounds.bottom() > height() - m_lift)
        dy = -(bounds.bottom() - (height() - m_lift));
    if (bounds.top() + dy < 0)
        dy = -bounds.top();

    int i = 0;
    for (QSGNode *child = root->firstChild(); child && i < m_images.size(); child = child->nextSibling(), ++i) {
        const QRect &r = m_images.at(i).first;
        static_cast<QSGImageNode *>(child)->setRect(
            QRectF(origin + QPointF(r.x() * s, r.y() * s + dy), QSizeF(r.size()) * s));
    }
    return root;
}
