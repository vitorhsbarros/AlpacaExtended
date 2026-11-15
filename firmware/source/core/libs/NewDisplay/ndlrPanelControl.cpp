#ifdef TRACKER_MINI

#include "ndlrPanelControl.h"

#include "FT812.h"
#include "display.h"

#include <math.h>
#include <string.h>

// Font slots (see display.cpp): 0=14 Regular, 1=17 Regular, 2=14 Bold, 3=17 Bold, 4=12 Regular
static constexpr int kFont17B = 3;

// The panel addresses packed glyphs starting at cell 0. Use a separate bitmap
// handle because EVE's context stack does not restore a handle's source/layout/size.
static constexpr uint8_t kNdlrGlyphBitmapHandle = 14;

// Palette (matches the web prototype)
static constexpr uint32_t kColBody = DISP_RGB(0x15, 0x19, 0x1f);
static constexpr uint32_t kColBodyEdge = DISP_RGB(0x2f, 0x37, 0x47);
static constexpr uint32_t kColRingEdge = DISP_RGB(0x3b, 0x44, 0x56);
static constexpr uint32_t kColKnob = DISP_RGB(0x11, 0x14, 0x19);
static constexpr uint32_t kColKnobEdge = DISP_RGB(0x3b, 0x44, 0x58);
static constexpr uint32_t kColNotch = DISP_RGB(0xae, 0xb7, 0xc9);
static constexpr uint32_t kColText = DISP_RGB(0xd5, 0xdc, 0xea);
static constexpr uint32_t kColTextDim = DISP_RGB(0x8d, 0x98, 0xad);
static constexpr uint32_t kColTextFaint = DISP_RGB(0x56, 0x60, 0x74);
static constexpr uint32_t kColHi = DISP_RGB(0xff, 0xd2, 0x3f);
static constexpr uint32_t kColOn = DISP_RGB(0x3d, 0xdc, 0x84);
static constexpr uint32_t kColBlue = DISP_RGB(0x2f, 0x6b, 0xff);
static constexpr uint32_t kColBlueLight = DISP_RGB(0x6f, 0xa0, 0xff);
static constexpr uint32_t kColRed = DISP_RGB(0xff, 0x4d, 0x4d);
static constexpr uint32_t kColCream = DISP_RGB(0xe9, 0xe2, 0xcf);
static constexpr uint32_t kColBlack = DISP_RGB(0, 0, 0);
static constexpr uint32_t kColInfoBg = DISP_RGB(0x15, 0x1a, 0x22);

// NDLR panel reference (photo) coordinates -> Mini screen coordinates
static inline int16_t sx(int v)
{
    return (int16_t)(6 + (v * 3) / 4);
}
static inline int16_t sy(int v)
{
    return (int16_t)(6 + (v * 3) / 4);
}
static inline int16_t sr(int v)
{
    return (int16_t)((v * 3) / 4);
}

static constexpr int kRingCx = 403, kRingCy = 255, kRingR = 130; // reference coordinates

static const char* const kChordNumeral[7] = { "I", "II", "III", "IV", "V", "VI", "VII" };
static const char* const kChordType[7] = { "triad", "7th", "sus2", "alt2", "alt1", "sus4", "6th" };
static const char* const kPartLabel[4] = { "PAD", "DRONE", "1", "2" };

// Short in-panel labels (the info column shows the full names)
static const char* const kEncLabel[8][5] = {
    { "POS", "STRUM", "" },      { "RANGE", "SPREAD", "" },    { "POS", "NOTES", "TRIG" },
    { "TEMPO", "KEY", "MODE", "QNT", "CLOCK" }, { "POS", "P-LEN", "" }, { "VAR", "PATT", "" },
    { "DIV", "R-LEN", "GATE" },  { "ACCNT", "RHY", "OFFS" },
};
static const char* const kEncMenuName[8] = { "PAD OUT", "DRN OUT", "M1 OUT", "M2 OUT",
                                             "CAPTURE", "CAP TRK", "PAD TRKS", "STRM/LEN" };
static const char* const kEncEditorName[8] = { "CHORD SEQ", "",       "",       "MIDI CTL",
                                               "PATT ED",   "RHY ED", "PERLIN", "" };

// What each dial turns in each editor, and the names of the editor cells
static const char* const kPatternDial[8] = { "LOAD", "VAR", "RHY", "STEP", "SAVE", "DIV", "LEN", "VALUE" };
static const char* const kRhythmDial[8] = { "LOAD", "VAR", "PATT", "BEAT", "SAVE", "DIV", "LEN", "VALUE" };
static const char* const kChordSeqDial[8] = { "LOAD", "ATTR", "STEP", "SLOT", "SAVE", "VALUE", "ARM", "SONG" };
static const char* const kPatternCell[8] = { "PATT LOAD", "VAR", "RHY", "STEP", "PATT SAVE", "DIV", "LEN", "VALUE" };
static const char* const kRhythmCell[8] = { "RHY LOAD", "VAR", "PATT", "BEAT", "RHY SAVE", "DIV", "LEN", "VALUE" };

static const char* const* editorDialLabels(uint8_t kind)
{
    return kind == ndlrSubRhythm ? kRhythmDial : kind == ndlrSubChordSeq ? kChordSeqDial : kPatternDial;
}

//--------------------------------------------------------------------
// drawing helpers (all coordinates in screen pixels)
static void disc(int x, int y, int r, uint32_t color)
{
    API_COLOR(color);
    API_POINT_SIZE(16 * r);
    API_BEGIN(FTPOINTS);
    API_VERTEX2F(x, y);
    API_END();
}

