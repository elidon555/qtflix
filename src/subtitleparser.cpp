#include "subtitleparser.h"

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QStringList>
#include <algorithm>

// ----------------------------------------------------------------------------------------------
// Helpers (file-local)
// ----------------------------------------------------------------------------------------------
namespace {

enum class Format { Srt, Vtt, Ass };

// Decode raw bytes: BOM aware (UTF-8 / UTF-16), UTF-8 by default, Latin-1 if the UTF-8 is invalid.
QString decode(QByteArray data)
{
    if (data.startsWith("\xEF\xBB\xBF"))
        data.remove(0, 3);
    else if (data.startsWith("\xFF\xFE") || data.startsWith("\xFE\xFF")) {
        auto enc = data.startsWith("\xFF\xFE") ? QStringConverter::Utf16LE : QStringConverter::Utf16BE;
        QStringDecoder dec(enc);
        return dec(data.mid(2));
    }
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    QString s = utf8(data);
    if (!utf8.hasError())
        return s;
    return QString::fromLatin1(data);
}

// "01:02:03,456" | "01:02:03.456" | "02:03.456" (VTT) | "1:02:03.45" (ASS, centiseconds). -1 if invalid.
qint64 parseTime(QStringView t)
{
    t = t.trimmed();
    if (t.isEmpty())
        return -1;
    qint64 h = 0, m = 0, s = 0, frac = 0;
    int fracDigits = 0;
    QList<QStringView> parts;
    // split on ':'
    qsizetype start = 0;
    for (qsizetype i = 0; i <= t.size(); ++i) {
        if (i == t.size() || t[i] == u':') {
            parts.append(t.mid(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() < 2 || parts.size() > 3)
        return -1;
    bool ok = true;
    QStringView secPart = parts.last();
    qsizetype sep = secPart.indexOf(u',');
    if (sep < 0)
        sep = secPart.indexOf(u'.');
    QStringView secs = sep >= 0 ? secPart.left(sep) : secPart;
    QStringView fr = sep >= 0 ? secPart.mid(sep + 1) : QStringView();
    s = secs.toLongLong(&ok);
    if (!ok)
        return -1;
    if (!fr.isEmpty()) {
        fracDigits = int(qMin<qsizetype>(fr.size(), 3));
        frac = fr.left(fracDigits).toLongLong(&ok);
        if (!ok)
            return -1;
        for (int i = fracDigits; i < 3; ++i)
            frac *= 10;
    }
    if (parts.size() == 3) {
        h = parts[0].toLongLong(&ok);
        if (!ok)
            return -1;
        m = parts[1].toLongLong(&ok);
    } else {
        m = parts[0].toLongLong(&ok);
    }
    if (!ok)
        return -1;
    return ((h * 60 + m) * 60 + s) * 1000 + frac;
}

QString escapeText(QStringView s)
{
    QString out;
    out.reserve(s.size());
    for (QChar c : s) {
        if (c == u'&') out += QLatin1String("&amp;");
        else if (c == u'<') out += QLatin1String("&lt;");
        else if (c == u'>') out += QLatin1String("&gt;");
        else out += c;
    }
    return out;
}

QString decodeEntities(QString s)
{
    if (!s.contains(u'&'))
        return s;
    s.replace(QLatin1String("&lt;"), QLatin1String("<"));
    s.replace(QLatin1String("&gt;"), QLatin1String(">"));
    s.replace(QLatin1String("&nbsp;"), QString(QChar(0xA0)));
    s.replace(QLatin1String("&quot;"), QLatin1String("\""));
    s.replace(QLatin1String("&apos;"), QLatin1String("'"));
    s.replace(QLatin1String("&lrm;"), QString());
    s.replace(QLatin1String("&rlm;"), QString());
    s.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return s;
}

// Turn SRT/VTT cue text into StyledText/RichText-safe markup: keeps <i>/<b>/<u> (and closing),
// strips every other tag (<font>, <c.x>, <v Name>, <00:00:01.000>, ...) and ASS-style {\an8}
// blocks; escapes the rest; newlines become <br>.
QString htmlCue(const QString &raw, bool vtt)
{
    static const QRegularExpression assBlock(QStringLiteral("\\{\\\\[^}]*\\}"));
    QString text = raw;
    text.remove(assBlock);

    QString out;
    out.reserve(text.size() + 16);
    QString plain;
    auto flush = [&]() {
        if (!plain.isEmpty()) {
            out += escapeText(vtt ? decodeEntities(plain) : plain);
            plain.clear();
        }
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == u'<') {
            const qsizetype close = text.indexOf(u'>', i + 1);
            if (close < 0) { plain += c; continue; }
            QString tag = text.mid(i + 1, close - i - 1).trimmed().toLower();
            // a '<' that does not start a tag-ish token: treat literally
            if (tag.isEmpty() || !(tag[0].isLetter() || tag[0] == u'/' || tag[0].isDigit())) {
                plain += c;
                continue;
            }
            flush();
            bool closing = tag.startsWith(u'/');
            if (closing) tag.remove(0, 1);
            const QString name = tag.section(QRegularExpression(QStringLiteral("[\\s.]")), 0, 0);
            if (name == u"i" || name == u"b" || name == u"u")
                out += (closing ? QStringLiteral("</") : QStringLiteral("<")) + name + u'>';
            i = close;
        } else if (c == u'\n') {
            flush();
            out += QLatin1String("<br>");
        } else {
            plain += c;
        }
    }
    flush();
    // drop leading/trailing breaks
    while (out.startsWith(QLatin1String("<br>"))) out.remove(0, 4);
    while (out.endsWith(QLatin1String("<br>"))) out.chop(4);
    return out;
}

// Balance <i>/<b>/<u>: a cue like "<i>foo" (common in sloppy SRTs) would otherwise leak style.
QString balanceTags(QString s)
{
    for (const char *n : {"i", "b", "u"}) {
        const QString open = QStringLiteral("<%1>").arg(QLatin1String(n));
        const QString close = QStringLiteral("</%1>").arg(QLatin1String(n));
        const int diff = int(s.count(open) - s.count(close));
        for (int k = 0; k < diff; ++k) s += close;
    }
    return s;
}

QVector<SubtitleTrack::Cue> parseSrtVtt(const QString &content, bool vtt)
{
    QVector<SubtitleTrack::Cue> cues;
    const QStringList lines = content.split(u'\n');
    qsizetype i = 0;
    const qsizetype n = lines.size();
    auto lineAt = [&](qsizetype k) {
        QString l = lines[k];
        if (l.endsWith(u'\r')) l.chop(1);
        return l;
    };
    if (vtt) {
        // skip header block (WEBVTT ... up to first blank line)
        while (i < n && !lineAt(i).trimmed().isEmpty()) ++i;
    }
    while (i < n) {
        QString l = lineAt(i);
        if (l.trimmed().isEmpty()) { ++i; continue; }
        if (vtt && (l.startsWith(QLatin1String("NOTE")) || l.startsWith(QLatin1String("STYLE"))
                    || l.startsWith(QLatin1String("REGION")))) {
            while (i < n && !lineAt(i).trimmed().isEmpty()) ++i;
            continue;
        }
        // find the timing line within this block (index / cue identifier lines come first)
        qsizetype timing = -1;
        for (qsizetype k = i; k < n && k < i + 3; ++k) {
            const QString lk = lineAt(k);
            if (lk.contains(QLatin1String("-->"))) { timing = k; break; }
            if (lk.trimmed().isEmpty()) break;
        }
        if (timing < 0) {
            while (i < n && !lineAt(i).trimmed().isEmpty()) ++i;
            continue;
        }
        const QString tl = lineAt(timing);
        const qsizetype arrow = tl.indexOf(QLatin1String("-->"));
        QStringView left = QStringView(tl).left(arrow).trimmed();
        QStringView right = QStringView(tl).mid(arrow + 3).trimmed();
        // strip VTT cue settings / SRT coordinates after the end time
        qsizetype sp = right.indexOf(u' ');
        if (sp < 0) sp = right.indexOf(u'\t');
        if (sp >= 0) right = right.left(sp);
        const qint64 st = parseTime(left);
        const qint64 en = parseTime(right);
        i = timing + 1;
        QStringList textLines;
        while (i < n) {
            const QString tx = lineAt(i);
            if (tx.trimmed().isEmpty()) break;
            // tolerate SRTs with no blank line between cues: "N" followed by a timing line
            if (!vtt && i + 1 < n && lineAt(i + 1).contains(QLatin1String("-->"))) {
                bool isNum = false;
                tx.trimmed().toInt(&isNum);
                if (isNum) break;
            }
            textLines << tx;
            ++i;
        }
        if (st < 0 || en < 0 || en <= st)
            continue;
        QString html = balanceTags(htmlCue(textLines.join(u'\n'), vtt));
        if (html.isEmpty())
            continue;
        cues.append({st, en, html});
    }
    return cues;
}

// Convert an ASS text field into StyledText markup.
QString assText(const QString &raw)
{
    QString out;
    QString plain;
    bool italic = false, bold = false, drawing = false;
    auto flush = [&]() {
        if (!plain.isEmpty()) { if (!drawing) out += escapeText(plain); plain.clear(); }
    };
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const QChar c = raw[i];
        if (c == u'{') {
            const qsizetype close = raw.indexOf(u'}', i + 1);
            if (close < 0) { plain += c; continue; }
            flush();
            const QString block = raw.mid(i + 1, close - i - 1);
            // look at override tags we care about
            const QStringList tags = block.split(u'\\', Qt::SkipEmptyParts);
            for (const QString &t : tags) {
                const QString tt = t.trimmed();
                if (tt == u"i1" && !italic) { out += QLatin1String("<i>"); italic = true; }
                else if (tt == u"i0" && italic) { out += QLatin1String("</i>"); italic = false; }
                else if (tt == u"b1" && !bold) { out += QLatin1String("<b>"); bold = true; }
                else if (tt == u"b0" && bold) { out += QLatin1String("</b>"); bold = false; }
                else if (tt.startsWith(u'p') && tt.size() >= 2 && tt[1].isDigit())
                    drawing = tt.mid(1).toInt() > 0;
                else if (tt == u"r") {
                    if (italic) { out += QLatin1String("</i>"); italic = false; }
                    if (bold) { out += QLatin1String("</b>"); bold = false; }
                }
            }
            i = close;
        } else if (c == u'\\' && i + 1 < raw.size()
                   && (raw[i + 1] == u'N' || raw[i + 1] == u'n' || raw[i + 1] == u'h')) {
            flush();
            out += raw[i + 1] == u'h' ? QStringLiteral("&nbsp;") : QStringLiteral("<br>");
            ++i;
        } else {
            plain += c;
        }
    }
    flush();
    if (italic) out += QLatin1String("</i>");
    if (bold) out += QLatin1String("</b>");
    while (out.startsWith(QLatin1String("<br>"))) out.remove(0, 4);
    while (out.endsWith(QLatin1String("<br>"))) out.chop(4);
    return out;
}

QVector<SubtitleTrack::Cue> parseAss(const QString &content)
{
    QVector<SubtitleTrack::Cue> cues;
    bool inEvents = false;
    // default ASS v4+ order
    QStringList format = {QStringLiteral("layer"), QStringLiteral("start"), QStringLiteral("end"),
                          QStringLiteral("style"), QStringLiteral("name"), QStringLiteral("marginl"),
                          QStringLiteral("marginr"), QStringLiteral("marginv"), QStringLiteral("effect"),
                          QStringLiteral("text")};
    const QStringList lines = content.split(u'\n');
    for (QString l : lines) {
        if (l.endsWith(u'\r')) l.chop(1);
        const QString t = l.trimmed();
        if (t.startsWith(u'[')) {
            inEvents = t.compare(QLatin1String("[Events]"), Qt::CaseInsensitive) == 0;
            continue;
        }
        if (!inEvents)
            continue;
        if (t.startsWith(QLatin1String("Format:"), Qt::CaseInsensitive)) {
            format.clear();
            for (const QString &f : t.mid(7).split(u','))
                format << f.trimmed().toLower();
            continue;
        }
        if (!t.startsWith(QLatin1String("Dialogue:"), Qt::CaseInsensitive))
            continue;
        const QString body = t.mid(9).trimmed();
        const int nf = int(format.size());
        const int startIdx = int(format.indexOf(QStringLiteral("start")));
        const int endIdx = int(format.indexOf(QStringLiteral("end")));
        int textIdx = int(format.indexOf(QStringLiteral("text")));
        if (textIdx < 0) textIdx = nf - 1;
        if (startIdx < 0 || endIdx < 0 || nf < 2)
            continue;
        // split into nf fields; the text field (last) keeps any commas
        QStringList fields;
        qsizetype pos = 0;
        for (int f = 0; f < nf - 1; ++f) {
            const qsizetype comma = body.indexOf(u',', pos);
            if (comma < 0) break;
            fields << body.mid(pos, comma - pos);
            pos = comma + 1;
        }
        if (fields.size() != nf - 1)
            continue;
        fields << body.mid(pos);
        const qint64 st = parseTime(fields[startIdx]);
        const qint64 en = parseTime(fields[endIdx]);
        if (st < 0 || en <= st)
            continue;
        const QString html = assText(fields[textIdx]);
        if (html.isEmpty())
            continue;
        cues.append({st, en, html});
    }
    return cues;
}

// Flatten (possibly overlapping) cues into a sorted list of non-overlapping intervals whose
// text is the concatenation of every cue active in it. Makes lookup a simple interval search.
QVector<SubtitleTrack::Cue> flatten(QVector<SubtitleTrack::Cue> cues)
{
    std::stable_sort(cues.begin(), cues.end(), [](const auto &a, const auto &b) {
        return a.start < b.start;
    });
    bool overlap = false;
    for (qsizetype k = 1; k < cues.size(); ++k)
        if (cues[k].start < cues[k - 1].end) { overlap = true; break; }
    if (!overlap)
        return cues;

    QVector<qint64> bounds;
    bounds.reserve(cues.size() * 2);
    for (const auto &c : cues) { bounds << c.start << c.end; }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    QVector<SubtitleTrack::Cue> out;
    QVector<int> active;   // indices into cues, in start order
    qsizetype next = 0;
    for (qsizetype b = 0; b + 1 < bounds.size(); ++b) {
        const qint64 a = bounds[b], e = bounds[b + 1];
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](int idx) { return cues[idx].end <= a; }),
                     active.end());
        while (next < cues.size() && cues[next].start <= a)
            active << int(next++);
        if (active.isEmpty())
            continue;
        QStringList texts;
        for (int idx : active)
            if (!texts.contains(cues[idx].text)) texts << cues[idx].text;
        const QString txt = texts.join(QLatin1String("<br>"));
        if (!out.isEmpty() && out.last().end == a && out.last().text == txt)
            out.last().end = e;  // merge identical adjacent segments
        else
            out.append({a, e, txt});
    }
    return out;
}

