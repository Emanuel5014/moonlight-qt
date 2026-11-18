#include "filedrop.h"

#include "backend/identitymanager.h"
#include "backend/nvcomputer.h"
#include "video/overlaymanager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSslConfiguration>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QWaitCondition>
#include <QXmlStreamReader>

#include <deque>
#include <map>

namespace {

constexpr qint64 k_InitialChunkSize = 1024 * 1024;
constexpr qint64 k_MinChunkSize = 256 * 1024;
constexpr qint64 k_DefaultMaxChunkSize = 8 * 1024 * 1024;
constexpr qint64 k_TargetChunkMs = 1000;
constexpr int k_RequestTimeoutMs = 15000;
constexpr int k_DataTimeoutMs = 60000;
constexpr int k_CancelTimeoutMs = 1500;
constexpr int k_MaxRetries = 3;
constexpr int k_FirstRetryDelayMs = 1000;
constexpr Uint32 k_ProgressIntervalMs = 250;
constexpr Uint32 k_MessageMs = 3000;
constexpr Uint32 k_ErrorMessageMs = 5000;
constexpr int k_MaxDisplayedNameLength = 40;

QString displayName(const QString& name)
{
    if (name.length() <= k_MaxDisplayedNameLength) {
        return name;
    }
    return name.left(k_MaxDisplayedNameLength - 3) + "...";
}

QString formatRate(double bytesPerSecond)
{
    if (bytesPerSecond >= 1024 * 1024) {
        return QString::number(bytesPerSecond / (1024 * 1024), 'f', 1) + " MB/s";
    }
    return QString::number(bytesPerSecond / 1024, 'f', 0) + " KB/s";
}

QString fileCount(int count)
{
    return count == 1 ? QString("1 file") : QString::number(count) + " files";
}

struct UploadSettings
{
    QString address;
    uint16_t httpsPort;
    QSslCertificate serverCert;
    QSslConfiguration sslConfig;
    QString uniqueId;
    bool toClipboard;
    int maxMbps; // 0 = no limit
};

// The reply of an /actions/upload request
struct UploadReply
{
    bool networkError = true;
    int status = 0;
    QString message;
    std::map<QString, QString> values;

    bool ok() const { return !networkError && status == 200; }
};

} // namespace

// Uploads the dropped files one batch at a time on its own thread, with its own
// QNetworkAccessManager, so neither the stream nor the UI ever waits for it.
class FileUploadThread : public QThread
{
public:
    explicit FileUploadThread(const UploadSettings& settings) :
        m_Settings(settings),
        m_Stopping(false),
        m_CancelGeneration(0),
        m_BatchGeneration(0),
        m_Nam(nullptr),
        m_RateBytes(0)
    {
        setObjectName("File upload");
    }

    void enqueue(const QStringList& paths)
    {
        QMutexLocker locker(&m_Mutex);
        m_Queue.push_back(paths);
        m_Wake.wakeAll();
    }

    // Aborts the current upload (telling the host to drop it) and makes run() return
    void requestStop()
    {
        QMutexLocker locker(&m_Mutex);
        m_Stopping = true;
        m_Wake.wakeAll();
    }

    // Aborts the current upload and drops the queued ones. Returns false if nothing was in progress.
    bool requestCancel()
    {
        QMutexLocker locker(&m_Mutex);
        bool busy = m_Busy || !m_Queue.empty();
        m_Queue.clear();
        m_CancelGeneration++;
        return busy;
    }

protected:
    void run() override
    {
        QNetworkAccessManager nam;
        nam.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        m_Nam = &nam;

        QObject::connect(&nam, &QNetworkAccessManager::sslErrors, &nam,
                         [this](QNetworkReply* reply, const QList<QSslError>& errors) {
            // Same rule as NvHTTP: only trust the certificate we got when pairing
            for (const QSslError& error : errors) {
                if (m_Settings.serverCert.isNull() || m_Settings.serverCert != error.certificate()) {
                    return;
                }
            }
            reply->ignoreSslErrors(errors);
        });

        for (;;) {
            QStringList paths;
            {
                QMutexLocker locker(&m_Mutex);
                m_Busy = false;
                while (m_Queue.empty() && !m_Stopping) {
                    m_Wake.wait(&m_Mutex);
                }
                if (m_Stopping) {
                    break;
                }
                paths = m_Queue.front();
                m_Queue.pop_front();
                m_Busy = true;
                m_BatchGeneration = m_CancelGeneration;
            }

            uploadBatch(paths);
        }

        m_Nam = nullptr;
    }

private:
    bool stopping()
    {
        QMutexLocker locker(&m_Mutex);
        return m_Stopping;
    }

