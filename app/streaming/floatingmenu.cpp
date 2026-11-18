#include "floatingmenu.h"

#include "session.h"
#include "path.h"
#include "settings/streamingpreferences.h"

#include <QCoreApplication>

#include <algorithm>
#include <climits>
#include <cmath>

using namespace FloatingMenuLogic;

// Logical sizes (multiplied by the window scale)
#define FONT_SIZE 16
#define CHECKBOX_SIZE 12
#define CHECKBOX_BORDER 2
#define LABEL_OFFSET 22
#define ICON_BAR_WIDTH 18
#define ICON_BAR_HEIGHT 2
#define ICON_BAR_SPACING 5

// Publishing the surface recreates the overlay texture, so limit it while dragging
#define DRAG_PUBLISH_INTERVAL_MS 8

static const SDL_Color k_ButtonColor = {20, 20, 20, 160};
static const SDL_Color k_PanelColor = {20, 20, 20, 220};
static const SDL_Color k_HoverColor = {255, 255, 255, 40};
static const SDL_Color k_TextColor = {255, 255, 255, 255};
static const SDL_Color k_DisabledColor = {150, 150, 150, 255};

// Fills a rectangle with straight (non-premultiplied) alpha, leaving the
// corners outside of the given radius transparent
static void fillRoundedRect(SDL_Surface* surface, const SDL_Rect& rect, SDL_Color color, int radius)
{
    Uint32 pixel = SDL_MapRGBA(surface->format, color.r, color.g, color.b, color.a);

    for (int y = std::max(0, rect.y); y < std::min(surface->h, rect.y + rect.h); y++) {
        Uint32* row = (Uint32*)((Uint8*)surface->pixels + y * surface->pitch);
        for (int x = std::max(0, rect.x); x < std::min(surface->w, rect.x + rect.w); x++) {
            // Distance from the nearest corner circle center, if we're in a corner
            int dx = 0, dy = 0;
            if (x < rect.x + radius) {
                dx = rect.x + radius - x;
            }
            else if (x >= rect.x + rect.w - radius) {
                dx = x - (rect.x + rect.w - radius - 1);
            }
            if (y < rect.y + radius) {
                dy = rect.y + radius - y;
            }
            else if (y >= rect.y + rect.h - radius) {
                dy = y - (rect.y + rect.h - radius - 1);
            }

            if (dx > 0 && dy > 0 && dx * dx + dy * dy > radius * radius) {
                continue;
            }

            row[x] = pixel;
        }
    }
}

static void fillRect(SDL_Surface* surface, const SDL_Rect& rect, SDL_Color color)
{
    SDL_FillRect(surface, &rect, SDL_MapRGBA(surface->format, color.r, color.g, color.b, color.a));
}

FloatingMenu::FloatingMenu(Session* session, SDL_Window* window) :
    m_Session(session),
    m_Window(window),
    m_Metrics(scaledMetrics(1.0f)),
    m_Scale(1.0f),
    m_UiScale(1.0f),
    m_WinW(0),
    m_WinH(0),
    m_Supported(false),
    m_Hidden(false),
    m_ReleasedCapture(false),
    m_ShowedCursor(false),
    m_Dirty(false),
    m_LastPublishTicks(0),
    m_FontData(Path::readDataFile("ModeSeven.ttf")),
    m_Font(nullptr),
    m_FontSize(0)
{
#ifndef QT_NO_DEBUG
    SDL_assert(floatingMenuSelfTest() == 0);
#endif

    StreamingPreferences* prefs = StreamingPreferences::get();
    m_Controller.setPosition(prefs->floatingMenuPosX, prefs->floatingMenuPosY);

    rebuildRows();
    updateWindowMetrics();
}

FloatingMenu::~FloatingMenu()
{
    if (m_Supported) {
        m_Session->getOverlayManager().setOverlaySurface(Overlay::OverlayFloatingMenu, nullptr, 0, 0);
    }

    if (m_Font != nullptr) {
        TTF_CloseFont(m_Font);
    }
}

bool FloatingMenu::isHidden() const
{
    return m_Hidden;
}

void FloatingMenu::setHidden(bool hidden)
{
    m_Hidden = hidden;
    m_Controller.setButtonVisible(!hidden);
    if (hidden) {
        m_Controller.close();
    }
    runCommands();
    publish(true);
}

