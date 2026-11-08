// SoftEther VPN Source Code - Developer Edition Master Branch
// Cedar Communication Module
//
// netrule.h
// Connect or disconnect VPN clients depending on the network state
//
// A rule matches either
//   - a wireless link: one or more SSIDs (compared with the SSID Windows is
//     currently connected to), or
//   - a wired link: the default gateway address of the wired adapter,
// and then connects or disconnects a named connection, or all connections.

#ifdef OS_WIN32

#ifndef NETRULE_H
#define NETRULE_H

#include "GlobalConst.h"

#include "Mayaqua/MayaType.h"

// Menu / command id used by the client manager
#define	NR_MENU_ID		100009

// Rule kinds
#define	NR_KIND_WIRELESS	0
#define	NR_KIND_WIRED		1

// Rule actions
#define	NR_ACTION_CONNECT		0
#define	NR_ACTION_DISCONNECT	1

// When true the connect / disconnect helpers do not ask anything: the monitor
// uses this to act without user interaction.
extern bool nr_auto_mode;

void NrMonitorStart();
void NrMonitorStop();
void NrRulesDialog(HWND hWnd);

#endif // NETRULE_H

#endif // OS_WIN32
