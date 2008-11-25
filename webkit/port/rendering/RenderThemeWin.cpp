/*
 * This file is part of the WebKit project.
 *
 * Copyright (C) 2006 Apple Computer, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"
#include "RenderThemeWin.h"

#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>

#include "ChromiumBridge.h"
#include "CSSValueKeywords.h"
#include "Document.h"
#include "FontSelector.h"
#include "FontUtilsWin.h"
#include "GraphicsContext.h"
#include "ScrollbarTheme.h"
#include "SkiaUtils.h"
#include "ThemeHelperWin.h"

#include "base/gfx/native_theme.h"
#include "base/gfx/skia_utils.h"
#include "base/win_util.h"

namespace {

enum PaddingType {
    TopPadding,
    RightPadding,
    BottomPadding,
    LeftPadding
};

const int kStyledMenuListInternalPadding[4] = { 1, 4, 1, 4 };

// The default variable-width font size.  We use this as the default font
// size for the "system font", and as a base size (which we then shrink) for
// form control fonts.
float DefaultFontSize = 16.0;

WebCore::FontDescription smallSystemFont;
WebCore::FontDescription menuFont;
WebCore::FontDescription labelFont;

}  // namespace

namespace WebCore {

bool RenderThemeWin::m_findInPageMode = false;

// Internal static helper functions.  We don't put them in an anonymous
// namespace so they have easier access to the WebCore namespace.

static bool supportsFocus(ControlPart appearance)
{
    switch (appearance) {
        case PushButtonPart:
        case ButtonPart:
        case DefaultButtonPart:
        case TextFieldPart:
        case TextAreaPart:
            return true;
        default:
            return false;
    }

    return false;
}

static void setFixedPadding(RenderStyle* style, const int padding[4])
{
    style->setPaddingLeft(Length(padding[LeftPadding], Fixed));
    style->setPaddingRight(Length(padding[RightPadding], Fixed));
    style->setPaddingTop(Length(padding[TopPadding], Fixed));
    style->setPaddingBottom(Length(padding[BottomPadding], Fixed));
}

// Return the height of system font |font| in pixels.  We use this size by
// default for some non-form-control elements.
static float systemFontSize(const LOGFONT& font)
{
    float size = -font.lfHeight;
    if (size < 0) {
        HFONT hFont = CreateFontIndirect(&font);
        if (hFont) {
            HDC hdc = GetDC(0);  // What about printing?  Is this the right DC?
            if (hdc) {
                HGDIOBJ hObject = SelectObject(hdc, hFont);
                TEXTMETRIC tm;
                GetTextMetrics(hdc, &tm);
                SelectObject(hdc, hObject);
                ReleaseDC(0, hdc);
                size = tm.tmAscent;
            }
            DeleteObject(hFont);
        }
    }

    // The "codepage 936" bit here is from Gecko; apparently this helps make
    // fonts more legible in Simplified Chinese where the default font size is
    // too small.
    // TODO(pkasting): http://b/1119883 Since this is only used for "small
    // caption", "menu", and "status bar" objects, I'm not sure how much this
    // even matters.  Plus the Gecko patch went in back in 2002, and maybe this
    // isn't even relevant anymore.  We should investigate whether this should
    // be removed, or perhaps broadened to be "any CJK locale".
    return ((size < 12.0f) && (GetACP() == 936)) ? 12.0f : size;
}

// We aim to match IE here.
// -IE uses a font based on the encoding as the default font for form controls.
// -Gecko uses MS Shell Dlg (actually calls GetStockObject(DEFAULT_GUI_FONT),
// which returns MS Shell Dlg)
// -Safari uses Lucida Grande.
//
// TODO(ojan): Fix this!
// The only case where we know we don't match IE is for ANSI encodings. IE uses
// MS Shell Dlg there, which we render incorrectly at certain pixel sizes
// (e.g. 15px). So, for now we just use Arial.
static wchar_t* defaultGUIFont(Document* document)
{
    UScriptCode dominantScript = document->dominantScript();
    const wchar_t* family = NULL;

    // TODO(jungshik) : Special-casing of Latin/Greeek/Cyrillic should go away
    // once GetFontFamilyForScript is enhanced to support GenericFamilyType for
    // real. For now, we make sure that we use Arial to match IE for those
    // scripts.
    if (dominantScript != USCRIPT_LATIN &&
        dominantScript != USCRIPT_CYRILLIC &&
        dominantScript != USCRIPT_GREEK &&
        dominantScript != USCRIPT_INVALID_CODE) {
        family = GetFontFamilyForScript(dominantScript, GENERIC_FAMILY_NONE);
        if (family)
            return const_cast<wchar_t*>(family);
    }
    return L"Arial";
}

// Converts |points| to pixels.  One point is 1/72 of an inch.
static float pointsToPixels(float points)
{
    static float pixelsPerInch = 0.0f;
    if (!pixelsPerInch) {
        HDC hdc = GetDC(0);  // What about printing?  Is this the right DC?
        if (hdc) {  // Can this ever actually be NULL?
            pixelsPerInch = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(0, hdc);
        } else {
            pixelsPerInch = 96.0f;
        }
    }

    static const float POINTS_PER_INCH = 72.0f;
    return points / POINTS_PER_INCH * pixelsPerInch;
}

static void setSizeIfAuto(RenderStyle* style, const IntSize& size)
{
    if (style->width().isIntrinsicOrAuto())
        style->setWidth(Length(size.width(), Fixed));
    if (style->height().isAuto())
        style->setHeight(Length(size.height(), Fixed));
}

// Implement WebCore::theme() for getting the global RenderTheme.
RenderTheme* theme()
{
    static RenderThemeWin winTheme;
    return &winTheme;
}

bool RenderThemeWin::supportsFocusRing(const RenderStyle* style) const
{
   // Let webkit draw one of its halo rings around any focused element,
   // except push buttons. For buttons we use the windows PBS_DEFAULTED
   // styling to give it a blue border.
   return style->appearance() == ButtonPart ||
          style->appearance() == PushButtonPart;
}

Color RenderThemeWin::platformActiveSelectionBackgroundColor() const
{
    if (ChromiumBridge::layoutTestMode())
        return Color("#0000FF");  // Royal blue.
    if (m_findInPageMode)
        return Color(255, 150, 50, 200);  // Orange.
    COLORREF color = GetSysColor(COLOR_HIGHLIGHT);
    return Color(GetRValue(color), GetGValue(color), GetBValue(color), 255);
}

Color RenderThemeWin::platformInactiveSelectionBackgroundColor() const
{
    if (ChromiumBridge::layoutTestMode())
        return Color("#999999");  // Medium gray.
    if (m_findInPageMode)
        return Color(255, 150, 50, 200);  // Orange.
    COLORREF color = GetSysColor(COLOR_GRAYTEXT);
    return Color(GetRValue(color), GetGValue(color), GetBValue(color), 255);
}

Color RenderThemeWin::platformActiveSelectionForegroundColor() const
{
    if (ChromiumBridge::layoutTestMode())
        return Color("#FFFFCC");  // Pale yellow.
    COLORREF color = GetSysColor(COLOR_HIGHLIGHTTEXT);
    return Color(GetRValue(color), GetGValue(color), GetBValue(color), 255);
}

Color RenderThemeWin::platformInactiveSelectionForegroundColor() const
{
    return Color::white;
}

Color RenderThemeWin::platformTextSearchHighlightColor() const
{
    return Color(255, 255, 150);
}

double RenderThemeWin::caretBlinkFrequency() const
{
    // Disable the blinking caret in layout test mode, as it introduces
    // a race condition for the pixel tests. http://b/1198440
    if (ChromiumBridge::layoutTestMode())
        return 0;

    // TODO(ericroman): this should be using the platform's blink frequency.
    return RenderTheme::caretBlinkFrequency();
}

void RenderThemeWin::systemFont(int propId, Document* document, FontDescription& fontDescription) const
{
    // This logic owes much to RenderThemeSafari.cpp.
    FontDescription* cachedDesc = NULL;
    wchar_t* faceName = 0;
    float fontSize = 0;
    switch (propId) {
        case CSSValueSmallCaption:
            cachedDesc = &smallSystemFont;
            if (!smallSystemFont.isAbsoluteSize()) {
                NONCLIENTMETRICS metrics;
                win_util::GetNonClientMetrics(&metrics);
                faceName = metrics.lfSmCaptionFont.lfFaceName;
                fontSize = systemFontSize(metrics.lfSmCaptionFont);
            }
            break;
        case CSSValueMenu:
            cachedDesc = &menuFont;
            if (!menuFont.isAbsoluteSize()) {
                NONCLIENTMETRICS metrics;
                win_util::GetNonClientMetrics(&metrics);
                faceName = metrics.lfMenuFont.lfFaceName;
                fontSize = systemFontSize(metrics.lfMenuFont);
            }
            break;
        case CSSValueStatusBar:
            cachedDesc = &labelFont;
            if (!labelFont.isAbsoluteSize()) {
                NONCLIENTMETRICS metrics;
                win_util::GetNonClientMetrics(&metrics);
                faceName = metrics.lfStatusFont.lfFaceName;
                fontSize = systemFontSize(metrics.lfStatusFont);
            }
            break;
        case CSSValueWebkitMiniControl:
        case CSSValueWebkitSmallControl:
        case CSSValueWebkitControl:
            faceName = defaultGUIFont(document);
            // Why 2 points smaller?  Because that's what Gecko does.
            fontSize = DefaultFontSize - pointsToPixels(2);
            break;
        default:
            faceName = defaultGUIFont(document);
            fontSize = DefaultFontSize;
            break;
    }

    if (!cachedDesc)
        cachedDesc = &fontDescription;

    if (fontSize) {
        ASSERT(faceName);
        cachedDesc->firstFamily().setFamily(AtomicString(faceName,
                                                         wcslen(faceName)));
        cachedDesc->setIsAbsoluteSize(true);
        cachedDesc->setGenericFamily(FontDescription::NoFamily);
        cachedDesc->setSpecifiedSize(fontSize);
        cachedDesc->setWeight(FontWeightNormal);
        cachedDesc->setItalic(false);
    }
    fontDescription = *cachedDesc;
}

int RenderThemeWin::minimumMenuListSize(RenderStyle* style) const
{
    return 0;
}

void RenderThemeWin::setCheckboxSize(RenderStyle* style) const
{
    // If the width and height are both specified, then we have nothing to do.
    if (!style->width().isIntrinsicOrAuto() && !style->height().isAuto())
        return;

    // FIXME:  A hard-coded size of 13 is used.  This is wrong but necessary for now.  It matches Firefox.
    // At different DPI settings on Windows, querying the theme gives you a larger size that accounts for
    // the higher DPI.  Until our entire engine honors a DPI setting other than 96, we can't rely on the theme's
    // metrics.
    const IntSize size(13, 13);
    setSizeIfAuto(style, size);
}

void RenderThemeWin::setRadioSize(RenderStyle* style) const
{
    // Use same sizing for radio box as checkbox.
    setCheckboxSize(style);
}

bool RenderThemeWin::paintButton(RenderObject* o,
                                 const RenderObject::PaintInfo& i,
                                 const IntRect& r)
{
    const ThemeData& themeData = getThemeData(o);

    WebCore::ThemeHelperWin helper(i.context, r);
    gfx::PlatformCanvas* canvas = helper.context()->platformContext()->canvas();

    HDC hdc = canvas->beginPlatformPaint();
    int state = themeData.m_state;
    RECT renderRect = helper.rect();

    gfx::NativeTheme::instance()->PaintButton(hdc,
                                              themeData.m_part,
                                              state,
                                              themeData.m_classicState,
                                              &renderRect);
    canvas->endPlatformPaint();
    return false;
}

bool RenderThemeWin::paintTextField(RenderObject* o, const RenderObject::PaintInfo& i, const IntRect& r)
{
    return paintTextFieldInternal(o, i, r, true);
}

bool RenderThemeWin::paintSearchField(RenderObject* o, const RenderObject::PaintInfo& i, const IntRect& r)
{
    return paintTextField(o, i, r);
}

void RenderThemeWin::adjustMenuListStyle(CSSStyleSelector* selector, RenderStyle* style, Element* e) const
{
    // Height is locked to auto on all browsers.
    style->setLineHeight(RenderStyle::initialLineHeight());
}

// Used to paint unstyled menulists (i.e. with the default border)
bool RenderThemeWin::paintMenuList(RenderObject* o, const RenderObject::PaintInfo& i, const IntRect& r)
{
    int borderRight = o->borderRight();
    int borderLeft = o->borderLeft();
    int borderTop = o->borderTop();
    int borderBottom = o->borderBottom();

    // If all the borders are 0, then tell skia not to paint the border on the textfield.
    // TODO(ojan): http://b/1210017 Figure out how to get Windows to not draw individual
    // borders and then pass that to skia so we can avoid drawing any borders that are
    // set to 0. For non-zero borders, we draw the border, but webkit just draws
    // over it.
    // TODO(ojan): layout-test-mode removes borders, so we end up never drawing
    // edges in layout-test-mode. See adjustMenuListStyle, style->resetBorder().
    // We really need to remove the layout-test-mode only hacks.
    bool drawEdges = !(borderRight == 0 && borderLeft == 0 && borderTop == 0 && borderBottom == 0);

    paintTextFieldInternal(o, i, r, drawEdges);

    // Take padding and border into account.
    // If the MenuList is smaller than the size of a button, make sure to
    // shrink it appropriately and not put its x position to the left of
    // the menulist.
    const int buttonWidth = GetSystemMetrics(SM_CXVSCROLL);
    int spacingLeft = borderLeft + o->paddingLeft();
    int spacingRight = borderRight + o->paddingRight();
    int spacingTop = borderTop + o->paddingTop();
    int spacingBottom = borderBottom + o->paddingBottom();

    int buttonX;
    if (r.right() - r.x() < buttonWidth) {
        buttonX = r.x();
    } else {
        buttonX = o->style()->direction() == LTR ? r.right() - spacingRight - buttonWidth : r.x() + spacingLeft;
    }

    // Compute the rectangle of the button in the destination image.
    IntRect rect(buttonX,
                 r.y() + spacingTop,
                 std::min(buttonWidth, r.right() - r.x()),
                 r.height() - (spacingTop + spacingBottom));

    // Get the correct theme data for a textfield and paint the menu.
    WebCore::ThemeHelperWin helper(i.context, rect);
    gfx::PlatformCanvas* canvas = helper.context()->platformContext()->canvas();
    HDC hdc = canvas->beginPlatformPaint();
    RECT renderRect = helper.rect();
    gfx::NativeTheme::instance()->PaintMenuList(hdc,
                                                CP_DROPDOWNBUTTON,
                                                determineState(o),
                                                determineClassicState(o),
                                                &renderRect);
    canvas->endPlatformPaint();
    return false;
}

void RenderThemeWin::adjustMenuListButtonStyle(CSSStyleSelector* selector, RenderStyle* style, Element* e) const
{
    adjustMenuListStyle(selector, style, e);
}

// Used to paint styled menulists (i.e. with a non-default border)
bool RenderThemeWin::paintMenuListButton(RenderObject* o, const RenderObject::PaintInfo& i, const IntRect& r)
{
    return paintMenuList(o, i, r);
}

int RenderThemeWin::popupInternalPaddingLeft(RenderStyle* style) const
{
    return menuListInternalPadding(style, LeftPadding);
}

int RenderThemeWin::popupInternalPaddingRight(RenderStyle* style) const
{
    return menuListInternalPadding(style, RightPadding);
}

int RenderThemeWin::popupInternalPaddingTop(RenderStyle* style) const
{
    return menuListInternalPadding(style, TopPadding);
}

int RenderThemeWin::popupInternalPaddingBottom(RenderStyle* style) const
{
    return menuListInternalPadding(style, BottomPadding);
}

void RenderThemeWin::adjustButtonInnerStyle(RenderStyle* style) const
{
    // This inner padding matches Firefox.
    style->setPaddingTop(Length(1, Fixed));
    style->setPaddingRight(Length(3, Fixed));
    style->setPaddingBottom(Length(1, Fixed));
    style->setPaddingLeft(Length(3, Fixed));
}

// static
void RenderThemeWin::setDefaultFontSize(int fontSize) {
    DefaultFontSize = static_cast<float>(fontSize);

    // Reset cached fonts.
    smallSystemFont = menuFont = labelFont = FontDescription();
}

unsigned RenderThemeWin::determineState(RenderObject* o)
{
    unsigned result = TS_NORMAL;
    ControlPart appearance = o->style()->appearance();
    if (!isEnabled(o))
        result = TS_DISABLED;
    else if (isReadOnlyControl(o) && (TextFieldPart == appearance || TextAreaPart == appearance))
        result = ETS_READONLY; // Readonly is supported on textfields.
    else if (isPressed(o)) // Active overrides hover and focused.
        result = TS_PRESSED;
    else if (supportsFocus(appearance) && isFocused(o))
        result = ETS_FOCUSED;
    else if (isHovered(o))
        result = TS_HOT;
    if (isChecked(o))
        result += 4; // 4 unchecked states, 4 checked states.
    return result;
}

unsigned RenderThemeWin::determineClassicState(RenderObject* o)
{
    unsigned result = 0;
    if (!isEnabled(o))
        result = DFCS_INACTIVE;
    else if (isPressed(o)) // Active supersedes hover
        result = DFCS_PUSHED;
    else if (isHovered(o))
        result = DFCS_HOT;
    if (isChecked(o))
        result |= DFCS_CHECKED;
    return result;
}

ThemeData RenderThemeWin::getThemeData(RenderObject* o)
{
    ThemeData result;
    switch (o->style()->appearance()) {
        case PushButtonPart:
        case ButtonPart:
            result.m_part = BP_PUSHBUTTON;
            result.m_classicState = DFCS_BUTTONPUSH;
            break;
        case CheckboxPart:
            result.m_part = BP_CHECKBOX;
            result.m_classicState = DFCS_BUTTONCHECK;
            break;
        case RadioPart:
            result.m_part = BP_RADIOBUTTON;
            result.m_classicState = DFCS_BUTTONRADIO;
            break;
        case ListboxPart:
        case MenulistPart:
        case TextFieldPart:
        case TextAreaPart:
            result.m_part = ETS_NORMAL;
            break;
    }

    result.m_state = determineState(o);
    result.m_classicState |= determineClassicState(o);

    return result;
}

bool RenderThemeWin::paintTextFieldInternal(RenderObject* o,
                                            const RenderObject::PaintInfo& i,
                                            const IntRect& r,
                                            bool drawEdges)
{
    // Nasty hack to make us not paint the border on text fields with a
    // border-radius. Webkit paints elements with border-radius for us.
    // TODO(ojan): Get rid of this if-check once we can properly clip rounded
    // borders: http://b/1112604 and http://b/1108635
    // TODO(ojan): make sure we do the right thing if css background-clip is
    // set.
    if (o->style()->hasBorderRadius())
        return false;

    const ThemeData& themeData = getThemeData(o);

    WebCore::ThemeHelperWin helper(i.context, r);
    gfx::PlatformCanvas* canvas = helper.context()->platformContext()->canvas();

    HDC hdc = canvas->beginPlatformPaint();
    COLORREF clr = gfx::SkColorToCOLORREF(o->style()->backgroundColor().rgb());
    RECT renderRect = helper.rect();

    gfx::NativeTheme::instance()->PaintTextField(hdc,
                                                 themeData.m_part,
                                                 themeData.m_state,
                                                 themeData.m_classicState,
                                                 &renderRect,
                                                 clr,
                                                 true,
                                                 drawEdges);
    canvas->endPlatformPaint();
    return false;
}

void RenderThemeWin::getMinimalButtonPadding(Length* minXPadding) const {
    // TODO(maruel): This get messy if 1. the theme change; 2. we are serializing.
    SIZE size;
    if (SUCCEEDED(gfx::NativeTheme::instance()->GetThemePartSize(
            gfx::NativeTheme::BUTTON, 0, BP_PUSHBUTTON, PBS_NORMAL, 0, TS_MIN,
            &size))) {
        *minXPadding = Length(size.cx, Fixed);
    }
}

int RenderThemeWin::menuListInternalPadding(RenderStyle* style, int paddingType) const
{
    // This internal padding is in addition to the user-supplied padding.
    // Matches the FF behavior.
    int padding = kStyledMenuListInternalPadding[paddingType];

    // Reserve the space for right arrow here. The rest of the padding is
    // set by adjustMenuListStyle, since PopMenuWin.cpp uses the padding from
    // RenderMenuList to lay out the individual items in the popup.
    // If the MenuList actually has appearance "NoAppearance", then that means
    // we don't draw a button, so don't reserve space for it.
    const int bar_type = style->direction() == LTR ? RightPadding : LeftPadding;
    if (paddingType == bar_type && style->appearance() != NoPart)
        padding += ScrollbarTheme::nativeTheme()->scrollbarThickness();

    return padding;
}

// static
void RenderThemeWin::setFindInPageMode(bool enable) {
  if (m_findInPageMode == enable)
      return;

  m_findInPageMode = enable;
  theme()->platformColorsDidChange();
}

}  // namespace WebCore
