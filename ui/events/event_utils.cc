// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/events/event_utils.h"

#include <vector>

#include "ui/events/event.h"
#include "ui/gfx/display.h"
#include "ui/gfx/screen.h"

namespace ui {

namespace {
int g_custom_event_types = ET_LAST;
}  // namespace

int RegisterCustomEventType() {
  return ++g_custom_event_types;
}

base::TimeDelta EventTimeForNow() {
  return base::TimeDelta::FromInternalValue(
      base::TimeTicks::Now().ToInternalValue());
}

bool ShouldDefaultToNaturalScroll() {
  return GetInternalDisplayTouchSupport() ==
      gfx::Display::TOUCH_SUPPORT_AVAILABLE;
}

gfx::Display::TouchSupport GetInternalDisplayTouchSupport() {
  gfx::Screen* screen = gfx::Screen::GetScreenByType(gfx::SCREEN_TYPE_NATIVE);
  // No screen in some unit tests.
  if (!screen)
    return gfx::Display::TOUCH_SUPPORT_UNKNOWN;
  const std::vector<gfx::Display>& displays = screen->GetAllDisplays();
  for (std::vector<gfx::Display>::const_iterator it = displays.begin();
       it != displays.end(); ++it) {
    if (it->IsInternal())
      return it->touch_support();
  }
  return gfx::Display::TOUCH_SUPPORT_UNAVAILABLE;
}

}  // namespace ui