void FloatingMenu::toggle()
{
    if (!m_Supported) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Floating menu is not supported by this renderer");
        return;
    }

    if (m_Controller.isOpen()) {
        m_Controller.close();
    }
    else {
        m_Controller.open();
    }
    runCommands();
    publish(true);
}

void FloatingMenu::onWindowChanged()
{
    int oldW = m_WinW, oldH = m_WinH;
    float oldScale = m_Scale, oldUiScale = m_UiScale;

    updateWindowMetrics();

    if (oldW != m_WinW || oldH != m_WinH || oldScale != m_Scale || oldUiScale != m_UiScale) {
        publish(true);
    }
}

void FloatingMenu::onRendererChanged(bool supported)
{
    if (!supported && m_Supported) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Floating menu disabled: the renderer doesn't support it");
    }

    m_Supported = supported;
    if (!supported) {
        m_Controller.close();
        runCommands();
        return;
    }

    updateWindowMetrics();
    publish(true);
}

bool FloatingMenu::handleEvent(const SDL_Event& event)
{
    if (!m_Supported) {
        return false;
    }

    bool consumed = false;

    switch (event.type) {
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        // Touch is out of scope. With a captured relative mouse the cursor
        // position is meaningless, but the release of a press we consumed
        // (e.g. the click that closed the panel and recaptured the mouse)
        // must not reach the host either.
        if (event.button.which == SDL_TOUCH_MOUSEID) {
            return false;
        }
        if (SDL_GetRelativeMouseMode() && (event.type == SDL_MOUSEBUTTONDOWN || !m_Controller.hasPress())) {
            return false;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
            consumed = event.type == SDL_MOUSEBUTTONDOWN ?
                           m_Controller.mouseDown(toPixels(event.button.x), toPixels(event.button.y)) :
                           m_Controller.mouseUp(toPixels(event.button.x), toPixels(event.button.y));
        }
        else {
            consumed = event.type == SDL_MOUSEBUTTONDOWN ?
                           m_Controller.otherButtonDown(event.button.button, toPixels(event.button.x), toPixels(event.button.y)) :
                           m_Controller.otherButtonUp(event.button.button, toPixels(event.button.x), toPixels(event.button.y));
        }
        break;

    case SDL_MOUSEMOTION:
        if (event.motion.which == SDL_TOUCH_MOUSEID || SDL_GetRelativeMouseMode()) {
            return false;
        }
        consumed = m_Controller.mouseMotion(toPixels(event.motion.x), toPixels(event.motion.y));
        break;

    case SDL_MOUSEWHEEL:
    {
        if (event.wheel.which == SDL_TOUCH_MOUSEID || SDL_GetRelativeMouseMode()) {
            return false;
        }
        int dy = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            dy = -dy;
        }
        consumed = m_Controller.mouseWheel(toPixels(event.wheel.mouseX), toPixels(event.wheel.mouseY), dy);
        break;
    }

    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (event.key.keysym.scancode != SDL_SCANCODE_ESCAPE) {
            return false;
        }
        if (event.type == SDL_KEYDOWN) {
            consumed = m_Controller.escDown();
        }
        else {
            consumed = m_Controller.escUp();
        }
        break;

    default:
        return false;
    }

    runCommands();
    publish(false);
    return consumed;
}

void FloatingMenu::rebuildRows()
{
    m_Rows = {
        Row::Immersive,
        Row::FullScreen,
        Row::Stats,
        Row::MouseMode,
        Row::ShowCursor,
        Row::Minimize,
        Row::HideButton,
    };
    if (m_Session->m_FileDrop.isEnabled()) {
        // Enabled only while files are being sent
        m_Rows.push_back(Row::CancelFileTransfer);
    }
    m_Rows.push_back(Row::Disconnect);
    m_Rows.push_back(Row::QuitAndExit);

    m_Controller.setRows(std::vector<bool>(m_Rows.size(), false));
}

void FloatingMenu::updateWindowMetrics()
{
    int pointsW = 0, pointsH = 0;
    SDL_GetWindowSize(m_Window, &pointsW, &pointsH);
    SDL_GetWindowSizeInPixels(m_Window, &m_WinW, &m_WinH);

    // Window points to pixels (2 on Retina displays, 1 on Windows)
    m_Scale = pointsW > 0 ? (float)m_WinW / pointsW : 1.0f;
    if (m_Scale <= 0) {
        m_Scale = 1.0f;
    }

    // Size of the menu itself. Windows reports pixels as points, so follow
    // the display scaling (DPI) there to keep the menu readable on HiDPI.
    m_UiScale = m_Scale;
#ifdef Q_OS_WIN32
    float hdpi = 0;
    if (SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(m_Window), nullptr, &hdpi, nullptr) == 0 && hdpi > 96.0f) {
        m_UiScale = hdpi / 96.0f;
    }