    // The session is ending or the user cancelled this batch
    bool aborted()
    {
        QMutexLocker locker(&m_Mutex);
        return m_Stopping || m_CancelGeneration != m_BatchGeneration;
    }

    // Waits without missing a stop or cancel request. Returns false if aborted.
    bool waitMs(qint64 ms)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) {
            if (aborted()) {
                return false;
            }
            QThread::msleep((unsigned long)qMin<qint64>(50, ms - timer.elapsed()));
        }
        return !aborted();
    }

    UploadReply post(const QString& command, const QList<QPair<QString, QString>>& args,
                     const QByteArray& body, int timeoutMs, bool abortable = true)
    {
        QUrl url;
        url.setScheme("https");
        url.setHost(m_Settings.address);
        url.setPort(m_Settings.httpsPort);
        url.setPath("/actions/upload/" + command);

        // Encode everything but unreserved characters: the host decodes '+' as a space
        QString query = "uniqueid=" + m_Settings.uniqueId +
                        "&uuid=" + QUuid::createUuid().toRfc4122().toHex();
        for (const auto& arg : args) {
            query += "&" + arg.first + "=" + QString::fromLatin1(QUrl::toPercentEncoding(arg.second));
        }
        url.setQuery(query, QUrl::StrictMode);

        QNetworkRequest request(url);
        request.setSslConfiguration(m_Settings.sslConfig);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

        QNetworkReply* reply = m_Nam->post(request, body);

        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(timeoutMs);

        // Check for stop and cancel requests while the request is in flight
        QTimer abortCheck;
        QObject::connect(&abortCheck, &QTimer::timeout, &loop, [&]() {
            if (abortable && aborted()) {
                loop.quit();
            }
        });
        abortCheck.start(100);

        loop.exec(QEventLoop::ExcludeUserInputEvents);

        UploadReply result;
        if (!reply->isFinished()) {
            reply->abort();
            result.message = aborted() ? "Cancelled" : "Request timed out";
        }
        else if (reply->error() != QNetworkReply::NoError) {
            result.message = reply->errorString();
        }
        else {
            result.networkError = false;

            QXmlStreamReader xml(reply->readAll());
            while (!xml.atEnd()) {
                if (xml.readNext() != QXmlStreamReader::StartElement) {
                    continue;
                }
                if (xml.name() == QStringLiteral("root")) {
                    result.status = xml.attributes().value("status_code").toInt();
                    result.message = xml.attributes().value("status_message").toString();
                }
                else {
                    QString name = xml.name().toString();
                    result.values[name] = xml.readElementText();
                }
            }
        }

        delete reply;
        return result;
    }

    // Why the host refused, in words the user can act on
    static QString describeFailure(const UploadReply& reply)
    {
        if (reply.networkError) {
            return "connection error (" + reply.message + ")";
        }

        switch (reply.status) {
        case 401:
            return "this client doesn't have the file upload permission on the host";
        case 403:
            return "the stream is no longer active";
        case 404:
            return "file upload is disabled on the host";
        case 413:
            return "the file is too large for the host";
        case 422:
            return "the file was damaged in transit";
        case 501:
            return "the host doesn't support file upload";
        case 503:
            return "nobody is logged on to the host";
        case 507:
            return "not enough free space on the host";
        default:
            return "error " + QString::number(reply.status) +
                   (reply.message.isEmpty() ? "" : " (" + reply.message + ")");
        }
    }

    // Errors that will fail every other file too
    static bool isFatalForBatch(const UploadReply& reply)
    {
        if (reply.networkError) {
            return true;
        }
        switch (reply.status) {
        case 401:
        case 403:
        case 404:
        case 501:
        case 503:
            return true;
        default:
            return false;
        }
    }

    void cancelOnHost(const QString& id)
    {
        // Best effort: the host also drops abandoned uploads on its own and when the stream ends
        post("cancel", {{"id", id}}, QByteArray(), k_CancelTimeoutMs, false);
    }

    // Keeps the average speed of the batch under the limit chosen by the user
    bool throttle()
    {
        if (m_Settings.maxMbps <= 0) {
            return !aborted();
        }
        qint64 dueMs = m_RateBytes * 8 / (m_Settings.maxMbps * 1000LL);
        return waitMs(dueMs - m_RateTimer.elapsed());
    }

    qint64 maxChunkForRate(qint64 maxChunk) const
    {
        if (m_Settings.maxMbps <= 0) {
            return maxChunk;
        }
        // About one second of transfer at the limit, so the pacing stays smooth
        return qBound(k_MinChunkSize, m_Settings.maxMbps * 125000LL, maxChunk);
    }

    void uploadBatch(const QStringList& paths)
    {
        uint32_t batch = QRandomGenerator::global()->generate();
        int total = paths.size();
        int sent = 0;
        int skippedFolders = 0;
        QString lastError;

        m_RateBytes = 0;
        m_RateTimer.start();

        for (int i = 0; i < total && !aborted(); i++) {
            QFileInfo info(paths[i]);
            if (info.isDir()) {
                skippedFolders++;
                continue;
            }

            QString error;
            if (uploadFile(info, batch, i + 1, total, error)) {
                sent++;
            }
            else if (!error.isEmpty()) {
                lastError = error;
                if (error.startsWith('!')) {
                    // The host refused for a reason that also applies to the other files
                    lastError = error.mid(1);
                    break;
                }
            }
        }

        if (stopping()) {
            return;
        }

        bool cancelled = aborted();
        bool inClipboard = false;
        if (sent > 0) {
            // Older hosts don't know this request: then the files are simply in the folder
            UploadReply reply = post("commit",
                                     {{"batch", QString::number(batch)},
                                      {"clipboard", m_Settings.toClipboard && !cancelled ? "1" : "0"}},
                                     QByteArray(), k_RequestTimeoutMs, false);
            inClipboard = reply.ok() && reply.values["clipboard"] == "1";
        }

        QString text;
        if (cancelled) {
            text = "File transfer cancelled";
            if (sent > 0) {
                text += "\n" + fileCount(sent) + " already saved on the host in Downloads\\streamfork";
            }
        }
        else if (sent > 0) {
            text = inClipboard ?
                       fileCount(sent) + " copied to the host: paste with Ctrl+V" :
                       fileCount(sent) + " saved on the host in Downloads\\streamfork";
        }
        if (!lastError.isEmpty() && !cancelled) {
            text += (text.isEmpty() ? "" : "\n") + lastError;
        }
        if (skippedFolders > 0) {
            text += (text.isEmpty() ? "" : "\n") + QString("Folders are not supported yet");
        }
        if (!text.isEmpty()) {
            FileDrop::postStatus(text, lastError.isEmpty() ? k_MessageMs : k_ErrorMessageMs);
        }
    }

    // Sends one chunk, retrying after network errors. The host tells us how much it
    // received when we retry, so a chunk that arrived but whose reply was lost isn't
    // written twice. Returns false with reply set to the last failure.
    bool sendChunk(const QString& id, qint64 offset, const QByteArray& chunk, UploadReply& reply)
    {
        for (int attempt = 0;; attempt++) {
            reply = post("data", {{"id", id}, {"offset", QString::number(offset)}}, chunk, k_DataTimeoutMs);
            if (reply.ok()) {
                return true;
            }
            if (aborted()) {
                return false;
            }

            if (!reply.networkError && reply.status == 409) {
                qint64 received = reply.values["received"].toLongLong();
                if (received == offset + chunk.size()) {
                    // The previous attempt made it, only its reply was lost
                    return true;
                }
                if (received != offset) {
                    return false;
                }
            }
            else if (!reply.networkError) {
                // The host refused the chunk: retrying won't help
                return false;
            }

            if (attempt >= k_MaxRetries) {
                return false;
            }

            int delayMs = k_FirstRetryDelayMs << attempt;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "File upload chunk at offset %lld failed (%s), retrying in %d ms",
                        (long long)offset, qPrintable(reply.message), delayMs);
            if (!waitMs(delayMs)) {
                return false;
            }
        }
    }

    // On failure, error is set to a message for the user, prefixed with '!' if
    // the rest of the batch should be abandoned
    bool uploadFile(const QFileInfo& info, uint32_t batch, int index, int total, QString& error)
    {
        QString name = info.fileName();

        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            error = "Unable to read " + displayName(name);
            return false;
        }
        qint64 size = file.size();

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Uploading dropped file %d/%d to the host (%lld bytes)",
                    index, total, (long long)size);

        UploadReply reply = post("begin",
                                 {{"batch", QString::number(batch)},
                                  {"name", name},
                                  {"size", QString::number(size)}},
                                 QByteArray(), k_RequestTimeoutMs);
        if (!reply.ok()) {
            if (aborted()) {
                return false;
            }
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "File upload refused by the host: status %d, %s",
                        reply.status, qPrintable(reply.message));
            error = (isFatalForBatch(reply) ? "!" : "") +
                    QString("Unable to send ") + displayName(name) + ": " + describeFailure(reply);
            return false;
        }

        QString id = reply.values["id"];
        qint64 maxChunk = reply.values["chunk_max"].toLongLong();
        if (maxChunk <= 0) {
            maxChunk = k_DefaultMaxChunkSize;
        }
        maxChunk = maxChunkForRate(maxChunk);

        QCryptographicHash sha256(QCryptographicHash::Sha256);
        qint64 chunkSize = qMin(k_InitialChunkSize, maxChunk);
        qint64 offset = 0;

        QElapsedTimer elapsed;
        elapsed.start();
        qint64 lastProgressMs = -(qint64)k_ProgressIntervalMs;

        while (offset < size) {
            if (!throttle()) {
                cancelOnHost(id);
                return false;
            }

            if (elapsed.elapsed() - lastProgressMs >= k_ProgressIntervalMs) {
                lastProgressMs = elapsed.elapsed();
                double rate = lastProgressMs > 0 ? offset * 1000.0 / lastProgressMs : 0;
                // Concatenated rather than QString::arg(), which would also expand "%1" in file names
                FileDrop::postStatus("Sending " + QString::number(index) + "/" + QString::number(total) +
                                         ": " + displayName(name) + " - " +
                                         QString::number(size > 0 ? (int)(offset * 100 / size) : 0) + "%" +
                                         (rate > 0 ? " (" + formatRate(rate) + ")" : QString()),
                                     0);
            }

            QByteArray chunk = file.read(qMin(chunkSize, size - offset));
            if (chunk.isEmpty()) {
                cancelOnHost(id);
                error = "Unable to read " + displayName(name);
                return false;
            }

            QElapsedTimer chunkTimer;
            chunkTimer.start();

            if (!sendChunk(id, offset, chunk, reply)) {
                if (!aborted()) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "File upload failed at offset %lld: status %d, %s",
                                (long long)offset, reply.status, qPrintable(reply.message));
                    error = (isFatalForBatch(reply) ? "!" : "") +
                            QString("Unable to send ") + displayName(name) + ": " + describeFailure(reply);
                }
                cancelOnHost(id);
                return false;
            }

            // Only hash what the host confirmed, so the checksum matches after a retry
            sha256.addData(chunk);
            offset += chunk.size();
            m_RateBytes += chunk.size();

            // Aim for about one request per second: big chunks on fast links, small
            // ones on slow links so that progress and cancellation stay responsive
            qint64 ms = qMax<qint64>(chunkTimer.elapsed(), 1);
            qint64 ideal = chunk.size() * k_TargetChunkMs / ms;
            chunkSize = qBound(k_MinChunkSize, qMin(ideal, chunkSize * 2), maxChunk);
        }

        QString sha256Hex = QString::fromLatin1(sha256.result().toHex());
        bool lostReply = false;
        for (int attempt = 0;; attempt++) {
            reply = post("end", {{"id", id}, {"sha256", sha256Hex}}, QByteArray(), k_RequestTimeoutMs);
            if (reply.ok() || !reply.networkError || aborted() || attempt >= k_MaxRetries) {
                break;
            }
            lostReply = true;
            if (!waitMs(k_FirstRetryDelayMs << attempt)) {
                break;
            }
        }

        if (!reply.ok()) {
            if (lostReply && !reply.networkError && reply.status == 404) {
                // An earlier attempt finished the upload but its reply was lost
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Assuming the dropped file was saved: the reply to its last request was lost");
                return true;
            }
            if (!aborted()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "File upload could not be completed: status %d, %s",
                            reply.status, qPrintable(reply.message));
                error = QString("Unable to send ") + displayName(name) + ": " + describeFailure(reply);
            }
            cancelOnHost(id);
            return false;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Dropped file %d/%d uploaded in %lld ms",
                    index, total, (long long)elapsed.elapsed());
        return true;
    }

    const UploadSettings m_Settings;

    QMutex m_Mutex;
    QWaitCondition m_Wake;
    std::deque<QStringList> m_Queue;
    bool m_Stopping;
    bool m_Busy = false;
    unsigned m_CancelGeneration;
    unsigned m_BatchGeneration;

    // Only used on the upload thread
    QNetworkAccessManager* m_Nam;
    QElapsedTimer m_RateTimer;
    qint64 m_RateBytes;
};

