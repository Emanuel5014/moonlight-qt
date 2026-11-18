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

#include <atomic>
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

struct HostInfo
{
    QString address;
    uint16_t httpsPort;
    QSslCertificate serverCert;
    QSslConfiguration sslConfig;
    QString uniqueId;
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
    explicit FileUploadThread(const HostInfo& host) :
        m_Host(host),
        m_Stopping(false),
        m_Nam(nullptr)
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
                if (m_Host.serverCert.isNull() || m_Host.serverCert != error.certificate()) {
                    return;
                }
            }
            reply->ignoreSslErrors(errors);
        });

        for (;;) {
            QStringList paths;
            {
                QMutexLocker locker(&m_Mutex);
                while (m_Queue.empty() && !m_Stopping) {
                    m_Wake.wait(&m_Mutex);
                }
                if (m_Stopping) {
                    break;
                }
                paths = m_Queue.front();
                m_Queue.pop_front();
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

    UploadReply post(const QString& command, const QList<QPair<QString, QString>>& args,
                     const QByteArray& body, int timeoutMs, bool abortOnStop = true)
    {
        QUrl url;
        url.setScheme("https");
        url.setHost(m_Host.address);
        url.setPort(m_Host.httpsPort);
        url.setPath("/actions/upload/" + command);

        // Encode everything but unreserved characters: the host decodes '+' as a space
        QString query = "uniqueid=" + m_Host.uniqueId +
                        "&uuid=" + QUuid::createUuid().toRfc4122().toHex();
        for (const auto& arg : args) {
            query += "&" + arg.first + "=" + QString::fromLatin1(QUrl::toPercentEncoding(arg.second));
        }
        url.setQuery(query, QUrl::StrictMode);

        QNetworkRequest request(url);
        request.setSslConfiguration(m_Host.sslConfig);
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

        // Check for stop requests while the request is in flight
        QTimer stopCheck;
        QObject::connect(&stopCheck, &QTimer::timeout, &loop, [&]() {
            if (abortOnStop && stopping()) {
                loop.quit();
            }
        });
        stopCheck.start(100);

        loop.exec(QEventLoop::ExcludeUserInputEvents);

        UploadReply result;
        if (!reply->isFinished()) {
            reply->abort();
            result.message = stopping() ? "Cancelled" : "Request timed out";
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
        // Best effort: the host also drops abandoned uploads on its own
        post("cancel", {{"id", id}}, QByteArray(), k_CancelTimeoutMs, false);
    }

    void uploadBatch(const QStringList& paths)
    {
        uint32_t batch = QRandomGenerator::global()->generate();
        int total = paths.size();
        int sent = 0;
        int skippedFolders = 0;
        QString lastError;

        for (int i = 0; i < total && !stopping(); i++) {
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

        QString text;
        if (sent > 0) {
            text = (sent == 1 ? QString("1 file") : QString::number(sent) + " files") +
                   " saved on the host in Downloads\\streamfork";
        }
        if (!lastError.isEmpty()) {
            text += (text.isEmpty() ? "" : "\n") + lastError;
        }
        if (skippedFolders > 0) {
            text += (text.isEmpty() ? "" : "\n") + QString("Folders are not supported yet");
        }
        if (!text.isEmpty()) {
            FileDrop::postStatus(text, lastError.isEmpty() ? k_MessageMs : k_ErrorMessageMs);
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
            if (stopping()) {
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

        QCryptographicHash sha256(QCryptographicHash::Sha256);
        qint64 chunkSize = qMin(k_InitialChunkSize, maxChunk);
        qint64 offset = 0;

        QElapsedTimer elapsed;
        elapsed.start();
        qint64 lastProgressMs = -(qint64)k_ProgressIntervalMs;

        while (offset < size) {
            if (stopping()) {
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
            sha256.addData(chunk);

            QElapsedTimer chunkTimer;
            chunkTimer.start();

            reply = post("data", {{"id", id}, {"offset", QString::number(offset)}}, chunk, k_DataTimeoutMs);
            if (!reply.ok()) {
                if (!stopping()) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "File upload failed at offset %lld: status %d, %s",
                                (long long)offset, reply.status, qPrintable(reply.message));
                    error = (isFatalForBatch(reply) ? "!" : "") +
                            QString("Unable to send ") + displayName(name) + ": " + describeFailure(reply);
                }
                cancelOnHost(id);
                return false;
            }
            offset += chunk.size();

            // Aim for about one request per second: big chunks on fast links, small
            // ones on slow links so that progress and cancellation stay responsive
            qint64 ms = qMax<qint64>(chunkTimer.elapsed(), 1);
            qint64 ideal = chunk.size() * k_TargetChunkMs / ms;
            chunkSize = qBound(k_MinChunkSize, qMin(ideal, chunkSize * 2), maxChunk);
        }

        reply = post("end", {{"id", id}, {"sha256", QString::fromLatin1(sha256.result().toHex())}},
                     QByteArray(), k_RequestTimeoutMs);
        if (!reply.ok()) {
            if (!stopping()) {
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

    const HostInfo m_Host;

    QMutex m_Mutex;
    QWaitCondition m_Wake;
    std::deque<QStringList> m_Queue;
    bool m_Stopping;

    // Only used on the upload thread
    QNetworkAccessManager* m_Nam;
};

FileDrop::FileDrop() :
    m_Enabled(false),
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

uint32_t FileDrop::start(bool enabled, NvComputer* computer)
{
    stop();

    m_Enabled = enabled;
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
        HostInfo host;
        {
            QReadLocker lock(&m_Computer->lock);
            host.address = m_Computer->activeAddress.address();
            host.httpsPort = m_Computer->activeHttpsPort;
            host.serverCert = m_Computer->serverCert;
        }
        host.sslConfig = IdentityManager::get()->getSslConfig();
        host.uniqueId = IdentityManager::get()->getUniqueId();

        m_Thread = new FileUploadThread(host);
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
