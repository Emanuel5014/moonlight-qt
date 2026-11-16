#pragma once

// Qt first: qsimd.h redefines __SSE__ unconditionally, SDL_cpuinfo.h doesn't
#include <QByteArray>
#include <QHash>
#include <QList>

#include <Limelight.h>
#include "SDL_compat.h"

// SDL user event codes used by LocalCursor. They must not collide with the
// SDL_CODE_* values defined in session.cpp, decoder.h and clipboardsync.h.
#define SDL_CODE_CURSOR_SHAPE 203
#define SDL_CODE_CURSOR_STATE 204

// A cursor image in BGRA byte order with straight (non-premultiplied) alpha
struct LocalCursorImage
{
    int width = 0;
    int height = 0;
    int hotX = 0;
    int hotY = 0;
    QByteArray bgra;
};

// Scales a cursor image (bilinear, alpha-correct). The result is clamped to 1..maxSize pixels per side.
LocalCursorImage scaleLocalCursorImage(const LocalCursorImage& image, float scale, int maxSize = 512);

// Moves id to the front of a most-recently-used list. Returns the id that fell
// off the end when the list grows beyond capacity, or 0.
uint32_t touchLocalCursorLru(QList<uint32_t>& lru, uint32_t id, int capacity);

#ifndef QT_NO_DEBUG
// Asserts the expected behavior of the pure helpers above in debug builds
void localCursorSelfTest();
#endif

// Draws the host cursor as a native cursor on the client using the shapes sent by
// streamfork hosts (STREAMFORK_CAP_LOCAL_CURSOR). The host then leaves the cursor out
// of the video, so mouse movements show up without waiting for the video round trip.
// Hosts without the extension never grant it, so nothing changes with them.
//
// All methods except the cl*() callbacks must be called on the SDL main thread.
class LocalCursor
{
public:
    LocalCursor();

    ~LocalCursor();

    // Returns the capabilities to request from the host. Must be called before starting the connection.
    uint32_t start(bool enabled);

    // The host acknowledged our streamfork capabilities
    void handleStreamforkReady(uint32_t capabilities);

    // SDL_USEREVENT with SDL_CODE_CURSOR_SHAPE or SDL_CODE_CURSOR_STATE
    void handleUserEvent(const SDL_UserEvent& event);

    // Turns the local cursor on or off (only possible once the host granted it) and tells the host
    void setActive(bool active);

    bool isActive() const { return m_Active; }

    // True when the user enabled the local cursor and the host supports it
    bool isGranted() const { return m_Granted; }

    // False when the host draws the cursor in the video (no shape available yet, or too large)
    bool hasHostShape() const { return m_StateShapeId != 0; }

    bool isHostCursorVisible() const { return m_StateVisible; }

    // Shows the host cursor shape, scaled by the given factor (cursor pixels per host pixel)
    void applyHostCursor(float scale);

    // Restores the system default cursor
    void restoreDefaultCursor();

    // Connection listener callbacks (invoked on a moonlight-common-c thread)
    static void clCursorShape(uint32_t shapeId, uint16_t width, uint16_t height,
                              uint16_t hotX, uint16_t hotY, const void* bgra);

    static void clCursorState(bool visible, uint32_t shapeId);

private:
    struct Shape {
        LocalCursorImage image;
        SDL_Cursor* cursor = nullptr; // Created on demand for cursorScale
        float cursorScale = 0;
    };

    void freeShapes();

    bool m_Enabled;
    bool m_Granted;
    bool m_Active;

    uint32_t m_StateShapeId;
    bool m_StateVisible;
    bool m_UsingHostCursor;
    bool m_WarnedUnknownShape;

    QHash<uint32_t, Shape> m_Shapes;
    QList<uint32_t> m_Lru; // Most recently used first, in the same order as the host
};