FileDrop::FileDrop() :
    m_Enabled(false),
    m_ToClipboard(true),
    m_MaxMbps(0),
    m_Computer(nullptr),
    m_Thread(nullptr),
    m_DropInProgress(false),
    m_StatusGeneration(0),
    m_HideTimer(0)
{

}

FileDrop::~FileDrop()
{
    stop();
}

uint32_t FileDrop::start(bool enabled, bool toClipboard, int maxMbps, NvComputer* computer)
{
    stop();

    m_Enabled = enabled;
    m_ToClipboard = toClipboard;
    m_MaxMbps = maxMbps;
    m_Computer = computer;
    m_DropInProgress = false;
    m_PendingPaths.clear();

    return enabled ? STREAMFORK_CAP_FILE_UPLOAD : 0;
}

void FileDrop::stop()
{
    if (m_Thread != nullptr) {
        m_Thread->requestStop();
        m_Thread->wait();
        delete m_Thread;
        m_Thread = nullptr;
    }

    if (m_HideTimer != 0) {
        SDL_RemoveTimer(m_HideTimer);
        m_HideTimer = 0;
    }

    m_PendingPaths.clear();
    m_DropInProgress = false;
}

bool FileDrop::cancelUploads()
{
    if (m_Thread == nullptr || !m_Thread->requestCancel()) {
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "File upload cancelled by the user");
    return true;
}