#endif

    m_Metrics = scaledMetrics(m_UiScale);
    m_Controller.setWindow(m_WinW, m_WinH, m_Metrics);
}

int FloatingMenu::toPixels(int points) const
{
    return (int)std::lround(points * m_Scale);
}

void FloatingMenu::runCommands()
{
    for (const Command& command : m_Controller.takeCommands()) {
        switch (command.kind) {
        case CommandKind::Opened:
            onOpened();
            break;
        case CommandKind::Closed:
            onClosed();
            break;
        case CommandKind::MoveFinished:
        {
            double posX, posY;
            m_Controller.getPosition(posX, posY);
            StreamingPreferences::get()->saveFloatingMenuPosition(posX, posY);
            m_Dirty = true;
            break;
        }
        case CommandKind::ActivateRow:
            if (command.row >= 0 && command.row < (int)m_Rows.size()) {
                activate(m_Rows[command.row]);
            }
            break;
        case CommandKind::StepRow:
            // No choice rows yet
            break;
        }
    }
}

void FloatingMenu::onOpened()
{
    SdlInputHandler* input = m_Session->m_InputHandler;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Floating menu opened");

    // The panel needs a visible local cursor
    if (input->isCaptureActive() && SDL_GetRelativeMouseMode()) {
        input->setCaptureActive(false);
        m_ReleasedCapture = true;
    }
    else if (SDL_ShowCursor(SDL_QUERY) == SDL_DISABLE) {
        SDL_ShowCursor(SDL_ENABLE);
        m_ShowedCursor = true;
    }
}

void FloatingMenu::onClosed()
{
    SdlInputHandler* input = m_Session->m_InputHandler;

    // Only undo what onOpened() did
    if (m_ReleasedCapture) {
        m_ReleasedCapture = false;
        input->setCaptureActive(true);
    }
    if (m_ShowedCursor) {
        m_ShowedCursor = false;
        SDL_ShowCursor(SDL_DISABLE);
    }
}

void FloatingMenu::closePanel()
{
    m_Controller.close();
    runCommands();
}

void FloatingMenu::activate(Row row)
{
    SdlInputHandler* input = m_Session->m_InputHandler;

    if (!rowEnabled(row)) {
        return;
    }

    switch (row) {
    case Row::Immersive:
        // Not KeyComboToggleKeyboardGrab: it looks at the live grab, which is
        // released while the panel holds the mouse capture
        input->m_CaptureSystemKeysMode = isImmersiveOn() ?
                                             StreamingPreferences::CSK_OFF :
                                             StreamingPreferences::CSK_ALWAYS;
        input->updateKeyboardGrabState();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Floating menu: immersive mode %s",
                    isImmersiveOn() ? "on" : "off");
        break;

    case Row::FullScreen:
        // These recapture the mouse themselves, so give it back first
        closePanel();
        input->performSpecialKeyCombo(SdlInputHandler::KeyComboToggleFullScreen);
        break;

    case Row::Stats:
        input->performSpecialKeyCombo(SdlInputHandler::KeyComboToggleStatsOverlay);
        break;

    case Row::MouseMode:
        closePanel();
        input->performSpecialKeyCombo(SdlInputHandler::KeyComboToggleMouseMode);
        break;

    case Row::ShowCursor:
        input->performSpecialKeyCombo(SdlInputHandler::KeyComboToggleCursorHide);
        // Keep the cursor visible while the panel is open
        if (!input->m_MouseCursorCapturedVisibilityState) {
            SDL_ShowCursor(SDL_ENABLE);
            m_ShowedCursor = true;
        }
        else {
            m_ShowedCursor = false;
        }
        break;

    case Row::Minimize:
        closePanel();
        input->performSpecialKeyCombo(SdlInputHandler::KeyComboToggleMinimize);
        break;

    case Row::HideButton:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Floating menu button hidden");
        setHidden(true);
        break;

    case Row::CancelFileTransfer:
        closePanel();
        m_Session->m_FileDrop.cancelUploads();
        break;

    case Row::Disconnect:
        closePanel();
        input->performSpecialKeyCombo(SdlInputHandler::KeyComboQuit);
        break;

    case Row::QuitAndExit:
        closePanel();
        input->performSpecialKeyCombo(SdlInputHandler::KeyComboQuitAndExit);
        break;
    }

    m_Dirty = true;
}

