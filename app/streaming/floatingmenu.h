#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

#include "SDL_compat.h"
#include <SDL_ttf.h>

#include "floatingmenulogic.h"

class Session;

// Floating stream menu (streamfork spec 08): a draggable button drawn over the
// video that opens a panel with stream actions. Lives on the main (SDL event)
// thread. The geometry and input rules are in floatingmenulogic.h.
class FloatingMenu
{
public:
    FloatingMenu(Session* session, SDL_Window* window);
    ~FloatingMenu();

    // Returns true if the event belongs to the menu and must not reach the host
    bool handleEvent(const SDL_Event& event);

    // Opens or closes the panel (menu shortcut)
    void toggle();

    // Window resized, moved between displays, full-screen toggled...
    void onWindowChanged();

    // The video renderer was (re)created. Without renderer support the menu
    // is neither drawn nor consumes any event.
    void onRendererChanged(bool supported);

    bool isHidden() const;
    void setHidden(bool hidden);

private:
    enum class Row {
        Immersive,
        FullScreen,
        Stats,
        MouseMode,
        ShowCursor,
        Minimize,
        HideButton,
        CancelFileTransfer, // streamfork spec 10
        Disconnect,
        QuitAndExit,
    };

    void rebuildRows();
    void updateWindowMetrics();
    void runCommands();
    void activate(Row row);
    void onOpened();
    void onClosed();
    void closePanel();
    void publish(bool force);
    SDL_Surface* render(int& x, int& y);
    void ensureFont();

    QString rowLabel(Row row) const;
    bool rowHasCheck(Row row) const;
    bool rowChecked(Row row) const;
    bool rowEnabled(Row row) const;
    bool isImmersiveOn() const;

    int toPixels(int points) const;

    Session* m_Session;
    SDL_Window* m_Window;
    FloatingMenuLogic::MenuController m_Controller;
    FloatingMenuLogic::Metrics m_Metrics;
    std::vector<Row> m_Rows;
    float m_Scale;   // Window pixels per point (input coordinates)
    float m_UiScale; // Menu size multiplier
    int m_WinW;
    int m_WinH;
    bool m_Supported;
    bool m_Hidden;
    bool m_ReleasedCapture;
    bool m_ShowedCursor;
    bool m_Dirty;
    Uint32 m_LastPublishTicks;
    QByteArray m_FontData;
    TTF_Font* m_Font;
    int m_FontSize;
};