static void fillRect(int x0, int y0, int x1, int y1, uint32_t color)
{
    API_COLOR(color);
    API_LINE_WIDTH(16);
    API_BEGIN(RECTS);
    API_VERTEX2F(x0, y0);
    API_VERTEX2F(x1, y1);
    API_END();
}

static void frameRect(int x0, int y0, int x1, int y1, uint32_t color)
{
    API_COLOR(color);
    API_LINE_WIDTH(16);
    API_BEGIN(LINE_STRIP);
    API_VERTEX2F(x0, y0);
    API_VERTEX2F(x1, y0);
    API_VERTEX2F(x1, y1);
    API_VERTEX2F(x0, y1);
    API_VERTEX2F(x0, y0);
    API_END();
}

static void drawDialNotch(int x, int y, int radius, int angle)
{
    constexpr float radiansPerTenthDegree = 0.001745329252f;
    const float radians = angle * radiansPerTenthDegree;
    const float dx = sinf(radians);
    const float dy = -cosf(radians);
    constexpr int subpixels = 16;
    const int inner = 3 * subpixels;
    const int outer = (radius - 4) * subpixels;

    API_VERTEX_FORMAT(4);
    API_COLOR(kColNotch);
    API_LINE_WIDTH(36);
    API_BEGIN(LINES);
    API_VERTEX2F(x * subpixels + lroundf(dx * inner), y * subpixels + lroundf(dy * inner));
    API_VERTEX2F(x * subpixels + lroundf(dx * outer), y * subpixels + lroundf(dy * outer));
    API_END();
    API_VERTEX_FORMAT(0);
}

// Font loaded into the panel's bitmap handle in the stage being drawn, or -1. Each stage is a
// separate display list wrapped in SAVE/RESTORE_CONTEXT, so every stage starts with stageBegin().
static int s_font = -1;

static void stageBegin()
{
    API_SAVE_CONTEXT();
    API_VERTEX_FORMAT(0);
    s_font = -1;
}

// Emit bitmap vertices directly so each line's display-list size is predictable. The font is set
// up only when it changes, which keeps a string to three or four commands plus one per glyph.
static void drawText(int x, int y, int font, uint16_t opt, uint32_t color, const char* text,
                     uint16_t maxWidth = 0)
{
    const strFont& f = fonts[font];
    const int len = maxWidth ? getMaxTextLengthThatFitSpecificWidth(&f, text, maxWidth)
                             : static_cast<int>(strlen(text));
    if (len == 0)
    {
        return;
    }

    const int width = getTextWidth(&f, text, len);
    x -= opt & OPT_CENTERX ? width / 2 : opt & OPT_RIGHTX ? width : 0;
    y -= f.height / 2;

    if (s_font != font)
    {
        s_font = font;
        API_BITMAP_HANDLE(kNdlrGlyphBitmapHandle);
        API_BITMAP_SOURCE(f.source);
        API_BITMAP_LAYOUT(f.format, f.linestride, f.height);
        API_BITMAP_SIZE(NEAREST, BORDER, BORDER, f.width, f.height);
    }
    API_COLOR(color);
    API_BEGIN(BITMAPS);

    // The font's first stored bitmap can be later than ASCII 0.
    uint8_t firstGlyph = 0;
    while (firstGlyph < 127 && !f.data[firstGlyph])
        ++firstGlyph;

    // VERTEX2II stores only nine bits of X. Translate runs on the right half.
    const int origin = x >= 512 ? 512 : 0;
    if (origin)
        API_VERTEX_TRANSLATE_X(origin * 16);

    for (int i = 0; i < len && x < 800; i++)
    {
        const uint8_t glyph = text[i];
        const int advance = f.data[glyph];
        if (x >= 0 && advance && glyph != ' ')
        {
            const uint8_t cell = glyph - firstGlyph;
            if (x - origin <= 511)
                API_VERTEX2II(x - origin, y, kNdlrGlyphBitmapHandle, cell);
            else
            {
                API_CELL(cell);
                API_VERTEX2F(x, y);
            }
        }
        x += advance;
    }
    API_END();
    if (origin)
        API_VERTEX_TRANSLATE_X(0);
}

// Several rectangles or dots in one primitive: set the colour, then add shapes.
// Rectangle corners are inclusive pixel positions.
static void rectsBegin()
{
    API_LINE_WIDTH(16);
    API_BEGIN(RECTS);
}

static void rect(int x0, int y0, int x1, int y1)
{
    API_VERTEX2F(x0, y0);
    API_VERTEX2F(x1, y1);
}

static void dotsBegin(int radius)
{
    API_POINT_SIZE(16 * radius);
    API_BEGIN(FTPOINTS);
}

//--------------------------------------------------------------------
cNdlrPanelControl::cNdlrPanelControl(strControlProperties* properties)
  : cDisplayControl(controlIdNdlrPanel, 3)
{
    colorsCount = 0;
    colors = nullptr;
    posX = 0;
    posY = 0;
    width = 800;
    height = 480;
    style = 0;

    if (properties != nullptr)
    {
        posX = properties->x;
        posY = properties->y;
        width = properties->w;
        height = properties->h;
        setStyle(properties->style);
    }
}

cNdlrPanelControl::~cNdlrPanelControl()
{
}

void cNdlrPanelControl::setStyle(uint32_t style)
{
    this->style = style;
}

void cNdlrPanelControl::setText(const char* text)
{
    (void)text;
}