bool FloatingMenu::isImmersiveOn() const
{
    SdlInputHandler* input = m_Session->m_InputHandler;

    switch (input->m_CaptureSystemKeysMode) {
    case StreamingPreferences::CSK_ALWAYS:
        return true;
    case StreamingPreferences::CSK_FULLSCREEN:
        return (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN) != 0;
    default:
        return false;
    }
}

QString FloatingMenu::rowLabel(Row row) const
{
    switch (row) {
    case Row::Immersive:
        return QCoreApplication::translate("FloatingMenu", "Immersive mode");
    case Row::FullScreen:
        return QCoreApplication::translate("FloatingMenu", "Full screen");
    case Row::Stats:
        return QCoreApplication::translate("FloatingMenu", "Statistics");
    case Row::MouseMode:
        return QCoreApplication::translate("FloatingMenu", "Remote desktop mouse");
    case Row::ShowCursor:
        return QCoreApplication::translate("FloatingMenu", "Show cursor");
    case Row::Minimize:
        return QCoreApplication::translate("FloatingMenu", "Minimize");
    case Row::HideButton:
        return QCoreApplication::translate("FloatingMenu", "Hide button");
    case Row::CancelFileTransfer:
        return QCoreApplication::translate("FloatingMenu", "Cancel file transfer");
    case Row::Disconnect:
        return QCoreApplication::translate("FloatingMenu", "Disconnect");
    case Row::QuitAndExit:
        return QCoreApplication::translate("FloatingMenu", "Quit app and exit");
    }
    return QString();
}

bool FloatingMenu::rowHasCheck(Row row) const
{
    switch (row) {
    case Row::Immersive:
    case Row::FullScreen:
    case Row::Stats:
    case Row::MouseMode:
    case Row::ShowCursor:
        return true;
    default:
        return false;
    }
}

bool FloatingMenu::rowChecked(Row row) const
{
    SdlInputHandler* input = m_Session->m_InputHandler;

    switch (row) {
    case Row::Immersive:
        return isImmersiveOn();
    case Row::FullScreen:
        return m_Session->m_IsFullScreen;
    case Row::Stats:
        return m_Session->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug);
    case Row::MouseMode:
        return input->m_AbsoluteMouseMode;
    case Row::ShowCursor:
        return input->m_AbsoluteMouseMode && input->m_MouseCursorCapturedVisibilityState;
    default:
        return false;
    }
}

bool FloatingMenu::rowEnabled(Row row) const
{
    if (row == Row::ShowCursor) {
        // The local cursor can only be shown in remote desktop mouse mode
        return m_Session->m_InputHandler->m_AbsoluteMouseMode;
    }
    if (row == Row::CancelFileTransfer) {
        return m_Session->m_FileDrop.isBusy();
    }
    return true;
}

void FloatingMenu::ensureFont()
{
    int size = std::max(1, (int)std::lround(FONT_SIZE * m_UiScale));
    if (m_Font != nullptr && m_FontSize == size) {
        return;
    }

    if (m_Font != nullptr) {
        TTF_CloseFont(m_Font);
        m_Font = nullptr;
    }

    if (m_FontData.isEmpty()) {
        return;
    }

    // m_FontData must stay around until the font is closed
    m_Font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()), 1, size);
    if (m_Font == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Floating menu: TTF_OpenFont() failed: %s",
                    TTF_GetError());
        return;
    }
    m_FontSize = size;
}

