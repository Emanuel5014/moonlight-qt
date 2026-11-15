// SdlInputHandler integration of the streamfork local cursor (see LocalCursor)

#include "input.h"
#include "streaming/streamutils.h"

uint32_t SdlInputHandler::startLocalCursor(bool enabled)
{
    return m_LocalCursor.start(enabled);
}

void SdlInputHandler::handleStreamforkReady(uint32_t capabilities)
{
    m_LocalCursor.handleStreamforkReady(capabilities);
    updateLocalCursor();
}

void SdlInputHandler::handleLocalCursorEvent(const SDL_UserEvent& event)
{
    m_LocalCursor.handleUserEvent(event);

    if (m_LocalCursor.isActive()) {
        int mouseX, mouseY;
        SDL_GetMouseState(&mouseX, &mouseY);
        applyLocalCursorVisibility(isMouseInVideoRegion(mouseX, mouseY));
    }
}

void SdlInputHandler::updateLocalCursor()
{
    bool mouseEmulationActive = false;
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (m_GamepadState[i].mouseEmulationTimer != 0) {
            mouseEmulationActive = true;
        }
    }

    // The client cursor only follows the host cursor in absolute mouse mode while the mouse is
    // captured. Otherwise (relative mode, gamepad mouse emulation) the host draws it in the video.
    bool wasActive = m_LocalCursor.isActive();
    m_LocalCursor.setActive(m_AbsoluteMouseMode && isCaptureActive() && !SDL_GetRelativeMouseMode() && !mouseEmulationActive);
    if (!wasActive && !m_LocalCursor.isActive()) {
        return;
    }

    int mouseX, mouseY;
    SDL_GetMouseState(&mouseX, &mouseY);
    bool mouseInVideoRegion = isMouseInVideoRegion(mouseX, mouseY);

    if (m_LocalCursor.isActive()) {
        applyLocalCursorVisibility(mouseInVideoRegion);
    }
    else if (m_AbsoluteMouseMode && isCaptureActive()) {
        // Back to the original cursor visibility
        SDL_ShowCursor((mouseInVideoRegion && m_MouseCursorCapturedVisibilityState == SDL_DISABLE) ? SDL_DISABLE : SDL_ENABLE);
    }
}

void SdlInputHandler::applyLocalCursorVisibility(bool mouseInVideoRegion)
{
    if (!mouseInVideoRegion) {
        // Outside of the video (e.g. black bars), we show the normal cursor
        m_LocalCursor.restoreDefaultCursor();
        SDL_ShowCursor(SDL_ENABLE);
    }
    else if (!m_LocalCursor.hasHostShape()) {
        // The host draws the cursor in the video, so behave as without the local cursor
        m_LocalCursor.restoreDefaultCursor();
        SDL_ShowCursor(m_MouseCursorCapturedVisibilityState);
    }
    else if (m_LocalCursor.isHostCursorVisible()) {
        m_LocalCursor.applyHostCursor(getLocalCursorScale());
        SDL_ShowCursor(SDL_ENABLE);
    }
    else {
        // Hidden on the host (e.g. full-screen video or typing)
        SDL_ShowCursor(SDL_DISABLE);
    }
}

float SdlInputHandler::getLocalCursorScale()
{
    int windowWidth, windowHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
    if (m_StreamWidth <= 0 || m_StreamHeight <= 0 || windowWidth <= 0 || windowHeight <= 0) {
        return 1.0f;
    }

    SDL_Rect src, dst;
    src.x = src.y = 0;
    src.w = m_StreamWidth;
    src.h = m_StreamHeight;
    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    // The cursor gets the apparent size it has in the video: one host pixel becomes
    // as many window units as the video is scaled by.
    float scale = (float)dst.w / m_StreamWidth;

#if defined(Q_OS_WIN32) && SDL_VERSION_ATLEAST(2, 26, 0)
    // Windows cursors are sized in physical pixels, which may differ from window units if
    // SDL scales the window coordinates. On macOS (and Wayland/X11) cursor images use the same
    // units as the window size (points), so the scale above already gives the right size there.
    int pixelWidth, pixelHeight;
    SDL_GetWindowSizeInPixels(m_Window, &pixelWidth, &pixelHeight);
    if (pixelWidth > 0) {
        scale *= (float)pixelWidth / windowWidth;
    }
#endif

    return scale;
}
