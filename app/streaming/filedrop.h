#pragma once

#include <Limelight.h>
#include "SDL_compat.h"

#include <QStringList>

// SDL user event codes used by FileDrop. They must not collide with the
// SDL_CODE_* values defined in session.cpp, decoder.h, clipboardsync.h and localcursor.h.
#define SDL_CODE_FILE_DROP_STATUS 210
#define SDL_CODE_FILE_DROP_HIDE_STATUS 211

class NvComputer;
class FileUploadThread;

namespace Overlay {
class OverlayManager;
}

// Sends the files dropped on the stream window to the host using the streamfork
// file drop extension: the capability is negotiated in the streamfork Hello and
// the files are uploaded in chunks to the host's HTTPS server, so they never hold
// up the control stream. Hosts without the extension don't grant the capability,
// and then a drop only shows a message.
//
// All methods must be called on the SDL main thread.
class FileDrop
{
public:
    FileDrop();

    ~FileDrop();

    // Returns the capabilities to request from the host. Must be called before starting the connection.
    // toClipboard puts the files of each drop in the host clipboard; maxMbps limits the upload speed (0 = no limit).
    uint32_t start(bool enabled, bool toClipboard, int maxMbps, NvComputer* computer);

    // Cancels any upload in progress
    void stop();

    bool isEnabled() const;

    // Files are being sent or waiting to be sent
    bool isBusy() const;

    // Cancels the current and queued uploads, keeping the files already sent.
    // Returns false if nothing was being sent.
    bool cancelUploads();

    // SDL_DROPBEGIN, SDL_DROPFILE, SDL_DROPTEXT and SDL_DROPCOMPLETE
    void handleDropEvent(const SDL_Event& event);

    // SDL_USEREVENT with one of the SDL_CODE_FILE_DROP_* values above
    void handleUserEvent(const SDL_UserEvent& event, Overlay::OverlayManager& overlayManager);

    // Shows a message in the status overlay, hiding it after hideAfterMs (0 = keep it).
    // Thread-safe: the message is delivered to the main thread as an SDL event.
    static void postStatus(const QString& text, Uint32 hideAfterMs);

private:
    void submitPendingDrop();

    static Uint32 hideTimerCallback(Uint32 interval, void* param);

    bool m_Enabled;
    bool m_ToClipboard;
    int m_MaxMbps;
    NvComputer* m_Computer;
    FileUploadThread* m_Thread;

    bool m_DropInProgress;
    QStringList m_PendingPaths;

    // Only the latest message may be hidden by its timer
    Uint32 m_StatusGeneration;
    SDL_TimerID m_HideTimer;
};