SDL_Surface* FloatingMenu::render(int& x, int& y)
{
    bool buttonVisible = !m_Hidden;
    bool panelVisible = m_Controller.isOpen();
    if (!buttonVisible && !panelVisible) {
        return nullptr;
    }

    Rect button = m_Controller.button();
    Rect panel = m_Controller.panel();

    // The surface covers the union of the visible parts
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    for (const Rect& r : {buttonVisible ? button : panel, panelVisible ? panel : button}) {
        left = std::min(left, r.x);
        top = std::min(top, r.y);
        right = std::max(right, r.x + r.w);
        bottom = std::max(bottom, r.y + r.h);
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, right - left, bottom - top, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Floating menu: SDL_CreateRGBSurfaceWithFormat() failed: %s",
                     SDL_GetError());
        return nullptr;
    }
    SDL_FillRect(surface, nullptr, 0);

    auto scaled = [this](int logical) {
        return std::max(1, (int)std::lround(logical * m_UiScale));
    };

    if (buttonVisible) {
        SDL_Rect b = { button.x - left, button.y - top, button.w, button.h };
        fillRoundedRect(surface, b, k_ButtonColor, m_Metrics.cornerRadius);

        // Three bars icon
        int barW = scaled(ICON_BAR_WIDTH), barH = scaled(ICON_BAR_HEIGHT), spacing = scaled(ICON_BAR_SPACING);
        int barX = b.x + (b.w - barW) / 2;
        int barY = b.y + (b.h - (barH + 2 * spacing)) / 2;
        for (int i = 0; i < 3; i++) {
            fillRect(surface, { barX, barY + i * spacing, barW, barH }, k_TextColor);
        }
    }

    if (panelVisible) {
        SDL_Rect p = { panel.x - left, panel.y - top, panel.w, panel.h };
        fillRoundedRect(surface, p, k_PanelColor, m_Metrics.cornerRadius);

        ensureFont();

        int checkSize = scaled(CHECKBOX_SIZE);
        int checkBorder = scaled(CHECKBOX_BORDER);
        for (int i = 0; i < (int)m_Rows.size(); i++) {
            Row row = m_Rows[i];
            SDL_Rect r = { p.x, p.y + m_Metrics.padding + i * m_Metrics.rowHeight, p.w, m_Metrics.rowHeight };
            bool enabled = rowEnabled(row);
            SDL_Color color = enabled ? k_TextColor : k_DisabledColor;

            if (i == m_Controller.hoveredRow() && enabled) {
                fillRect(surface, r, k_HoverColor);
            }

            if (rowHasCheck(row)) {
                SDL_Rect box = { r.x + m_Metrics.padding, r.y + (r.h - checkSize) / 2, checkSize, checkSize };
                fillRect(surface, box, color);
                if (!rowChecked(row)) {
                    SDL_Rect inner = { box.x + checkBorder, box.y + checkBorder, box.w - 2 * checkBorder, box.h - 2 * checkBorder };
                    fillRect(surface, inner, k_PanelColor);
                }
            }

            if (m_Font != nullptr) {
                SDL_Surface* text = TTF_RenderUTF8_Blended(m_Font, rowLabel(row).toUtf8().constData(), color);
                if (text != nullptr) {
                    SDL_Rect dst = { r.x + m_Metrics.padding + scaled(LABEL_OFFSET), r.y + (r.h - text->h) / 2, text->w, text->h };
                    SDL_SetSurfaceBlendMode(text, SDL_BLENDMODE_BLEND);
                    SDL_BlitSurface(text, nullptr, surface, &dst);
                    SDL_FreeSurface(text);
                }
            }
        }
    }

    x = left;
    y = top;
    return surface;
}

void FloatingMenu::publish(bool force)
{
    if (!m_Supported) {
        return;
    }

    if (m_Controller.takeRedraw()) {
        m_Dirty = true;
    }
    if (!m_Dirty && !force) {
        return;
    }

    Uint32 now = SDL_GetTicks();
    if (!force && m_Controller.isDragging() && now - m_LastPublishTicks < DRAG_PUBLISH_INTERVAL_MS) {
        // Keep m_Dirty set: the next event (at the latest the button release) publishes it
        return;
    }

    int x = 0, y = 0;
    SDL_Surface* surface = render(x, y);
    m_Session->getOverlayManager().setOverlaySurface(Overlay::OverlayFloatingMenu, surface, x, y);
    m_Dirty = false;
    m_LastPublishTicks = now;

    // Overlays only show up with a presented frame, and the host may not be
    // sending any (static desktop).
    // NB: No m_DecoderLock here: the decoder is only created and destroyed on
    // this thread, and holding it makes the decode thread drop decode units.
    if (m_Session->m_VideoDecoder != nullptr) {
        m_Session->m_VideoDecoder->redrawLastFrame();
    }
}
