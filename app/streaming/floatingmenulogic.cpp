#include "floatingmenulogic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

int floatingMenuSelfTest()
{
    s_Failures = 0;

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
