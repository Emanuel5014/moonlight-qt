#include "floatingmenulogic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace FloatingMenuLogic {

// Logical sizes, multiplied by the window scale
static const int k_ButtonSize = 40;
static const int k_RowHeight = 28;
static const int k_PanelWidth = 300;
static const int k_Padding = 8;
static const int k_CornerRadius = 6;
static const int k_DragThreshold = 4;

// Keeps [pos, pos + size) inside [0, limit), or 0 when it doesn't fit
static int clampToRange(int pos, int size, int limit)
{
    return std::max(0, std::min(pos, limit - size));
}

template<class T>
static void insertSorted(std::vector<T>& values, const T& value, bool (*less)(const T&, const T&))
{
    for (const T& v : values) {
        if (v == value) {
            return;
        }
    }
    values.insert(std::upper_bound(values.begin(), values.end(), value, less), value);
}

bool Rect::contains(int px, int py) const
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

bool Rect::operator==(const Rect& other) const
{
    return x == other.x && y == other.y && w == other.w && h == other.h;
}

bool Resolution::operator==(const Resolution& other) const
{
    return w == other.w && h == other.h;
}

Metrics scaledMetrics(float scale)
{
    auto scaled = [scale](int logical) {
        return std::max(1, (int)std::lround(logical * scale));
    };

    return {
        scaled(k_ButtonSize),
        scaled(k_RowHeight),
        scaled(k_PanelWidth),
        scaled(k_Padding),
        scaled(k_CornerRadius),
        scaled(k_DragThreshold),
    };
}

Rect buttonRect(double fracX, double fracY, int winW, int winH, int size)
{
    int centerX = (int)std::lround(fracX * winW);
    int centerY = (int)std::lround(fracY * winH);

    return {
        clampToRange(centerX - size / 2, size, winW),
        clampToRange(centerY - size / 2, size, winH),
        size,
        size,
    };
}

void buttonFraction(const Rect& button, int winW, int winH, double& fracX, double& fracY)
{
    if (winW <= 0 || winH <= 0) {
        fracX = 0.5;
        fracY = 0.0;
        return;
    }

    fracX = std::clamp((button.x + button.w / 2.0) / winW, 0.0, 1.0);
    fracY = std::clamp((button.y + button.h / 2.0) / winH, 0.0, 1.0);
}

Rect panelRect(const Rect& button, int rowCount, int winW, int winH, const Metrics& m)
{
    Rect panel;
    panel.w = m.panelWidth;
    panel.h = rowCount * m.rowHeight + 2 * m.padding;
    panel.x = clampToRange(button.x, panel.w, winW);

    if (button.y + button.h + panel.h <= winH) {
        panel.y = button.y + button.h;
    }
    else if (button.y - panel.h >= 0) {
        panel.y = button.y - panel.h;
    }
    else {
        panel.y = 0;
    }

    return panel;
}

int rowAt(const Rect& panel, int rowCount, const Metrics& m, int x, int y)
{
    if (!panel.contains(x, y) || m.rowHeight <= 0) {
        return -1;
    }

    int offset = y - panel.y - m.padding;
    if (offset < 0) {
        return -1;
    }

    int row = offset / m.rowHeight;
    return row < rowCount ? row : -1;
}

ChoiceHalf choiceHalfAt(const Rect& panel, int x)
{
    return x < panel.x + panel.w / 2 ? ChoiceHalf::Previous : ChoiceHalf::Next;
}

std::vector<Resolution> resolutionSteps(Resolution native, Resolution current)
{
    auto less = [](const Resolution& a, const Resolution& b) {
        long long areaA = (long long)a.w * a.h;
        long long areaB = (long long)b.w * b.h;
        return areaA != areaB ? areaA < areaB : a.w < b.w;
    };

    std::vector<Resolution> steps = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    for (const Resolution& extra : {native, current}) {
        if (extra.w > 0 && extra.h > 0) {
            insertSorted<Resolution>(steps, extra, less);
        }
    }
    return steps;
}

static bool intLess(const int& a, const int& b)
{
    return a < b;
}

std::vector<int> fpsSteps(int nativeHz, int current)
{
    std::vector<int> steps = {30, 60, 90, 120, 144};
    for (int extra : {nativeHz, current}) {
        if (extra > 0) {
            insertSorted<int>(steps, extra, intLess);
        }
    }
    return steps;
}

