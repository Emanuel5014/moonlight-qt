#include "localcursor.h"

#include <QtGlobal>

#include <cmath>
#include <utility>

LocalCursorImage scaleLocalCursorImage(const LocalCursorImage& image, float scale, int maxSize)
{
    if (image.width <= 0 || image.height <= 0 || image.bgra.size() < image.width * image.height * 4) {
        return image;
    }

    int width = qBound(1, (int)std::lround(image.width * scale), maxSize);
    int height = qBound(1, (int)std::lround(image.height * scale), maxSize);
    if (width == image.width && height == image.height) {
        return image;
    }

    LocalCursorImage scaled;
    scaled.width = width;
    scaled.height = height;
    scaled.hotX = qBound(0, (int)(image.hotX * (float)width / image.width), width - 1);
    scaled.hotY = qBound(0, (int)(image.hotY * (float)height / image.height), height - 1);
    scaled.bgra.resize(width * height * 4);

    const uchar* src = (const uchar*)image.bgra.constData();
    uchar* dst = (uchar*)scaled.bgra.data();
    const float stepX = (float)image.width / width;
    const float stepY = (float)image.height / height;

    for (int y = 0; y < height; y++) {
        float fy = qBound(0.0f, (y + 0.5f) * stepY - 0.5f, (float)(image.height - 1));
        int y0 = (int)fy;
        int y1 = qMin(y0 + 1, image.height - 1);
        float wy = fy - y0;

        for (int x = 0; x < width; x++) {
            float fx = qBound(0.0f, (x + 0.5f) * stepX - 0.5f, (float)(image.width - 1));
            int x0 = (int)fx;
            int x1 = qMin(x0 + 1, image.width - 1);
            float wx = fx - x0;

            const struct {
                int x, y;
                float weight;
            } taps[] = {
                { x0, y0, (1 - wx) * (1 - wy) },
                { x1, y0, wx * (1 - wy) },
                { x0, y1, (1 - wx) * wy },
                { x1, y1, wx * wy },
            };

            // Interpolate premultiplied colors so transparent pixels don't darken the edges
            float b = 0, g = 0, r = 0, a = 0;
            for (const auto& tap : taps) {
                const uchar* p = src + (tap.y * image.width + tap.x) * 4;
                float alpha = p[3] / 255.0f * tap.weight;
                b += p[0] * alpha;
                g += p[1] * alpha;
                r += p[2] * alpha;
                a += alpha;
            }

            uchar* out = dst + (y * width + x) * 4;
            if (a > 0) {
                out[0] = (uchar)qBound(0, (int)std::lround(b / a), 255);
                out[1] = (uchar)qBound(0, (int)std::lround(g / a), 255);
                out[2] = (uchar)qBound(0, (int)std::lround(r / a), 255);
                out[3] = (uchar)qBound(0, (int)std::lround(a * 255), 255);
            }
            else {
                out[0] = out[1] = out[2] = out[3] = 0;
            }
        }
    }

    return scaled;
}

uint32_t touchLocalCursorLru(QList<uint32_t>& lru, uint32_t id, int capacity)
{
    lru.removeOne(id);
    lru.prepend(id);

    if (lru.size() > capacity) {
        return lru.takeLast();
    }

    return 0;
}