// Reads and parses a subtitle file (runs on a worker thread).
QVector<SubtitleTrack::Cue> loadFile(const QString &path)
{
    QFile f(path);
    if (f.size() >= 64 * 1024 * 1024 || !f.open(QIODevice::ReadOnly)) {
        qWarning("SubtitleTrack: cannot open %s", qPrintable(path));
        return {};
    }
    QString content = decode(f.readAll());
    content.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    const QString ext = QFileInfo(path).suffix().toLower();
    const QString head = content.left(512).trimmed();
    Format fmt = Format::Srt;
    if (ext == u"vtt" || head.startsWith(QLatin1String("WEBVTT")))
        fmt = Format::Vtt;
    else if (ext == u"ass" || ext == u"ssa" || head.startsWith(QLatin1String("[Script Info]"), Qt::CaseInsensitive))
        fmt = Format::Ass;
    QVector<SubtitleTrack::Cue> cues = fmt == Format::Ass ? parseAss(content)
                                                          : parseSrtVtt(content, fmt == Format::Vtt);
    return flatten(std::move(cues));
}

} // namespace

// ----------------------------------------------------------------------------------------------

SubtitleTrack::SubtitleTrack(QObject *parent) : QObject(parent) {}

void SubtitleTrack::setSource(const QUrl &u)
{
    if (u == m_source)
        return;
    m_source = u;
    const quint64 gen = ++m_gen;
    const bool hadCues = !m_cues.isEmpty();
    m_cues.clear();
    m_lastIdx = 0;

    QString path;
    if (u.isLocalFile()) path = u.toLocalFile();
    else if (u.scheme().isEmpty()) path = u.toString();
    else if (u.scheme() == u"qrc") path = u':' + u.path();

    if (!path.isEmpty()) {
        // parsed off the GUI thread; a result for an older source is dropped
        auto *watcher = new QFutureWatcher<QVector<Cue>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, gen]() {
            watcher->deleteLater();
            if (gen != m_gen)
                return;
            m_cues = watcher->result();
            m_lastIdx = 0;
            emit cuesChanged();
            update();
        });
        watcher->setFuture(QtConcurrent::run(loadFile, path));
    }
    emit sourceChanged();
    if (hadCues)
        emit cuesChanged();
    update();
}


