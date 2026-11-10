#pragma once
/* -----------------------------------------------------------------------
 * Welcome3ds — the 3DS's first screen: who watches (the profiles saved),
 * add an account, the settings, leaving.  In place of the Wii's main menu
 * and profile picker.
 * ----------------------------------------------------------------------- */
#include "../../../core/SavedProfile.h"
#include "Ui3ds.h"
#include <vector>

class Welcome3ds {
public:
    enum class Result { None, Open, Add, Settings, Remove, Exit };
    explicit Welcome3ds(const std::vector<SavedProfile>& profiles) : profiles(profiles) {}
    /* one frame: input and drawing */
    Result update(ir_t& ir);
    int selected = 0;

private:
    const std::vector<SavedProfile>& profiles;
    Ui3ds::Touch touch;
    Ui3ds::Scroller vs;
    unsigned frames = 0, removeArmed = 0;
};