std::vector<int> bitrateStepsKbps(int currentKbps)
{
    std::vector<int> steps = {5000, 10000, 15000, 20000, 30000, 40000, 50000, 60000, 80000, 100000, 120000, 150000};
    if (currentKbps > 0) {
        insertSorted<int>(steps, currentKbps, intLess);
    }
    return steps;
}

// ---------------------------------------------------------------------------
// MenuController

MenuController::MenuController() :
    m_WinW(0), m_WinH(0),
    m_Metrics(scaledMetrics(1.0f)),
    m_FracX(0.5), m_FracY(0.0),
    m_ButtonVisible(true),
    m_Open(false),
    m_Hovered(-1),
    m_Press(Press::None),
    m_PressX(0), m_PressY(0),
    m_PressRow(-1),
    m_Dragging(false),
    m_DragStart{}, m_DragRect{},
    m_SwallowEscUp(false),
    m_OtherButtonsPressed(0),
    m_Redraw(true)
{
}

void MenuController::setWindow(int winW, int winH, const Metrics& m)
{
    m_WinW = winW;
    m_WinH = winH;
    m_Metrics = m;
    m_Redraw = true;
}

void MenuController::setPosition(double fracX, double fracY)
{
    m_FracX = std::clamp(fracX, 0.0, 1.0);
    m_FracY = std::clamp(fracY, 0.0, 1.0);
    m_Redraw = true;
}

void MenuController::getPosition(double& fracX, double& fracY) const
{
    fracX = m_FracX;
    fracY = m_FracY;
}

void MenuController::setRows(const std::vector<bool>& isChoiceRow)
{
    m_IsChoiceRow = isChoiceRow;
    if (m_Hovered >= (int)m_IsChoiceRow.size()) {
        m_Hovered = -1;
    }
    m_Redraw = true;
}

void MenuController::setButtonVisible(bool visible)
{
    if (m_ButtonVisible != visible) {
        m_ButtonVisible = visible;
        m_Redraw = true;
    }
}

bool MenuController::isOpen() const
{
    return m_Open;
}

void MenuController::open()
{
    if (!m_Open) {
        m_Open = true;
        m_Hovered = -1;
        m_Commands.push_back({CommandKind::Opened, -1, 0});
        m_Redraw = true;
    }
}

void MenuController::close()
{
    if (m_Open) {
        m_Open = false;
        m_Hovered = -1;
        m_Commands.push_back({CommandKind::Closed, -1, 0});
        m_Redraw = true;
    }
}

bool MenuController::mouseDown(int x, int y)
{
    m_PressX = x;
    m_PressY = y;

    if (m_ButtonVisible && button().contains(x, y)) {
        m_Press = Press::OnButton;
        m_DragStart = button();
        return true;
    }
    else if (m_Open && panel().contains(x, y)) {
        m_Press = Press::OnPanel;
        m_PressRow = rowAt(panel(), (int)m_IsChoiceRow.size(), m_Metrics, x, y);
        return true;
    }
    else if (m_Open) {
        // A click outside the panel closes it and doesn't reach the host
        m_Press = Press::OutsidePanel;
        close();
        return true;
    }

    m_Press = Press::None;
    return false;
}

bool MenuController::mouseUp(int x, int y)
{
    Press press = m_Press;
    m_Press = Press::None;

    switch (press) {
    case Press::OnButton:
        if (m_Dragging) {
            m_Dragging = false;
            m_Commands.push_back({CommandKind::MoveFinished, -1, 0});
            m_Redraw = true;
        }
        else if (m_Open) {
            close();
        }
        else {
            open();
        }
        return true;

    case Press::OnPanel:
    {
        int row = rowAt(panel(), (int)m_IsChoiceRow.size(), m_Metrics, x, y);
        if (row >= 0 && row == m_PressRow) {
            if (m_IsChoiceRow[row]) {
                int delta = choiceHalfAt(panel(), x) == ChoiceHalf::Previous ? -1 : +1;
                m_Commands.push_back({CommandKind::StepRow, row, delta});
            }
            else {
                m_Commands.push_back({CommandKind::ActivateRow, row, 0});
            }
        }
        return true;
    }

    case Press::OutsidePanel:
        return true;

    case Press::None:
    default:
        return false;
    }
}