void SubtitleTrack::setPositionMs(qint64 ms)
{
    if (ms == m_pos)
        return;
    m_pos = ms;
    emit positionMsChanged();
    update();
}

void SubtitleTrack::setOffsetMs(qint64 ms)
{
    if (ms == m_offset)
        return;
    m_offset = ms;
    emit offsetMsChanged();
    update();
}

// offsetMs > 0 delays the subtitles (they appear later), < 0 makes them appear earlier.
void SubtitleTrack::update()
{
    QString text;
    const qint64 t = m_pos - m_offset;
    const int n = int(m_cues.size());
    if (n > 0) {
        int idx = -1;
        auto inside = [&](int k) { return k >= 0 && k < n && m_cues[k].start <= t && t < m_cues[k].end; };
        if (inside(m_lastIdx)) {
            idx = m_lastIdx;
        } else if (m_lastIdx >= 0 && m_lastIdx < n && t >= m_cues[m_lastIdx].start
                   && (m_lastIdx + 1 >= n || t < m_cues[m_lastIdx + 1].start)) {
            // between the remembered cue and the next one: nothing shown, keep the index
            idx = -1;
        } else if (inside(m_lastIdx + 1)) {
            idx = m_lastIdx + 1;   // normal forward playback
        } else {
            // seek: binary search for the last cue starting at or before t
            auto it = std::upper_bound(m_cues.cbegin(), m_cues.cend(), t,
                                       [](qint64 v, const Cue &c) { return v < c.start; });
            const int k = int(it - m_cues.cbegin()) - 1;
            m_lastIdx = qMax(0, k);
            if (inside(k)) idx = k;
        }
        if (idx >= 0) {
            m_lastIdx = idx;
            text = m_cues[idx].text;
        }
    }
    if (text != m_current) {
        m_current = text;
        emit currentTextChanged();
    }
}