void FileDrop::handleDropEvent(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_DROPBEGIN:
        m_DropInProgress = true;
        m_PendingPaths.clear();
        break;

    case SDL_DROPFILE:
        if (event.drop.file != nullptr) {
            m_PendingPaths.append(QString::fromUtf8(event.drop.file));
            SDL_free(event.drop.file);
        }

        // Platforms that don't send SDL_DROPBEGIN/SDL_DROPCOMPLETE deliver one file at a time
        if (!m_DropInProgress) {
            submitPendingDrop();
        }
        break;

    case SDL_DROPTEXT:
        // Dropped text is not a file: ignore it
        SDL_free(event.drop.file);
        break;

    case SDL_DROPCOMPLETE:
        m_DropInProgress = false;
        submitPendingDrop();
        break;
    }
}

void FileDrop::submitPendingDrop()
{
    QStringList paths = m_PendingPaths;
    m_PendingPaths.clear();

    if (paths.isEmpty() || !m_Enabled) {
        return;
    }

    if (!(LiGetStreamforkCapabilities() & STREAMFORK_CAP_FILE_UPLOAD) || m_Computer == nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Ignoring %d dropped file(s): the host doesn't accept files",
                    (int)paths.size());
        postStatus("The host doesn't accept files", k_MessageMs);
        return;
    }

    if (m_Thread == nullptr) {
        UploadSettings settings;
        {
            QReadLocker lock(&m_Computer->lock);
            settings.address = m_Computer->activeAddress.address();
            settings.httpsPort = m_Computer->activeHttpsPort;
            settings.serverCert = m_Computer->serverCert;
        }
        settings.sslConfig = IdentityManager::get()->getSslConfig();
        settings.uniqueId = IdentityManager::get()->getUniqueId();
        settings.toClipboard = m_ToClipboard;
        settings.maxMbps = m_MaxMbps;

        m_Thread = new FileUploadThread(settings);
        m_Thread->start();
    }

    m_Thread->enqueue(paths);
}

