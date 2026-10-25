#include "pd/text/palette.h"

namespace pd::syn {
namespace {
//            plain  keyword            type                 string              number
const Color kNight[kClsCount] = {0, rgb(232, 160, 64), rgb(126, 176, 212), rgb(150, 196, 120), rgb(214, 150, 110),
    // comment          meta                tag                  attr                punct
    rgb(112, 110, 106), rgb(192, 138, 200), rgb(126, 176, 212), rgb(220, 190, 120), rgb(128, 124, 116),
    // heading          emph                code                 link
    rgb(240, 176, 80), rgb(236, 186, 150), rgb(150, 196, 120), rgb(126, 176, 212)};
const Color kPaper[kClsCount] = {0, rgb(176, 64, 16), rgb(32, 96, 150), rgb(56, 120, 40), rgb(160, 90, 20),
    rgb(146, 138, 122), rgb(128, 64, 144), rgb(32, 96, 150), rgb(150, 100, 20), rgb(140, 132, 116),
    rgb(192, 80, 16), rgb(130, 70, 40), rgb(56, 120, 40), rgb(32, 96, 150)};
const Color kPhosphor[kClsCount] = {0, rgb(200, 255, 210), rgb(150, 255, 170), rgb(50, 200, 90), rgb(130, 240, 150),
    rgb(30, 125, 55), rgb(110, 225, 140), rgb(200, 255, 210), rgb(110, 215, 130), rgb(45, 165, 80),
    rgb(210, 255, 220), rgb(170, 255, 190), rgb(50, 200, 90), rgb(140, 245, 170)};
}  // namespace

const Color* palette(int i) {
    static const Color* const all[3] = {kNight, kPaper, kPhosphor};
    return all[(i % 3 + 3) % 3];
}
}  // namespace pd::syn