#ifndef QT_NO_DEBUG
void localCursorSelfTest()
{
    LocalCursorImage image;
    image.width = 2;
    image.height = 2;
    image.hotX = 1;
    image.hotY = 1;
    image.bgra = QByteArray(2 * 2 * 4, (char)0xFF);

    // No scaling keeps the image
    LocalCursorImage same = scaleLocalCursorImage(image, 1.0f);
    SDL_assert(same.width == 2 && same.height == 2 && same.bgra == image.bgra);

    // Upscaling scales the size and the hotspot
    LocalCursorImage up = scaleLocalCursorImage(image, 2.0f);
    SDL_assert(up.width == 4 && up.height == 4 && up.hotX == 2 && up.hotY == 2);
    SDL_assert(up.bgra.size() == 4 * 4 * 4 && up.bgra == QByteArray(4 * 4 * 4, (char)0xFF));

    // Downscaling an opaque red pixel next to a transparent black one gives half-transparent red, not dark red
    LocalCursorImage edge;
    edge.width = 2;
    edge.height = 1;
    edge.bgra = QByteArray("\x00\x00\xFF\xFF\x00\x00\x00\x00", 8);
    LocalCursorImage down = scaleLocalCursorImage(edge, 0.5f);
    SDL_assert(down.width == 1 && down.height == 1);
    SDL_assert((uchar)down.bgra[2] == 0xFF && (uchar)down.bgra[0] == 0);
    SDL_assert((uchar)down.bgra[3] >= 0x7F && (uchar)down.bgra[3] <= 0x80);

    // The size is clamped
    LocalCursorImage big;
    big.width = 256;
    big.height = 256;
    big.bgra = QByteArray(256 * 256 * 4, 0);
    LocalCursorImage clamped = scaleLocalCursorImage(big, 4.0f);
    SDL_assert(clamped.width == 512 && clamped.height == 512);

    // Most-recently-used order
    QList<uint32_t> lru;
    SDL_assert(touchLocalCursorLru(lru, 1, 3) == 0);
    SDL_assert(touchLocalCursorLru(lru, 2, 3) == 0);
    SDL_assert(touchLocalCursorLru(lru, 3, 3) == 0);
    SDL_assert(touchLocalCursorLru(lru, 4, 3) == 1);
    SDL_assert(touchLocalCursorLru(lru, 2, 3) == 0);
    SDL_assert(touchLocalCursorLru(lru, 5, 3) == 3);
    SDL_assert(lru == QList<uint32_t>({ 5, 2, 4 }));
}
#endif

LocalCursor::LocalCursor()
    : m_Enabled(false),
      m_Granted(false),
      m_Active(false),
      m_StateShapeId(0),
      m_StateVisible(true),
      m_UsingHostCursor(false),
      m_WarnedUnknownShape(false)
{
}

LocalCursor::~LocalCursor()
{
    freeShapes();
}

uint32_t LocalCursor::start(bool enabled)
{
    freeShapes();

    m_Enabled = enabled;
    m_Granted = false;
    m_Active = false;
    m_StateShapeId = 0;
    m_StateVisible = true;
    m_WarnedUnknownShape = false;

    return enabled ? STREAMFORK_CAP_LOCAL_CURSOR : 0;
}

void LocalCursor::handleStreamforkReady(uint32_t capabilities)
{
    if (!m_Enabled) {
        return;
    }

    m_Granted = (capabilities & STREAMFORK_CAP_LOCAL_CURSOR) != 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Local cursor: %s",
                m_Granted ? "supported by the host" : "not supported by the host");
}

void LocalCursor::handleUserEvent(const SDL_UserEvent& event)
{
    switch (event.code) {
    case SDL_CODE_CURSOR_SHAPE:
    {
        LocalCursorImage* image = (LocalCursorImage*)event.data1;
        uint32_t shapeId = (uint32_t)(uintptr_t)event.data2;

        if (m_Granted) {
            auto existing = m_Shapes.find(shapeId);
            if (existing != m_Shapes.end() && existing->cursor != nullptr) {
                if (SDL_GetCursor() == existing->cursor) {
                    SDL_SetCursor(SDL_GetDefaultCursor());
                }
                SDL_FreeCursor(existing->cursor);
            }

            Shape shape;
            shape.image = *image;
            m_Shapes.insert(shapeId, shape);

            uint32_t evicted = touchLocalCursorLru(m_Lru, shapeId, LI_CURSOR_MIN_CACHED_SHAPES);
            if (evicted != 0 && evicted != m_StateShapeId) {
                Shape old = m_Shapes.take(evicted);
                if (old.cursor != nullptr) {
                    if (SDL_GetCursor() == old.cursor) {
                        SDL_SetCursor(SDL_GetDefaultCursor());
                    }
                    SDL_FreeCursor(old.cursor);
                }
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Local cursor: received shape %u (%dx%d, hotspot %d,%d)",
                        shapeId, image->width, image->height, image->hotX, image->hotY);
        }

        delete image;
        break;
    }

    case SDL_CODE_CURSOR_STATE:
    {
        uint32_t shapeId = (uint32_t)(uintptr_t)event.data1;
        bool visible = event.data2 != nullptr;

        if (!m_Granted) {
            break;
        }

        m_StateShapeId = shapeId;
        m_StateVisible = visible;

        if (shapeId != 0) {
            if (m_Shapes.contains(shapeId)) {
                // Keep the same most-recently-used order as the host
                touchLocalCursorLru(m_Lru, shapeId, LI_CURSOR_MIN_CACHED_SHAPES);
            }
            else if (!m_WarnedUnknownShape) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Local cursor: unknown shape %u, using the default cursor",
                            shapeId);
                m_WarnedUnknownShape = true;
            }
        }
        break;
    }

    default:
        SDL_assert(false);
        break;
    }
}

