#include "clipboardsync.h"

// Coalesce bursts of clipboard updates (some apps set the clipboard several times)
#define CLIPBOARD_SEND_DELAY_MS 30

// Same limit as moonlight-common-c for text
#define CLIPBOARD_MAX_TEXT_LENGTH (1024 * 1024)

// The wire format uses LF line endings
static QByteArray normalizeLineEndings(const char* text)
{
    QByteArray normalized(text);
    normalized.replace("\r\n", "\n");
    return normalized;
}

ClipboardSync::ClipboardSync()
    : m_Enabled(false),
      m_SendTimer(0)
{
}

ClipboardSync::~ClipboardSync()
{
    stop();
}

void ClipboardSync::start(bool enabled)
{
    stop();

    m_Enabled = enabled;
    m_LastSyncedText.clear();

    LiSetStreamforkCapabilities(enabled ?
                                    (STREAMFORK_CAP_CLIPBOARD_TEXT |
                                     STREAMFORK_CAP_CLIPBOARD_TO_CLIENT |
                                     STREAMFORK_CAP_CLIPBOARD_TO_HOST) :
                                    0);
}

void ClipboardSync::stop()
{
    if (m_SendTimer != 0) {
        SDL_RemoveTimer(m_SendTimer);
        m_SendTimer = 0;
    }
}

void ClipboardSync::handleClipboardUpdate()
{
    if (!m_Enabled || !(LiGetStreamforkCapabilities() & STREAMFORK_CAP_CLIPBOARD_TO_HOST)) {
        return;
    }

    if (m_SendTimer == 0) {
        m_SendTimer = SDL_AddTimer(CLIPBOARD_SEND_DELAY_MS, sendTimerCallback, this);
    }
}

void ClipboardSync::handleUserEvent(const SDL_UserEvent& event)
{
    switch (event.code) {
    case SDL_CODE_CLIPBOARD_RECEIVED:
    {
        QByteArray text((const char*)event.data1, (int)(uintptr_t)event.data2);
        SDL_free(event.data1);

        if (!m_Enabled) {
            break;
        }

        // Remember it before setting it, so the resulting SDL_CLIPBOARDUPDATE isn't sent back
        m_LastSyncedText = text;
        if (SDL_SetClipboardText(text.constData()) != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to set clipboard text from host: %s",
                        SDL_GetError());
        }
        break;
    }

    case SDL_CODE_STREAMFORK_READY:
    {
        uint32_t capabilities = (uint32_t)(uintptr_t)event.data1;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard sync with host: %s%s",
                    (capabilities & STREAMFORK_CAP_CLIPBOARD_TO_HOST) ? "client->host " : "",
                    (capabilities & STREAMFORK_CAP_CLIPBOARD_TO_CLIENT) ? "host->client" : "");

        // Like Parsec, make what the user copied before connecting available on the host
        if (m_Enabled && (capabilities & STREAMFORK_CAP_CLIPBOARD_TO_HOST)) {
            sendClipboard();
        }
        break;
    }

    case SDL_CODE_CLIPBOARD_SEND:
        m_SendTimer = 0;
        sendClipboard();
        break;

    default:
        SDL_assert(false);
        break;
    }
}

void ClipboardSync::sendClipboard()
{
    if (!m_Enabled || !SDL_HasClipboardText()) {
        return;
    }

    char* rawText = SDL_GetClipboardText();
    if (rawText == nullptr) {
        return;
    }
    QByteArray text = normalizeLineEndings(rawText);
    SDL_free(rawText);

    if (text.isEmpty() || text == m_LastSyncedText) {
        return;
    }

    if (text.size() > CLIPBOARD_MAX_TEXT_LENGTH) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard text too large to send to the host (%d bytes)",
                    (int)text.size());
        return;
    }

    m_LastSyncedText = text;

    // Never log the content of the clipboard
    int err = LiSendClipboard(LI_CLIPBOARD_TEXT, text.constData(), (uint32_t)text.size());
    if (err != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to send clipboard to host: %d",
                    err);
    }
}

Uint32 ClipboardSync::sendTimerCallback(Uint32, void*)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_CLIPBOARD_SEND;
    SDL_PushEvent(&event);

    // One-shot timer
    return 0;
}

void ClipboardSync::clClipboardReceived(uint8_t format, const void* data, uint32_t length)
{
    if (format != LI_CLIPBOARD_TEXT || length > CLIPBOARD_MAX_TEXT_LENGTH) {
        return;
    }

    // The data is only valid during the callback, so copy it for the main thread
    char* text = (char*)SDL_malloc(length + 1);
    if (text == nullptr) {
        return;
    }
    SDL_memcpy(text, data, length);
    text[length] = 0;

    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_CLIPBOARD_RECEIVED;
    event.user.data1 = text;
    event.user.data2 = (void*)(uintptr_t)length;
    if (SDL_PushEvent(&event) <= 0) {
        SDL_free(text);
    }
}

void ClipboardSync::clStreamforkReady(uint32_t capabilities)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_STREAMFORK_READY;
    event.user.data1 = (void*)(uintptr_t)capabilities;
    SDL_PushEvent(&event);
}