static unsigned int buttonBit(int button)
{
    return (button >= 1 && button <= 32) ? 1u << (button - 1) : 0;
}

bool MenuController::otherButtonDown(int button, int x, int y)
{
    bool consumed;
    if (overMenu(x, y)) {
        consumed = true;
    }
    else if (m_Open) {
        // Like the left button, a press outside the panel closes it
        close();
        consumed = true;
    }
    else {
        consumed = false;
    }

    if (consumed) {
        m_OtherButtonsPressed |= buttonBit(button);
    }
    return consumed;
}

bool MenuController::otherButtonUp(int button, int, int)
{
    unsigned int bit = buttonBit(button);
    bool consumed = (m_OtherButtonsPressed & bit) != 0;
    m_OtherButtonsPressed &= ~bit;
    return consumed;
}

bool MenuController::hasPress() const
{
    return m_Press != Press::None || m_OtherButtonsPressed != 0;
}

bool MenuController::mouseMotion(int x, int y)
{
    if (m_Press == Press::OnButton) {
        if (!m_Dragging &&
                (std::abs(x - m_PressX) > m_Metrics.dragThreshold ||
                 std::abs(y - m_PressY) > m_Metrics.dragThreshold)) {
            m_Dragging = true;
            close();
        }

        if (m_Dragging) {
            m_DragRect = m_DragStart;
            m_DragRect.x = clampToRange(m_DragStart.x + x - m_PressX, m_DragStart.w, m_WinW);
            m_DragRect.y = clampToRange(m_DragStart.y + y - m_PressY, m_DragStart.h, m_WinH);
            buttonFraction(m_DragRect, m_WinW, m_WinH, m_FracX, m_FracY);
            m_Redraw = true;
        }
        return true;
    }

    if (m_Open) {
        setHovered(rowAt(panel(), (int)m_IsChoiceRow.size(), m_Metrics, x, y));
        return overMenu(x, y);
    }

    return false;
}

bool MenuController::mouseWheel(int x, int y, int dy)
{
    if (m_Open && panel().contains(x, y)) {
        int row = rowAt(panel(), (int)m_IsChoiceRow.size(), m_Metrics, x, y);
        if (row >= 0 && m_IsChoiceRow[row] && dy != 0) {
            m_Commands.push_back({CommandKind::StepRow, row, dy > 0 ? +1 : -1});
        }
        return true;
    }

    return m_ButtonVisible && button().contains(x, y);
}

bool MenuController::escDown()
{
    if (m_Open) {
        close();
        m_SwallowEscUp = true;
        return true;
    }

    // Key repeats of the press that closed the panel
    return m_SwallowEscUp;
}

bool MenuController::escUp()
{
    if (m_SwallowEscUp) {
        m_SwallowEscUp = false;
        return true;
    }
    return false;
}

Rect MenuController::button() const
{
    if (m_Dragging) {
        return m_DragRect;
    }
    return buttonRect(m_FracX, m_FracY, m_WinW, m_WinH, m_Metrics.buttonSize);
}

Rect MenuController::panel() const
{
    return panelRect(button(), (int)m_IsChoiceRow.size(), m_WinW, m_WinH, m_Metrics);
}

int MenuController::hoveredRow() const
{
    return m_Hovered;
}

bool MenuController::isDragging() const
{
    return m_Dragging;
}

std::vector<Command> MenuController::takeCommands()
{
    std::vector<Command> commands;
    commands.swap(m_Commands);
    return commands;
}

bool MenuController::takeRedraw()
{
    bool redraw = m_Redraw;
    m_Redraw = false;
    return redraw;
}

bool MenuController::overMenu(int x, int y) const
{
    return (m_ButtonVisible && button().contains(x, y)) ||
           (m_Open && panel().contains(x, y));
}

void MenuController::setHovered(int row)
{
    if (m_Hovered != row) {
        m_Hovered = row;
        m_Redraw = true;
    }
}

// ---------------------------------------------------------------------------
// Self-test

static int s_Failures;

#define FM_CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FM_CHECK failed at line %d: %s\n", __LINE__, #cond); \
            s_Failures++; \
        } \
    } while (0)