void cNdlrPanelControl::setValue(int value)
{
    this->value = value;
}

void cNdlrPanelControl::setColors(const uint32_t* colors)
{
    (void)colors;
}

void cNdlrPanelControl::setData(void* data)
{
    view = static_cast<const strNdlrPanelView*>(data);
}

//--------------------------------------------------------------------
// Stage 0: panel body, ring, encoder labels, outer (menu/editor) labels
void cNdlrPanelControl::update()
{
    if (view == nullptr)
    {
        return;
    }

    stageBegin();

    fillRect(6, 6, 606, 396, kColBody);
    frameRect(6, 6, 606, 396, kColBodyEdge);

    drawText(308, 41, kFont17B, OPT_CENTERX, kColTextDim, "NDLR PANEL");

    // chord ring
    const int rcx = sx(kRingCx), rcy = sy(kRingCy);
    disc(rcx, rcy, sr(kRingR) + 4, kColRingEdge);
    disc(rcx, rcy, sr(kRingR) - 3, kColBody);

    for (int i = 0; i < 8; i++)
    {
        const strNdlrGeom& g = kNdlrGeom[ndlrEnc1 + i];
        const int ex = sx(g.x), ey = sy(g.y), er = sr(g.r);
        const bool left = i < 4;

        // labels between the encoder and the chord ring
        const int n = kNdlrEncLabelCount[i];
        const int lx = left ? ex + er + 8 : ex - er - 8;
        const uint16_t opt = left ? 0 : OPT_RIGHTX;
        if (view->menuMode == 2)
        {
            // in an editor each dial has one job
            drawText(lx, ey, kNdlrFontSmall, opt, view->selected == i ? kColHi : kColText,
                     editorDialLabels(view->sub.kind)[i]);
        }
        else
        {
            for (int j = 0; j < n; j++)
            {
                const bool active = view->menuMode == 0 && view->activeEnc == i && view->encLabel[i] == j;
                drawText(lx, ey - (n - 1) * 6 + j * 12, kNdlrFontSmall, opt, active ? kColHi : kColTextDim,
                     kEncLabel[i][j]);
            }
        }

        // outer labels: only the set that matches the current mode
        if (view->menuMode != 0)
        {
            const char* outer = view->menuMode == 1 ? kEncMenuName[i] : kEncEditorName[i];
            const int ox = left ? ex - er - 10 : ex + er + 10;
            const bool open = view->menuMode == 2 && view->editor == i;
            drawText(ox, ey, kNdlrFontSmall, left ? OPT_RIGHTX : 0,
                     open ? kColOn : outer[0] ? kColHi : kColTextFaint, outer[0] ? outer : "-");
        }
    }

    API_RESTORE_CONTEXT();
}

