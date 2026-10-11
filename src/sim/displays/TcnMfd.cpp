// Artscout - 2026: the TCN MFD format, TO 1F-16CMAM-34-1-1 2.1.6.16 (figure on manual p.80).
//
// Backup TACAN control: channel (1-126), band (X/Y) and mode, shown for whichever source is
// live -- the UFC, or the AUX COMM panel when its master switch has taken over.
//
// Base page (right column, OSB 6-10 = button 5-9):
//   6  mode  "AA T/R" / "T/R"  (this sim has no receive-only mode)
//   7  up triangle (INC)   8  "CH" + channel (opens the keypad)   9  down triangle (DEC)
//   10 band X / Y
// Keypad page: ENTR RTN RCL on OSB 2-4, digits 1-5 down the left column (OSB 20 to 16) and
// 6 7 8 9 0 down the right (OSB 6-10), "* nnn *" in the middle. A third digit commits at once;
// anything outside 1-126 is dropped. The bottom format row stays on both pages.

#include "stdhdr.h"
#include "mfd.h"
#include "navsystem.h"
#include "graphics/include/render2d.h"
#include "otwdrive.h"
#include "cpmanager.h"
#include "icp.h"
#include "aircrft.h"
#include "simdrive.h"

extern NavigationSystem* gNavigationSys;

namespace
{
const int kMinChannel = 1;
const int kMaxChannel = 126;

NavigationSystem::Tacan_Channel_Src LiveSource()
{
    return gNavigationSys ? gNavigationSys->GetControlSrc() : NavigationSystem::ICP;
}

// The triangle the real page draws beside OSB 7 (up) and OSB 9 (down), against the right edge.
void DrawArrow(DrawableClass* d, VirtualDisplay* display, int button, bool up)
{
    static const float arrowH = 0.0375f;
    static const float arrowW = 0.0433f;
    float x, y;
    d->GetButtonPos(button, &x, &y);

    display->AdjustOriginInViewport(x - arrowW * 1.5f, y);
    const float h = up ? arrowH : -arrowH;
    display->Tri(0.0F, h, arrowW, -h, -arrowW, -h);
    display->CenterOriginInViewport();
}

// button index -> keypad digit; -1 when the button is not on the keypad
int KeypadDigit(int button)
{
    if (button >= 15 and button <= 19) // left column, OSB 16 at the bottom: 5 4 3 2 1 upward
        return 20 - button;

    if (button >= 5 and button <= 9) // right column: 6 7 8 9 0 downward
        return button == 9 ? 0 : button + 1;

    return -1;
}
} // namespace

void TcnMfdDrawable::CommitEntry(void)
{
    if (gNavigationSys and digits > 0 and entry >= kMinChannel and entry <= kMaxChannel)
    {
        NavigationSystem::Tacan_Channel_Src src = LiveSource();
        gNavigationSys->SetTacanChannel(src, entry, gNavigationSys->GetTacanBand(src));
    }

    entering = false;
    digits = 0;
    entry = 0;
}

void TcnMfdDrawable::Display(VirtualDisplay* newDisplay)
{
    display = newDisplay;

    if (not gNavigationSys)
    {
        display->TextCenterVertical(0.0f, 0.0f, "TCN OFF");
        BottomRow();
        return;
    }

    NavigationSystem::Tacan_Channel_Src src = LiveSource();

    int channel = 0;
    TacanList::StationSet band = TacanList::X;
    gNavigationSys->GetTacanChannel(src, &channel, &band);
    const bool aa = gNavigationSys->GetDomain(src) == TacanList::AA;
    char buf[32];

    if (entering)
    {
        LabelButton(1, "ENTR");
        LabelButton(2, "RTN");
        LabelButton(3, "RCL");

        for (int b = 5; b <= 9; b++)
        {
            sprintf(buf, "%d", KeypadDigit(b));
            LabelButton(b, buf);
        }

        for (int b = 15; b <= 19; b++)
        {
            sprintf(buf, "%d", KeypadDigit(b));
            LabelButton(b, buf);
        }

        display->TextCenter(0.0f, 0.1f, "ENTER");
        display->TextCenter(0.0f, 0.1f - display->TextHeight(), "TACAN CHANNEL");

        // before the first digit the box shows the channel now selected
        sprintf(buf, "* %d *", digits > 0 ? entry : channel);
        display->TextCenter(0.0f, 0.1f - 3.0f * display->TextHeight(), buf);
    }
    else
    {
        LabelButton(5, aa ? "AA T/R" : "T/R");
        DrawArrow(this, display, 6, true);
        sprintf(buf, "%d", channel);
        LabelButton(7, "CH", buf);
        DrawArrow(this, display, 8, false);
        LabelButton(9, band == TacanList::X ? "X" : "Y");
    }

    BottomRow();
}

void TcnMfdDrawable::PushButton(int whichButton, int whichMFD)
{
    if (not gNavigationSys or (whichButton >= 10 and whichButton <= 14))
    {
        MfdDrawable::PushButton(whichButton, whichMFD);
        return;
    }

    NavigationSystem::Tacan_Channel_Src src = LiveSource();

    if (entering)
    {
        const int digit = KeypadDigit(whichButton);

        if (digit >= 0)
        {
            if (digits < 3)
            {
                entry = entry * 10 + digit;
                digits++;

                // a third digit can only be 100-126; commit as soon as it is typed
                if (digits == 3)
                    CommitEntry();
            }
        }
        else if (whichButton == 1) // ENTR
        {
            CommitEntry();
        }
        else if (whichButton == 2) // RTN
        {
            entering = false;
            digits = 0;
            entry = 0;
        }
        else if (whichButton == 3) // RCL: drop what was typed; the box shows the value in use again
        {
            entry = 0;
            digits = 0;
        }

        return;
    }

    switch (whichButton)
    {
    case 5: // mode
        gNavigationSys->ToggleDomain(src);
        break;

    case 6: // INC
    case 8: // DEC
    {
        int channel = gNavigationSys->GetTacanChannel(src);
        channel += (whichButton == 6) ? 1 : -1;

        if (channel > kMaxChannel)
            channel = kMinChannel;
        else if (channel < kMinChannel)
            channel = kMaxChannel;

        gNavigationSys->SetTacanChannel(src, channel, gNavigationSys->GetTacanBand(src));
    }
    break;

    case 7: // channel keypad
        entering = true;
        digits = 0;
        entry = 0;
        break;

    case 9: // band
        gNavigationSys->StepTacanBand(src);
        break;

    default:
        MfdDrawable::PushButton(whichButton, whichMFD);
        break;
    }
}