static void buttonRectCentersAndClamps()
{
    FM_CHECK((buttonRect(0.5, 0.0, 1920, 1080, 40) == Rect{940, 0, 40, 40}));
    FM_CHECK((buttonRect(1.0, 1.0, 1920, 1080, 40) == Rect{1880, 1040, 40, 40}));
}

static void fractionRoundTrip()
{
    double fx, fy;
    buttonFraction({940, 0, 40, 40}, 1920, 1080, fx, fy);
    FM_CHECK(fx == 0.5);
    FM_CHECK(fy == 20.0 / 1080);
}

static void zeroSizeWindowIsSafe()
{
    double fx, fy;
    buttonFraction({0, 0, 40, 40}, 0, 0, fx, fy);
    FM_CHECK(fx == 0.5 && fy == 0.0);

    Rect z = buttonRect(0.5, 0.5, 0, 0, 40);
    FM_CHECK(z.x == 0 && z.y == 0);
}

static void panelFlipsAboveAndClampsInTinyWindow()
{
    Metrics m = scaledMetrics(1.0f);

    Rect below = panelRect({940, 0, 40, 40}, 10, 1920, 1080, m);
    FM_CHECK((below == Rect{940, 40, 300, 296}));

    Rect above = panelRect({940, 1040, 40, 40}, 10, 1920, 1080, m);
    FM_CHECK(above.y == 1040 - 296);

    Rect tiny = panelRect({0, 100, 40, 40}, 10, 200, 250, m);
    FM_CHECK(tiny.x == 0 && tiny.y == 0);

    Rect right = panelRect({1880, 0, 40, 40}, 10, 1920, 1080, m);
    FM_CHECK(right.x == 1620);
}

static void rowAtAndHalves()
{
    Metrics m = scaledMetrics(1.0f);
    Rect below = panelRect({940, 0, 40, 40}, 10, 1920, 1080, m);

    FM_CHECK(rowAt(below, 10, m, 950, 40 + 8 + 5) == 0);
    FM_CHECK(rowAt(below, 10, m, 950, 40 + 8 + 28 * 9 + 5) == 9);
    FM_CHECK(rowAt(below, 10, m, 950, 40 + 2) == -1);
    FM_CHECK(rowAt(below, 10, m, 939, 60) == -1);

    FM_CHECK(choiceHalfAt(below, 941) == ChoiceHalf::Previous);
    FM_CHECK(choiceHalfAt(below, 1239) == ChoiceHalf::Next);
}

static void scaledMetricsRetina()
{
    FM_CHECK(scaledMetrics(2.0f).buttonSize == 80);
    FM_CHECK(scaledMetrics(2.0f).rowHeight == 56);
}

static void customCurrentValuesAreInserted()
{
    std::vector<Resolution> expectedRes = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
    FM_CHECK(resolutionSteps({2560, 1440}, {3440, 1440}) == expectedRes);

    FM_CHECK((fpsSteps(60, 75) == std::vector<int>{30, 60, 75, 90, 120, 144}));
    FM_CHECK((fpsSteps(165, 60) == std::vector<int>{30, 60, 90, 120, 144, 165}));

    std::vector<int> bitrates = bitrateStepsKbps(37000);
    FM_CHECK(bitrates.size() == 13 && bitrates[4] == 30000 && bitrates[5] == 37000 && bitrates[6] == 40000);
}

static void stepValueClampsWithoutWrap()
{
    FM_CHECK(stepValue(fpsSteps(60, 60), 60, +1) == 90);
    FM_CHECK(stepValue(fpsSteps(60, 60), 144, +1) == 144);
    FM_CHECK(stepValue(fpsSteps(60, 60), 30, -1) == 30);
    FM_CHECK(stepValue(std::vector<int>{1, 2}, 7, +1) == 7);
}

// 1920x1080, scale 1, button at the top center ({940, 0, 40, 40}),
// 10 rows with row 5 being a choice row. The panel below the button
// is {940, 40, 300, 296}.
static MenuController makeController()
{
    MenuController c;
    c.setWindow(1920, 1080, scaledMetrics(1.0f));
    c.setPosition(0.5, 0.0);
    std::vector<bool> rows(10, false);
    rows[5] = true;
    c.setRows(rows);
    c.takeCommands();
    c.takeRedraw();
    return c;
}

static int rowY(int row)
{
    return 40 + 8 + 28 * row + 5;
}

