#pragma once

#include <Limelight.h>
#include "SDL_compat.h"

#include <QByteArray>

// SDL user event codes used by ClipboardSync. They must not collide
// with the SDL_CODE_* values defined in session.cpp and decoder.h.
#define SDL_CODE_CLIPBOARD_RECEIVED 200
#define SDL_CODE_STREAMFORK_READY 201
#define SDL_CODE_CLIPBOARD_SEND 202

// Keeps the clipboard in sync with the host using the streamfork clipboard
// extension of the control stream. Hosts without the extension never answer
// our request, so nothing happens with them.
//
// All methods except the cl*() callbacks must be called on the SDL main thread.
class ClipboardSync
{
public:
    ClipboardSync();

    ~ClipboardSync();

    // Must be called before starting the connection
    void start(bool enabled);

    void stop();

    // SDL_CLIPBOARDUPDATE
    void handleClipboardUpdate();

    // SDL_USEREVENT with one of the SDL_CODE_* values above
    void handleUserEvent(const SDL_UserEvent& event);

    // Connection listener callbacks (invoked on a moonlight-common-c thread)
    static void clClipboardReceived(uint8_t format, const void* data, uint32_t length);

    static void clStreamforkReady(uint32_t capabilities);

private:
    void sendClipboard();

    static Uint32 sendTimerCallback(Uint32 interval, void* param);

    bool m_Enabled;
    SDL_TimerID m_SendTimer;

    // Last text sent to or received from the host. Our own clipboard writes
    // trigger SDL_CLIPBOARDUPDATE too, so this keeps us from echoing them back.
    QByteArray m_LastSyncedText;
};