//--------------------------------------------------------------------
// Stage 1: selection highlight, all controls, the little NDLR screen
void cNdlrPanelControl::update2()
{
    if (view == nullptr)
    {
        return;
    }

    stageBegin();

    const int rcx = sx(kRingCx), rcy = sy(kRingCy);
    const bool shiftOn = view->shift != 0;

    // 1) selection highlight (drawn first so the controls sit on top of it)
    {
        const strNdlrGeom& g = kNdlrGeom[view->selected];
        const int x = sx(g.x), y = sy(g.y);
        if (g.kind == ndlrKindScreen)
        {
            fillRect(x - 54, y - 47, x + 54, y + 47, kColHi);
        }
        else if (g.kind != ndlrKindRecord)
        {
            const int r = sr(g.r) + (g.kind == ndlrKindEnc ? 8 : 6);
            disc(x, y, r, kColHi);
            disc(x, y, r - 3, kColBody);
        }
    }

    // 2) controls
    for (int c = 0; c < ndlrControlCount; c++)
    {
        const strNdlrGeom& g = kNdlrGeom[c];
        const int x = sx(g.x), y = sy(g.y), r = sr(g.r);

        switch (g.kind)
        {
            case ndlrKindEnc:
            {
                disc(x, y, r + 1, kColKnobEdge);
                disc(x, y, r - 1, kColKnob);
                drawDialNotch(x, y, r, view->menuMode == 2 ? 0 : view->encAngle[c - ndlrEnc1]);
                break;
            }
            case ndlrKindChord:
            {
                const int deg = c - ndlrChord1;
                const bool active = view->chordDegree == deg;
                const bool queued = view->queuedChord == deg + 1;
                const uint32_t color = queued ? kColBlueLight : active ? kColOn : kColTextDim;
                disc(x, y, r, color);
                disc(x, y, r - 2, kColKnob);
                drawText(x, y, kNdlrFontSmall, OPT_CENTERX, active ? kColOn : kColText, kChordNumeral[deg]);

                // chord type label on the inside of the ring, shown while Shift is active
                if (shiftOn)
                {
                    const float vx = (float)(rcx - x), vy = (float)(rcy - y);
                    const float len = sqrtf(vx * vx + vy * vy);
                    const int inset = deg == 0 ? r + 13 : r + 10;
                    const int tx = x + (int)(vx / len * inset);
                    const int ty = y + (int)(vy / len * inset);
                    drawText(tx, ty, kNdlrFontSmall, OPT_CENTERX, kColHi, kChordType[deg]);
                }
                break;
            }
            case ndlrKindShift:
            {
                if (shiftOn)
                {
                    disc(x, y, r + 4, kColBlueLight);
                    disc(x, y, r + 2, kColBody);
                }
                disc(x, y, r, kColBlue);
                drawText(x, y + r + 12, kNdlrFontSmall, OPT_CENTERX, kColTextDim, "SHIFT");
                break;
            }
            case ndlrKindPart:
            {
                const int p = c - ndlrBtnPad;
                if (view->modRing[p])
                {
                    disc(x, y, r + 3, kColRed);
                    disc(x, y, r + 1, kColBody);
                }
                const bool queued = view->pendingPartToggles & (1u << p);
                disc(x, y, r, queued ? kColBlueLight : view->partOn[p] ? kColOn : kColCream);
                drawText(x, y - r - 8, kNdlrFontSmall, OPT_CENTERX, kColTextDim, kPartLabel[p]);
                break;
            }
            case ndlrKindBtn:
            {
                bool lit = false;
                if (c == ndlrBtnMenu)
                {
                    lit = view->menuMode != 0;
                }
                else if (c == ndlrBtnAll)
                {
                    lit = view->partOn[0] && view->partOn[1] && view->partOn[2] && view->partOn[3];
                }
                disc(x, y, r, lit ? kColHi : kColCream);
                if (c == ndlrBtnAll)
                {
                    drawText(x - r - 4, y, kNdlrFontSmall, OPT_RIGHTX, kColTextDim, "ALL");
                    drawText(x + r + 4, y, kNdlrFontSmall, 0, shiftOn ? kColHi : kColTextFaint, "MOD");
                }
                else if (c == ndlrBtnMotifSel)
                {
                    drawText(x - r - 4, y, kNdlrFontSmall, OPT_RIGHTX, shiftOn ? kColHi : kColTextFaint, "PANIC");
                    drawText(x + r + 4, y, kNdlrFontSmall, 0, kColHi, view->motif == 2 ? "1/[2]" : "[1]/2");
                }
                else
                {
                    drawText(x + r + 5, y, kNdlrFontSmall, 0, kColTextDim, "MENU");
                    drawText(x + r + 5, y + 12, kNdlrFontSmall, 0, shiftOn ? kColHi : kColTextFaint, "+EDIT");
                }
                break;
            }
            case ndlrKindRecord:
            {
                if (view->selected == ndlrBtnRecord)
                    frameRect(x - 23, y - 9, x + 19, y + 9, kColHi);
                disc(x - 16, y, 4, view->recording ? kColRed : kColTextFaint);
                drawText(x + 3, y, kNdlrFontSmall, OPT_CENTERX,
                         view->recording ? kColRed : kColText, "REC");
                break;
            }
            case ndlrKindScreen:
            {
                // the NDLR's own display, drawn small
                const int x0 = x - 51, y0 = y - 44, x1 = x + 51, y1 = y + 44;
                fillRect(x0, y0, x1, y1, kColBlack);
                frameRect(x0, y0, x1, y1, kColBodyEdge);

                drawText(x, y0 + 9, kNdlrFontSmall, OPT_CENTERX, kColText, view->keyMode, 96);

                // encoder-select markers: left column = encoders 1-4, right column = 5-8
                for (int i = 0; i < 8; i++)
                {
                    const int mx = i < 4 ? x0 + 4 : x1 - 4;
                    const int my = y0 + 24 + (i % 4) * 9;
                    disc(mx, my, 3, kColText);
                    if (view->activeEnc != i)
                    {
                        disc(mx, my, 2, kColBlack);
                    }
                }
                frameRect(x0 + 11, y0 + 18, x1 - 11, y0 + 57, kColText);
                drawText(x, y0 + 29, kNdlrFontSmall, OPT_CENTERX, kColTextDim, view->detailText, 82);
                drawText(x, y0 + 46, kNdlrFontSmall, OPT_CENTERX, kColText, view->bpmText);

                drawText(x0 + 4, y0 + 69, kNdlrFontNormal, 0, kColText, view->chordText, 58);
                drawText(x1 - 4, y0 + 69, kNdlrFontSmall, OPT_RIGHTX,
                         view->typePending ? kColBlueLight : kColText, view->typeText, 40);

                for (int p = 0; p < 4; p++)
                {
                    const int dx = x - 30 + p * 20;
                    const bool motifSel = (p == 2 && view->motif == 1) || (p == 3 && view->motif == 2);
                    disc(dx, y1 - 4, 2, motifSel ? kColHi : kColText);
                    if (!view->partOn[p])
                    {
                        disc(dx, y1 - 4, 1, kColBlack);
                    }
                }
                break;
            }
            default: break;
        }
    }

    API_RESTORE_CONTEXT();
}

//--------------------------------------------------------------------
// The sub screen (stage 2). Everything is laid out from its top-left corner.
static constexpr int kSubX0 = 618, kSubX1 = 790, kSubY0 = 10;
static constexpr int kSubMid = (kSubX0 + kSubX1) / 2;

static uint32_t subColor(const strNdlrSubView& sub, int slot, uint32_t base = kColText)
{
    return sub.hiMask & (1u << slot) ? kColHi : sub.onMask & (1u << slot) ? kColOn : base;
}

static void subText(const strNdlrSubView& sub, int slot, int x, int y, uint16_t opt, int font = kNdlrFontSmall,
                    uint32_t base = kColText)
{
    drawText(x, y, font, opt, subColor(sub, slot, base), sub.text[slot]);
}

static uint32_t barColor(uint8_t state, uint32_t normal)
{
    switch (state)
    {
        case ndlrBarDim: return kColTextFaint;
        case ndlrBarPlaying: return kColOn;
        case ndlrBarSelected: return kColHi;
        case ndlrBarRest: return kColRed;
        case ndlrBarTie: return kColOn;
        default: return normal;
    }
}