static bool hasCommand(const std::vector<Command>& commands, CommandKind kind, int row = -1, int delta = 0)
{
    for (const Command& c : commands) {
        if (c.kind == kind &&
                ((kind != CommandKind::ActivateRow && kind != CommandKind::StepRow) || c.row == row) &&
                (kind != CommandKind::StepRow || c.delta == delta)) {
            return true;
        }
    }
    return false;
}

static void clickOutsideClosedNotConsumed()
{
    MenuController c = makeController();
    FM_CHECK(!c.mouseDown(100, 500));
    FM_CHECK(!c.mouseUp(100, 500));
    FM_CHECK(!c.mouseMotion(960, 20));
    FM_CHECK(c.button() == (Rect{940, 0, 40, 40}));
}

static void pressOutsideReleaseOnButtonNotConsumed()
{
    MenuController c = makeController();
    FM_CHECK(!c.mouseDown(100, 500));
    FM_CHECK(!c.mouseUp(960, 20));
    FM_CHECK(!c.isOpen());
}

static void clickOnButtonToggles()
{
    MenuController c = makeController();
    FM_CHECK(c.mouseDown(960, 20));
    FM_CHECK(c.mouseUp(961, 21));
    FM_CHECK(c.isOpen());
    FM_CHECK(hasCommand(c.takeCommands(), CommandKind::Opened));

    FM_CHECK(c.mouseDown(960, 20));
    FM_CHECK(c.mouseUp(960, 20));
    FM_CHECK(!c.isOpen());
    FM_CHECK(hasCommand(c.takeCommands(), CommandKind::Closed));
}

static void dragPastEdgeClampsAndConsumesUp()
{
    MenuController c = makeController();
    FM_CHECK(c.mouseDown(960, 20));
    FM_CHECK(c.mouseMotion(1000, 20));
    FM_CHECK(c.isDragging());
    FM_CHECK(c.mouseMotion(5000, -300));
    FM_CHECK(c.button() == (Rect{1880, 0, 40, 40}));
    FM_CHECK(c.mouseUp(5000, -300));
    FM_CHECK(!c.isDragging());
    FM_CHECK(hasCommand(c.takeCommands(), CommandKind::MoveFinished));

    double fx, fy;
    c.getPosition(fx, fy);
    FM_CHECK(fx == 1900.0 / 1920);
    FM_CHECK(fy == 20.0 / 1080);
}

static void dragClosesPanel()
{
    MenuController c = makeController();
    c.open();
    FM_CHECK(c.mouseDown(960, 20));
    FM_CHECK(c.mouseMotion(1000, 20));
    FM_CHECK(!c.isOpen());
}

static void clickRowActivates()
{
    MenuController c = makeController();
    c.open();
    c.takeCommands();
    FM_CHECK(c.mouseDown(950, rowY(2)));
    FM_CHECK(c.mouseUp(950, rowY(2)));
    std::vector<Command> commands = c.takeCommands();
    FM_CHECK(commands.size() == 1 && hasCommand(commands, CommandKind::ActivateRow, 2));
    FM_CHECK(c.isOpen());
}

static void choiceRowStepsByHalfAndWheel()
{
    MenuController c = makeController();
    c.open();
    c.takeCommands();

    FM_CHECK(c.mouseDown(945, rowY(5)) && c.mouseUp(945, rowY(5)));
    FM_CHECK(hasCommand(c.takeCommands(), CommandKind::StepRow, 5, -1));

    FM_CHECK(c.mouseDown(1235, rowY(5)) && c.mouseUp(1235, rowY(5)));
    FM_CHECK(hasCommand(c.takeCommands(), CommandKind::StepRow, 5, +1));

    FM_CHECK(c.mouseWheel(1000, rowY(5), -1));
    FM_CHECK(hasCommand(c.takeCommands(), CommandKind::StepRow, 5, -1));

    FM_CHECK(c.mouseWheel(1000, rowY(2), 1));
    FM_CHECK(c.takeCommands().empty());

    FM_CHECK(!c.mouseWheel(100, 900, 1));
}

static void clickOutsideOpenClosesAndConsumesBoth()
{
    MenuController c = makeController();
    c.open();
    FM_CHECK(c.mouseDown(100, 900));
    FM_CHECK(!c.isOpen());
    FM_CHECK(c.mouseUp(100, 900));
    FM_CHECK(!c.mouseMotion(100, 900));
}

