// N64 runtime browser for selecting IWAD before DOOM startup.

#ifndef __I_WAD_BROWSER_N64__
#define __I_WAD_BROWSER_N64__

void I_N64RunWadBrowser(void);
#ifdef N64_BENCH
void I_N64ForceSelectedWad(const char* path);
#endif
const char* I_N64GetSelectedWadPath(void);
const char* I_N64GetSelectedBaseIwadPath(void);
int I_N64GetSelectedPlayerCount(void);
void I_N64SetBrowserStatusMessage(const char* message);

#endif