// Sixteen pattern bars standing on `base`. Colour commands are sent only when the colour changes.
static void patternBars(const strNdlrSubView& sub, int x, int base, int barWidth, int pitch)
{
    uint32_t last = 0xffffffff;
    rectsBegin();
    for (int i = 0; i < sub.countA; i++)
    {
        const uint32_t color = barColor(sub.barAState[i], kColBlueLight);
        if (color != last)
        {
            API_COLOR(color);
            last = color;
        }
        rect(x + i * pitch, base - sub.barA[i], x + i * pitch + barWidth - 1, base);
    }
    API_END();
}

static void drawPadSub(const strNdlrSubView& sub)
{
    const int x0 = kSubX0, x1 = kSubX1, y0 = kSubY0;
    subText(sub, 0, x0 + 6, y0 + 10, 0);
    subText(sub, 1, x1 - 6, y0 + 10, OPT_RIGHTX, kNdlrFontSmall, kColTextDim);

    // one bar per note, placed by pitch
    uint32_t last = 0xffffffff;
    rectsBegin();
    for (int i = 0; i < sub.countA; i++)
    {
        const uint32_t color = barColor(sub.barAState[i], kColText);
        if (color != last)
        {
            API_COLOR(color);
            last = color;
        }
        rect(x0 + 8 + sub.barA[i], y0 + 20, x0 + 10 + sub.barA[i], y0 + 34);
    }
    API_END();

    subText(sub, 2, x0 + 8, y0 + 44, 0);
    subText(sub, 3, kSubMid, y0 + 44, OPT_CENTERX);
    subText(sub, 4, x1 - 8, y0 + 44, OPT_RIGHTX);
    subText(sub, 5, kSubMid, y0 + 60, OPT_CENTERX);
    subText(sub, 6, x0 + 8, y0 + 78, 0);
    subText(sub, 7, x0 + 8, y0 + 96, 0);
}

static void drawDroneSub(const strNdlrSubView& sub)
{
    const int x0 = kSubX0, x1 = kSubX1, y0 = kSubY0;
    subText(sub, 0, x0 + 6, y0 + 10, 0);
    subText(sub, 1, x1 - 6, y0 + 10, OPT_RIGHTX, kNdlrFontSmall, kColTextDim);

    // four octave boxes, then root / fifth / octave, then the beats it retriggers on
    rectsBegin();
    API_COLOR(kColBodyEdge);
    for (int i = 0; i < 4; i++)
    {
        if (sub.a != i + 1)
            rect(x0 + 8 + i * 40, y0 + 20, x0 + 41 + i * 40, y0 + 33);
    }
    for (int i = 0; i < 3; i++)
    {
        if (!(sub.b & (1u << i)))
            rect(x0 + 8 + i * 53, y0 + 38, x0 + 55 + i * 53, y0 + 51);
    }
    for (int i = 0; i < 4; i++)
    {
        if (!(sub.c & (1u << i)))
            rect(x0 + 60 + i * 16, y0 + 60, x0 + 71 + i * 16, y0 + 70);
    }
    API_COLOR(subColor(sub, 12));
    if (sub.a >= 1 && sub.a <= 4)
        rect(x0 + 8 + (sub.a - 1) * 40, y0 + 20, x0 + 41 + (sub.a - 1) * 40, y0 + 33);
    API_COLOR(subColor(sub, 13));
    for (int i = 0; i < 3; i++)
    {
        if (sub.b & (1u << i))
            rect(x0 + 8 + i * 53, y0 + 38, x0 + 55 + i * 53, y0 + 51);
    }
    API_COLOR(subColor(sub, 14));
    for (int i = 0; i < 4; i++)
    {
        if (sub.c & (1u << i))
            rect(x0 + 60 + i * 16, y0 + 60, x0 + 71 + i * 16, y0 + 70);
    }
    API_END();

    // names on the boxes: dark on a lit box, dim on an empty one
    static const char* const kOctave[4] = { "1", "2", "3", "4" };
    static const char* const kNote[3] = { "ROOT", "5TH", "OCT" };
    for (int i = 0; i < 4; i++)
        drawText(x0 + 25 + i * 40, y0 + 27, kNdlrFontSmall, OPT_CENTERX, sub.a == i + 1 ? kColBlack : kColTextDim, kOctave[i]);
    for (int i = 0; i < 3; i++)
        drawText(x0 + 32 + i * 53, y0 + 45, kNdlrFontSmall, OPT_CENTERX, sub.b & (1u << i) ? kColBlack : kColTextDim, kNote[i]);

    drawText(x0 + 8, y0 + 65, kNdlrFontSmall, 0, subColor(sub, 14), sub.text[2]);
    subText(sub, 3, x0 + 8, y0 + 90, 0);
}