static void otherButtonsTrackedPerPress()
{
    MenuController c = makeController();
    // Closed: a press outside is the host's, wherever it is released
    FM_CHECK(!c.otherButtonDown(3, 100, 900));
    FM_CHECK(!c.otherButtonUp(3, 960, 20));
    // A press on the button is ours, wherever it is released
    FM_CHECK(c.otherButtonDown(3, 960, 20));
    FM_CHECK(c.otherButtonUp(3, 100, 900));
    FM_CHECK(!c.isOpen());

    // Open: no action on rows, and a press outside closes like the left button
    c.open();
    c.takeCommands();
    FM_CHECK(c.otherButtonDown(2, 950, rowY(2)));
    FM_CHECK(c.takeCommands().empty());
    FM_CHECK(c.isOpen());
    FM_CHECK(c.otherButtonUp(2, 950, rowY(2)));
    FM_CHECK(c.otherButtonDown(3, 100, 900));
    FM_CHECK(!c.isOpen());
    FM_CHECK(c.otherButtonUp(3, 100, 900));
}

static void hasPressUntilRelease()
{
    MenuController c = makeController();
    c.mouseDown(100, 500);
    FM_CHECK(!c.hasPress());
    c.mouseUp(100, 500);

    c.open();
    FM_CHECK(c.mouseDown(100, 900));
    FM_CHECK(c.hasPress());
    FM_CHECK(c.mouseUp(100, 900));
    FM_CHECK(!c.hasPress());

    FM_CHECK(c.otherButtonDown(3, 960, 20));
    FM_CHECK(c.hasPress());
    FM_CHECK(c.otherButtonUp(3, 960, 20));
    FM_CHECK(!c.hasPress());
}

static void escUpConsumedOnlyAfterConsumedDown()
{
    MenuController c = makeController();
    FM_CHECK(!c.escDown());
    FM_CHECK(!c.escUp());

    c.open();
    FM_CHECK(c.escDown());
    FM_CHECK(!c.isOpen());
    FM_CHECK(c.escUp());
    FM_CHECK(!c.escUp());
}

static void escRepeatsAfterClosingAreConsumed()
{
    MenuController c = makeController();
    c.open();
    FM_CHECK(c.escDown());
    FM_CHECK(c.escDown()); // key repeat, panel already closed
    FM_CHECK(c.escUp());
    FM_CHECK(!c.escDown());
}

static void hiddenButtonIgnoresClicks()
{
    MenuController c = makeController();
    c.setButtonVisible(false);
    FM_CHECK(!c.mouseDown(960, 20));
    FM_CHECK(!c.mouseUp(960, 20));
    FM_CHECK(!c.isOpen());
}

static void hoverMarksRedraw()
{
    MenuController c = makeController();
    c.open();
    c.takeRedraw();
    FM_CHECK(c.mouseMotion(950, rowY(3)));
    FM_CHECK(c.hoveredRow() == 3);
    FM_CHECK(c.takeRedraw());
    FM_CHECK(c.mouseMotion(951, rowY(3)));
    FM_CHECK(!c.takeRedraw());
}

int floatingMenuSelfTest()
{
    s_Failures = 0;

    clickOutsideClosedNotConsumed();
    pressOutsideReleaseOnButtonNotConsumed();
    clickOnButtonToggles();
    dragPastEdgeClampsAndConsumesUp();
    dragClosesPanel();
    clickRowActivates();
    choiceRowStepsByHalfAndWheel();
    clickOutsideOpenClosesAndConsumesBoth();
    otherButtonsTrackedPerPress();
    hasPressUntilRelease();
    escUpConsumedOnlyAfterConsumedDown();
    escRepeatsAfterClosingAreConsumed();
    hiddenButtonIgnoresClicks();
    hoverMarksRedraw();

    buttonRectCentersAndClamps();
    fractionRoundTrip();
    zeroSizeWindowIsSafe();
    panelFlipsAboveAndClampsInTinyWindow();
    rowAtAndHalves();
    scaledMetricsRetina();
    customCurrentValuesAreInserted();
    stepValueClampsWithoutWrap();

    return s_Failures;
}

}