void FileDrop::postStatus(const QString& text, Uint32 hideAfterMs)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_FILE_DROP_STATUS;
    event.user.data1 = SDL_strdup(text.toUtf8().constData());
    event.user.data2 = (void*)(uintptr_t)hideAfterMs;
    SDL_PushEvent(&event);
}

Uint32 FileDrop::hideTimerCallback(Uint32, void* param)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_FILE_DROP_HIDE_STATUS;
    event.user.data1 = param;
    SDL_PushEvent(&event);
    return 0;
}

void FileDrop::handleUserEvent(const SDL_UserEvent& event, Overlay::OverlayManager& overlayManager)
{
    switch (event.code) {
    case SDL_CODE_FILE_DROP_STATUS:
    {
        char* text = (char*)event.data1;
        Uint32 hideAfterMs = (Uint32)(uintptr_t)event.data2;

        if (m_HideTimer != 0) {
            SDL_RemoveTimer(m_HideTimer);
            m_HideTimer = 0;
        }
        m_StatusGeneration++;

        overlayManager.updateOverlayText(Overlay::OverlayStatusUpdate, text);
        overlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
        SDL_free(text);

        if (hideAfterMs != 0) {
            m_HideTimer = SDL_AddTimer(hideAfterMs, hideTimerCallback, (void*)(uintptr_t)m_StatusGeneration);
        }
        break;
    }

    case SDL_CODE_FILE_DROP_HIDE_STATUS:
        // A newer message may have replaced the one this timer was for
        if ((Uint32)(uintptr_t)event.data1 == m_StatusGeneration) {
            m_HideTimer = 0;
            overlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
        }
        break;
    }
}
