// The three PicoCalc Writer themes (Night is also picocalc_ui's THEME_NIGHT).
#include "pd/ui.h"

namespace pd::ui {

//                          name        bg                    fg                    dim
const Theme THEME_NIGHT = {"Night", rgb(18, 18, 20), rgb(214, 208, 196), rgb(110, 108, 104),
                           // sel             cursor
                           rgb(52, 70, 92), rgb(232, 160, 64),
                           // status bg/fg
                           rgb(26, 26, 30), rgb(120, 118, 112),
                           // panel bg/fg
                           rgb(34, 34, 40), rgb(214, 208, 196),
                           // highlight bg/fg
                           rgb(232, 160, 64), rgb(18, 18, 20),
                           // accent, error
                           rgb(232, 160, 64), rgb(229, 115, 115)};

const Theme THEME_PAPER = {"Paper", rgb(244, 239, 228), rgb(43, 43, 43), rgb(140, 132, 116),
                           rgb(200, 214, 230), rgb(192, 80, 16),
                           rgb(234, 228, 214), rgb(130, 122, 106),
                           rgb(252, 249, 242), rgb(43, 43, 43),
                           rgb(192, 80, 16), rgb(252, 249, 242),
                           rgb(192, 80, 16), rgb(190, 40, 40)};

const Theme THEME_PHOSPHOR = {"Phosphor", rgb(0, 10, 2), rgb(80, 255, 120), rgb(30, 120, 50),
                              rgb(20, 70, 30), rgb(180, 255, 190),
                              rgb(0, 18, 6), rgb(30, 130, 55),
                              rgb(0, 24, 8), rgb(80, 255, 120),
                              rgb(80, 255, 120), rgb(0, 10, 2),
                              rgb(80, 255, 120), rgb(255, 120, 80)};

const Theme& theme(int i) {
    static const Theme* const all[kThemeCount] = {&THEME_NIGHT, &THEME_PAPER, &THEME_PHOSPHOR};
    return *all[(i % kThemeCount + kThemeCount) % kThemeCount];
}

}  // namespace pd::ui
