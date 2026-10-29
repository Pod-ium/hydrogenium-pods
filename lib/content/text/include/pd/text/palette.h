// PicoPods - syntax colours matching the three pd::ui themes (from PicoCalc
// Writer). palette(i)[cls] is the colour of syn::Cls cls; Plain uses the
// theme's fg, so palette(i)[Plain] is 0.
#pragma once
#include "pd/core.h"
#include "pd/text/syntax.h"

namespace pd::syn {
const Color* palette(int theme_index);   // kClsCount entries; index wraps like ui::theme()
}