static void drawTkmSub(const strNdlrSubView& sub)
{
    const int x0 = kSubX0, x1 = kSubX1, y0 = kSubY0;
    subText(sub, 0, x0 + 6, y0 + 10, 0);
    subText(sub, 1, x1 - 6, y0 + 10, OPT_RIGHTX);
    subText(sub, 4, kSubMid, y0 + 86, OPT_CENTERX);
    subText(sub, 5, x0 + 8, y0 + 102, 0, kNdlrFontSmall, kColTextDim);
    subText(sub, 6, x1 - 8, y0 + 102, OPT_RIGHTX, kNdlrFontSmall, kColTextDim);
    subText(sub, 2, x0 + 8, y0 + 27, 0, kFont17B);
    subText(sub, 3, x1 - 8, y0 + 27, OPT_RIGHTX, kFont17B);

    // one octave of keyboard with a dot on every note of the key and mode
    static constexpr uint8_t kWhite[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr uint8_t kBlack[5] = { 1, 3, 6, 8, 10 };
    static constexpr uint8_t kBlackAfter[5] = { 0, 1, 3, 4, 5 };
    const int kx = x0 + 9, ky = y0 + 40, kw = 22;
    rectsBegin();
    API_COLOR(kColCream);
    for (int i = 0; i < 7; i++)
        rect(kx + i * kw, ky, kx + i * kw + kw - 2, ky + 34);
    API_COLOR(kColBlack);
    for (int i = 0; i < 5; i++)
        rect(kx + (kBlackAfter[i] + 1) * kw - 7, ky, kx + (kBlackAfter[i] + 1) * kw + 5, ky + 19);
    API_END();

    // Dots: every note of the key and mode. On the white keys they are dark, with the chord
    // filled green and the key root yellow. On the black keys they are light, green or yellow.
    static constexpr uint32_t kMarkBlack[4] = { 0, kColCream, kColOn, kColHi };
    dotsBegin(4);
    API_COLOR(kColBlack);
    for (int i = 0; i < 7; i++)
    {
        if (sub.marks[kWhite[i]])
            API_VERTEX2F(kx + i * kw + 10, ky + 27);
    }
    for (int i = 0; i < 5; i++)
    {
        if (!sub.marks[kBlack[i]])
            continue;
        API_COLOR(kMarkBlack[sub.marks[kBlack[i]]]);
        API_VERTEX2F(kx + (kBlackAfter[i] + 1) * kw - 1, ky + 12);
    }
    API_END();
    dotsBegin(2);
    for (int i = 0; i < 7; i++)
    {
        if (sub.marks[kWhite[i]] < 2)
            continue;
        API_COLOR(sub.marks[kWhite[i]] == 3 ? kColHi : kColOn);
        API_VERTEX2F(kx + i * kw + 10, ky + 27);
    }
    API_END();
}

static void drawMotifSub(const strNdlrSubView& sub)
{
    const int x0 = kSubX0, x1 = kSubX1, y0 = kSubY0;
    drawText(x0 + 6, y0 + 10, kNdlrFontSmall, 0, kColHi, sub.text[0]);
    subText(sub, 1, x1 - 6, y0 + 10, OPT_RIGHTX, kNdlrFontSmall, kColTextDim);

    // pattern number, its sixteen notes, then clock divide
    subText(sub, 2, x0 + 20, y0 + 32, OPT_RIGHTX);
    patternBars(sub, x0 + 25, y0 + 40, 4, 5);
    subText(sub, 3, x1 - 6, y0 + 32, OPT_RIGHTX);

    // rhythm number and its beats: height is velocity, red is a rest, green is a tie
    subText(sub, 5, x0 + 20, y0 + 56, OPT_RIGHTX);
    uint32_t last = 0xffffffff;
    rectsBegin();
    for (int i = 0; i < 32; i++)
    {
        const uint8_t state = sub.barBState[i];
        const bool rest = state == ndlrBarRest, tie = state == ndlrBarTie;
        const uint32_t color = i >= sub.countB ? kColTextFaint : barColor(state, kColText);
        if (color != last)
        {
            API_COLOR(color);
            last = color;
        }
        const int bx = x0 + 25 + i * 3;
        if (rest)
            rect(bx, y0 + 63, bx + 1, y0 + 64);
        else if (tie)
            rect(bx - 1, y0 + 56, bx + 1, y0 + 57);
        else
            rect(bx, y0 + 64 - (2 + sub.barB[i] * 14 / 127), bx + 1, y0 + 64);
    }
    API_END();
    subText(sub, 4, x1 - 6, y0 + 56, OPT_RIGHTX, kNdlrFontSmall, kColTextDim);

    subText(sub, 6, x0 + 8, y0 + 80, 0);
    subText(sub, 7, x1 - 6, y0 + 80, OPT_RIGHTX);
    subText(sub, 8, x0 + 8, y0 + 98, 0, kNdlrFontSmall, kColTextDim);
    subText(sub, 9, kSubMid, y0 + 98, OPT_CENTERX, kNdlrFontSmall, kColTextDim);
    subText(sub, 10, x1 - 6, y0 + 98, OPT_RIGHTX, kNdlrFontSmall, kColTextDim);
}

// Editor cells sit like the eight dials: four on the left, four on the right, round a graph.
static constexpr int16_t kCellTop[4] = { 18, 52, 166, 184 };
static constexpr int16_t kCellBottom[4] = { 50, 66, 180, 216 };

static void cellBounds(int dial, int& x0, int& y0, int& x1, int& y1)
{
    x0 = dial < 4 ? kSubX0 + 2 : kSubX0 + 87;
    x1 = dial < 4 ? kSubX0 + 85 : kSubX1 - 2;
    y0 = kSubY0 + kCellTop[dial % 4];
    y1 = kSubY0 + kCellBottom[dial % 4];
}

static void drawEditorCells(const strNdlrSubView& sub, const char* const* names)
{
    // labels first, then the values, so each font is loaded once
    for (int dial = 0; dial < 8; dial++)
    {
        int x0, y0, x1, y1;
        cellBounds(dial, x0, y0, x1, y1);
        const bool big = dial % 4 == 0 || dial % 4 == 3;
        drawText(x0 + 4, big ? y0 + 8 : (y0 + y1) / 2, kNdlrFontSmall, 0, kColTextDim, names[dial]);
        if (!big)
            drawText(x1 - 4, (y0 + y1) / 2, kNdlrFontSmall, OPT_RIGHTX, kColHi, sub.text[2 + dial]);
    }
    for (int dial = 0; dial < 8; dial++)
    {
        int x0, y0, x1, y1;
        cellBounds(dial, x0, y0, x1, y1);
        if (dial % 4 == 0 || dial % 4 == 3)
            drawText((x0 + x1) / 2, y0 + 23, kFont17B, OPT_CENTERX, kColHi, sub.text[2 + dial]);
    }
    if (sub.selCell < 8)
    {
        int x0, y0, x1, y1;
        cellBounds(sub.selCell, x0, y0, x1, y1);
        frameRect(x0, y0, x1, y1, kColHi);
    }
}

static void drawPatternEditor(const strNdlrSubView& sub)
{
    const int x0 = kSubX0, x1 = kSubX1, y0 = kSubY0;
    drawText(x0 + 5, y0 + 9, kNdlrFontSmall, 0, kColOn, sub.text[0]);
    drawText(x1 - 5, y0 + 9, kNdlrFontSmall, OPT_RIGHTX, kColText, sub.text[1]);
    subText(sub, 10, x0 + 7, y0 + 77, 0);
    drawText(x1 - 7, y0 + 77, kNdlrFontSmall, OPT_RIGHTX, kColText, sub.text[11]);
    drawEditorCells(sub, kPatternCell);
    patternBars(sub, x0 + 8, y0 + 162, 7, 10);
}

static void drawRhythmEditor(const strNdlrSubView& sub)
{
    const int x0 = kSubX0, x1 = kSubX1, y0 = kSubY0;
    drawText(x0 + 5, y0 + 9, kNdlrFontSmall, 0, kColOn, sub.text[0]);
    drawText(x1 - 5, y0 + 9, kNdlrFontSmall, OPT_RIGHTX, kColText, sub.text[1]);
    drawText(x1 - 7, y0 + 77, kNdlrFontSmall, OPT_RIGHTX, kColText, sub.text[11]);
    drawEditorCells(sub, kRhythmCell);

    // a circle of beats: each line points at the centre and its length is the velocity
    constexpr float kTwoPi = 6.283185307f;
    const int cx = kSubMid, cy = y0 + 118, radius = 40, count = sub.countB;
    if (count == 0)
        return;
    int16_t px[32], py[32];
    for (int i = 0; i < count; i++)
    {
        const float a = kTwoPi * i / count;
        px[i] = static_cast<int16_t>(cx + lroundf(radius * sinf(a)));
        py[i] = static_cast<int16_t>(cy - lroundf(radius * cosf(a)));
    }
    uint32_t last = 0xffffffff;
    API_LINE_WIDTH(12);
    API_BEGIN(LINES);
    for (int i = 0; i < count; i++)
    {
        const uint8_t state = sub.barBState[i];
        if (state == ndlrBarRest || (state == ndlrBarTie && i == 0))
            continue;
        const uint32_t color = barColor(state, kColBlueLight);
        if (color != last)
        {
            API_COLOR(color);
            last = color;
        }
        API_VERTEX2F(px[i], py[i]);
        if (state == ndlrBarTie)
        {
            API_VERTEX2F(px[i - 1], py[i - 1]); // a tie joins the beat before it
        }
        else
        {
            const int length = 6 + sub.barB[i] * 28 / 127;
            API_VERTEX2F(px[i] + (cx - px[i]) * length / radius, py[i] + (cy - py[i]) * length / radius);
        }
    }
    API_END();
    last = 0xffffffff;
    dotsBegin(3);
    for (int i = 0; i < count; i++)
    {
        const uint32_t color = barColor(sub.barBState[i], kColBlueLight);
        if (color != last)
        {
            API_COLOR(color);
            last = color;
        }
        API_VERTEX2F(px[i], py[i]);
    }
    API_END();
}

static void drawChordSeq(const strNdlrSubView& sub)
{
    const int x0 = kSubX0, x1 = kSubX1, y0 = kSubY0;
    drawText(x0 + 5, y0 + 9, kNdlrFontSmall, 0, kColOn, sub.text[0]);
    drawText(x0 + 6, y0 + 26, kNdlrFontSmall, 0, kColTextDim, "LOAD");
    drawText(x0 + 91, y0 + 26, kNdlrFontSmall, 0, kColTextDim, "SAVE");

    // the chosen step: duration, key and mode, chord and type
    subText(sub, 3, kSubMid, y0 + 60, OPT_CENTERX);
    subText(sub, 4, kSubMid - 4, y0 + 75, OPT_RIGHTX);
    subText(sub, 5, kSubMid + 3, y0 + 75, 0);
    subText(sub, 6, kSubMid - 4, y0 + 90, OPT_RIGHTX);
    subText(sub, 7, kSubMid + 3, y0 + 90, 0);

    // three sections of six steps
    static const char* const kSection[3] = { "A", "B", "C" };
    for (int sec = 0; sec < 3; sec++)
        drawText(x0 + 10, y0 + 113 + sec * 15, kNdlrFontSmall, OPT_CENTERX, kColText, kSection[sec]);
    subText(sub, 9, x1 - 6, y0 + 113, OPT_RIGHTX, kNdlrFontSmall, kColTextFaint);
    subText(sub, 10, x1 - 6, y0 + 143, OPT_RIGHTX, kNdlrFontSmall, kColTextFaint);

    // the song: eight slots, each a section and a number of repeats
    for (int i = 0; i < 8; i++)
    {
        const uint32_t color = sub.slotState[i] == 2 ? kColOn : sub.slotState[i] == 1 ? kColText : kColTextFaint;
        drawText(x0 + 24 + (i % 4) * 41, y0 + 180 + (i >> 2) * 20, kNdlrFontSmall, OPT_CENTERX, color, sub.slotText[i]);
    }
    drawText(x0 + 44, y0 + 34, kFont17B, OPT_CENTERX, kColHi, sub.text[2]);
    drawText(x0 + 129, y0 + 34, kFont17B, OPT_CENTERX, kColHi, sub.text[8]);

    static constexpr uint32_t kStepColor[4] = { kColBlue, kColText, kColHi, kColOn };
    rectsBegin();
    for (int state = 0; state < 4; state++)
    {
        bool colorSet = false;
        for (int i = 0; i < 18; i++)
        {
            if (sub.steps[i] != state)
                continue;
            if (!colorSet)
                API_COLOR(kStepColor[state]);
            colorSet = true;
            const int bx = x0 + 22 + (i % 6) * 17, by = y0 + 108 + (i / 6) * 15;
            rect(bx, by, bx + 10, by + 9);
        }
    }
    // underline the song field being edited: the section letter or the repeats
    {
        const int slot = sub.field >> 1;
        const int tx = x0 + 24 + (slot % 4) * 41, ty = y0 + 189 + (slot >> 2) * 20;
        API_COLOR(kColHi);
        if (sub.field & 1)
            rect(tx - 3, ty, tx + 10, ty + 1);
        else
            rect(tx - 10, ty, tx - 4, ty + 1);
    }
    API_END();

    // frame the block the highlighted dial works on
    if (sub.selCell < 8)
    {
        static constexpr int16_t kTop[4] = { 18, 50, 100, 160 };
        static constexpr int16_t kBottom[4] = { 48, 98, 158, 216 };
        const int row = sub.selCell % 4;
        const int fx0 = row == 0 && sub.selCell == 4 ? x0 + 87 : x0 + 2;
        const int fx1 = row == 0 && sub.selCell == 0 ? x0 + 85 : x1 - 2;
        frameRect(fx0, y0 + kTop[row], fx1, y0 + kBottom[row], kColHi);
    }
}

static void drawSubScreen(const strNdlrPanelView& view)
{
    const strNdlrSubView& sub = view.sub;
    const int height = view.menuMode == 2 ? kNdlrEditorHeight : kNdlrSubHeight;
    fillRect(kSubX0, kSubY0, kSubX1, kSubY0 + height, kColBlack);
    switch (sub.kind)
    {
        case ndlrSubPad: drawPadSub(sub); break;
        case ndlrSubDrone: drawDroneSub(sub); break;
        case ndlrSubTkm: drawTkmSub(sub); break;
        case ndlrSubMotif: drawMotifSub(sub); break;
        case ndlrSubPattern: drawPatternEditor(sub); break;
        case ndlrSubRhythm: drawRhythmEditor(sub); break;
        case ndlrSubChordSeq: drawChordSeq(sub); break;
        default: break;
    }
}

//--------------------------------------------------------------------
// Stage 2: sub screen, info column, description strip, toast
void cNdlrPanelControl::update3()
{
    if (view == nullptr)
    {
        return;
    }

    stageBegin();

    // right-hand column: the sub screen, then the help text under it
    fillRect(614, 6, 794, 396, kColInfoBg);
    frameRect(614, 6, 794, 396, kColBodyEdge);
    drawSubScreen(*view);

    int y = kSubY0 + (view->menuMode == 2 ? kNdlrEditorHeight : kNdlrSubHeight) + 14;
    for (int i = 0; i < view->infoCount && i < kNdlrInfoLines && y < 390; i++)
    {
        switch (view->infoKind[i])
        {
            case ndlrLineTitle:
                drawText(621, y, kFont17B, 0, kColHi, view->info[i], 166);
                y += 20;
                break;
            case ndlrLineHeader:
                y += 2;
                drawText(621, y, kNdlrFontSmall, 0, kColTextFaint, view->info[i], 166);
                y += 13;
                break;
            case ndlrLineActive:
                drawText(621, y, kNdlrFontSmall, 0, kColText, view->info[i]);
                y += 13;
                break;
            case ndlrLineNow:
                y += 3;
                drawText(621, y, kNdlrFontSmall, 0, kColOn, view->info[i], 166);
                y += 13;
                break;
            default:
                drawText(621, y, kNdlrFontSmall, 0, kColTextDim, view->info[i]);
                y += 13;
                break;
        }
    }

    if (view->toast[0] != 0)
    {
        fillRect(156, 14, 456, 36, kColBlack);
        frameRect(156, 14, 456, 36, kColHi);
        drawText(306, 25, kNdlrFontSmall, OPT_CENTERX, kColHi, view->toast);
    }

    // description strip
    fillRect(6, 402, 794, 474, kColInfoBg);
    frameRect(6, 402, 794, 474, kColBodyEdge);
    for (int i = 0; i < view->descCount && i < kNdlrDescLines; i++)
    {
        drawText(14, 417 + i * 19, kNdlrFontNormal, 0, kColText, view->desc[i]);
    }

    API_RESTORE_CONTEXT();
}

#endif // TRACKER_MINI
