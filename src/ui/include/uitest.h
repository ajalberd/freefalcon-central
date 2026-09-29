#ifndef _UITEST_H_
#define _UITEST_H_

// Artscout - 2026: a scripted UI test harness, so a change to the menus can be checked by running the
// game rather than by asking someone to look. "FFViper.exe -nomovie -uitest <script>" runs the script
// (a path relative to <exe dir>\uitest\) on its own thread once ui95 is up: it waits for windows,
// clicks controls through the real mouse path, and writes images and layout dumps to
// <exe dir>\uitest\out\. The last line of out\result.log says how the run ended. See UI-OVERHAUL.md.

void UiTest_SetScript(const char *arg); // from the command line
bool UiTest_Requested(); // -uitest was given (VR is forced off for the run)
void UiTest_Start(); // from UI_Startup, once gMainHandler exists

#endif