void LocalCursor::setActive(bool active)
{
    active = active && m_Granted;
    if (active == m_Active) {
        return;
    }

    m_Active = active;

    // The host reports the cursor state again when we turn it back on
    m_StateShapeId = 0;
    m_StateVisible = true;
    if (!active) {
        restoreDefaultCursor();
    }

    int err = LiSetLocalCursorActive(active);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Local cursor: %s (%d)",
                active ? "on, the host stops drawing the cursor in the video" : "off, the host draws the cursor in the video",
                err);
}

void LocalCursor::applyHostCursor(float scale)
{
    auto it = m_Shapes.find(m_StateShapeId);
    if (it == m_Shapes.end()) {
        // We don't have this shape: better a default cursor than none
        restoreDefaultCursor();
        return;
    }

    Shape& shape = *it;
    if (shape.cursor == nullptr || qAbs(shape.cursorScale - scale) > 0.001f) {
        LocalCursorImage scaled = scaleLocalCursorImage(shape.image, scale);
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(scaled.bgra.data(),
                                                                  scaled.width,
                                                                  scaled.height,
                                                                  32,
                                                                  scaled.width * 4,
                                                                  SDL_PIXELFORMAT_BGRA32);
        SDL_Cursor* cursor = nullptr;
        if (surface != nullptr) {
            cursor = SDL_CreateColorCursor(surface, scaled.hotX, scaled.hotY);
            SDL_FreeSurface(surface);
        }

        if (cursor == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Local cursor: failed to create cursor: %s",
                        SDL_GetError());
            restoreDefaultCursor();
            return;
        }

        SDL_Cursor* oldCursor = shape.cursor;
        shape.cursor = cursor;
        shape.cursorScale = scale;
        SDL_SetCursor(cursor);
        if (oldCursor != nullptr) {
            SDL_FreeCursor(oldCursor);
        }
    }
    else if (SDL_GetCursor() != shape.cursor) {
        SDL_SetCursor(shape.cursor);
    }

    m_UsingHostCursor = true;
}

void LocalCursor::restoreDefaultCursor()
{
    if (m_UsingHostCursor) {
        SDL_SetCursor(SDL_GetDefaultCursor());
        m_UsingHostCursor = false;
    }
}

void LocalCursor::freeShapes()
{
    restoreDefaultCursor();

    for (const Shape& shape : std::as_const(m_Shapes)) {
        if (shape.cursor != nullptr) {
            SDL_FreeCursor(shape.cursor);
        }
    }

    m_Shapes.clear();
    m_Lru.clear();
}

void LocalCursor::clCursorShape(uint32_t shapeId, uint16_t width, uint16_t height,
                                uint16_t hotX, uint16_t hotY, const void* bgra)
{
    // The data is only valid during the callback, so copy it for the main thread
    LocalCursorImage* image = new LocalCursorImage();
    image->width = width;
    image->height = height;
    image->hotX = hotX;
    image->hotY = hotY;
    image->bgra = QByteArray((const char*)bgra, (int)width * height * 4);

    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_CURSOR_SHAPE;
    event.user.data1 = image;
    event.user.data2 = (void*)(uintptr_t)shapeId;
    if (SDL_PushEvent(&event) <= 0) {
        delete image;
    }
}

void LocalCursor::clCursorState(bool visible, uint32_t shapeId)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_CURSOR_STATE;
    event.user.data1 = (void*)(uintptr_t)shapeId;
    event.user.data2 = (void*)(uintptr_t)(visible ? 1 : 0);
    SDL_PushEvent(&event);
}
