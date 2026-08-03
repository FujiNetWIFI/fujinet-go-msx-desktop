/* Menu/command identifiers for the Windows frontend.
 *
 * Unlike the CoCo/ADAM siblings, Machine is the ONLY radio group here (MSX
 * has no TV-input/Artifact-Colour/CPU equivalents), and it is a fixed
 * 3-entry table (MSX/MSX2/MSX2+), not a runtime-walked name table, so
 * IDM_RADIO_SPAN only needs to cover 3.
 */
#ifndef MSX_WIN_RESOURCE_H
#define MSX_WIN_RESOURCE_H

#define IDI_APPICON        101

#define IDM_RESET          201
#define IDM_IMPORT_MEDIA   202
#define IDM_FUJINET_CONFIG 204
#define IDM_FUJINET_LOG    205
#define IDM_FULLSCREEN     206
#define IDM_DEBUGGER       207
#define IDM_ABOUT          208
#define IDM_EXIT           209

#define IDM_MACHINE_BASE   300
#define IDM_RADIO_SPAN     8  /* headroom over the 3 current entries */

#endif
