// SoftEther VPN Source Code - Developer Edition Master Branch
// Cedar Communication Module
//
// netrule.c
// Connect or disconnect VPN connections depending on the network state.
//
// The rules are kept in an INI file next to the client configuration:
//   %ProgramData%\SoftEther VPN Client\net_rules.ini
//
//   [rule0]
//   Enabled=1
//   Kind=wireless          ; wireless | wired
//   Ssids=Home,Office       ; wireless only, compared case insensitively
//   Gateway=192.168.1.1     ; wired only
//   Action=connect          ; connect | disconnect
//   Target=                 ; empty means every connection
//
// The monitor thread re-evaluates the rules every five seconds. A rule matches
// when the SSID Windows is connected to is one of its SSIDs (wireless) or when
// the default gateway of the wired adapter equals its gateway (wired); every
// matching rule is then applied.

#include "Cedar.h"
#include "CMInner.h"

#include "netrule.h"

#include <wlanapi.h>
#include <iphlpapi.h>
#include <iptypes.h>
#include <shlobj.h>

bool nr_auto_mode = false;

#define	NR_INI_FILE		L"net_rules.ini"
#define	NR_MAX_RULES	64
#define	NR_MONITOR_MS	5000

typedef struct NR_RULE
{
	bool Enabled;
	UINT Kind;
	LIST *Ssids;			// LIST of wchar_t *
	wchar_t Gateway[64];
	UINT Action;
	wchar_t Target[64];
} NR_RULE;

static THREAD *nr_thread = NULL;
static volatile bool nr_stop = false;

// ---------------------------------------------------------------- utilities
static void NrIniPath(wchar_t *path, UINT size)
{
	wchar_t dir[MAX_PATH];

	Zero(dir, sizeof(dir));
	if (SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, dir) != S_OK)
	{
		GetModuleFileNameW(NULL, dir, MAX_PATH);
	}
	UniStrCpy(path, size, dir);
	UniStrCat(path, size, L"\\SoftEther VPN Client");
	MakeDir(path);
	UniStrCat(path, size, L"\\");
	UniStrCat(path, size, NR_INI_FILE);
}

static bool NrSsidInRule(NR_RULE *r, wchar_t *ssid)
{
	UINT i;

	if (r == NULL || ssid == NULL || r->Ssids == NULL)
	{
		return false;
	}

	for (i = 0;i < LIST_NUM(r->Ssids);i++)
	{
		wchar_t *s = LIST_DATA(r->Ssids, i);
		if (s != NULL && UniStrCmpi(s, ssid) == 0)
		{
			return true;
		}
	}

	return false;
}

static void NrAddSsid(NR_RULE *r, wchar_t *ssid)
{
	wchar_t *s;

	if (r == NULL || IsEmptyStr(ssid))
	{
		return;
	}

	if (NrSsidInRule(r, ssid))
	{
		return;
	}

	s = CopyUniStr(ssid);
	Add(r->Ssids, s);
}

static void NrFreeRule(NR_RULE *r)
{
	UINT i;

	if (r == NULL)
	{
		return;
	}

	if (r->Ssids != NULL)
	{
		for (i = 0;i < LIST_NUM(r->Ssids);i++)
		{
			Free(LIST_DATA(r->Ssids, i));
		}
		ReleaseList(r->Ssids);
	}

	Free(r);
}

// ------------------------------------------------------------------ storage
static LIST *NrLoadRules()
{
	wchar_t path[MAX_PATH];
	wchar_t section[64], buf[2048];
	LIST *o = NewListFast(NULL);
	UINT i;

	NrIniPath(path, sizeof(path));

	for (i = 0;i < NR_MAX_RULES;i++)
	{
		NR_RULE *r;
		wchar_t *p;

		UniFormat(section, sizeof(section), L"rule%u", i);
		GetPrivateProfileStringW(section, L"Action", L"", buf, sizeof(buf) / sizeof(wchar_t), path);
		if (IsEmptyStr(buf))
		{
			if (i > 0)
			{
				break;
			}
			continue;
		}

		r = ZeroMalloc(sizeof(NR_RULE));
		r->Ssids = NewListFast(NULL);
		r->Enabled = GetPrivateProfileIntW(section, L"Enabled", 1, path) != 0;
		r->Kind = NR_KIND_WIRELESS;
		GetPrivateProfileStringW(section, L"Kind", L"wireless", buf, sizeof(buf) / sizeof(wchar_t), path);
		if (UniStrCmpi(buf, L"wired") == 0)
		{
			r->Kind = NR_KIND_WIRED;
		}
		GetPrivateProfileStringW(section, L"Action", L"connect", buf, sizeof(buf) / sizeof(wchar_t), path);
		if (UniStrCmpi(buf, L"disconnect") == 0)
		{
			r->Action = NR_ACTION_DISCONNECT;
		}
		GetPrivateProfileStringW(section, L"Gateway", L"", r->Gateway, sizeof(r->Gateway) / sizeof(wchar_t), path);
		GetPrivateProfileStringW(section, L"Target", L"", r->Target, sizeof(r->Target) / sizeof(wchar_t), path);

		// SSIDs, comma separated
		GetPrivateProfileStringW(section, L"Ssids", L"", buf, sizeof(buf) / sizeof(wchar_t), path);
		p = buf;
		while (*p != 0)
		{
			wchar_t item[128];
			UINT n = 0;

			while (*p == L' ' || *p == L',')
			{
				p++;
			}
			while (*p != 0 && *p != L',' && n < (sizeof(item) / sizeof(wchar_t)) - 1)
			{
				item[n++] = *p++;
			}
			item[n] = 0;
			Trim(item);
			NrAddSsid(r, item);
		}

		Add(o, r);
	}

	return o;
}

static void NrSaveRules(LIST *rules)
{
	wchar_t path[MAX_PATH], section[64], buf[1024];
	UINT i, j;

	NrIniPath(path, sizeof(path));

	// wipe the old content
	DeleteFileW(path);

	if (rules == NULL)
	{
		return;
	}

	for (i = 0;i < LIST_NUM(rules);i++)
	{
		NR_RULE *r = LIST_DATA(rules, i);

		UniFormat(section, sizeof(section), L"rule%u", i);
		WritePrivateProfileStringW(section, L"Enabled", r->Enabled ? L"1" : L"0", path);
		WritePrivateProfileStringW(section, L"Kind", r->Kind == NR_KIND_WIRED ? L"wired" : L"wireless", path);
		WritePrivateProfileStringW(section, L"Gateway", r->Gateway, path);
		WritePrivateProfileStringW(section, L"Action", r->Action == NR_ACTION_DISCONNECT ? L"disconnect" : L"connect", path);
		WritePrivateProfileStringW(section, L"Target", r->Target, path);

		Zero(buf, sizeof(buf));
		for (j = 0;j < LIST_NUM(r->Ssids);j++)
		{
			wchar_t *s = LIST_DATA(r->Ssids, j);
			if (j > 0)
			{
				UniStrCat(buf, sizeof(buf), L",");
			}
			UniStrCat(buf, sizeof(buf), s);
		}
		WritePrivateProfileStringW(section, L"Ssids", buf, path);
	}
}

// --------------------------------------------------------------- net probes
// The SSID of the wireless link Windows is currently connected to.
bool NrGetCurrentSsid(wchar_t *ssid, UINT size)
{
	HANDLE h = NULL;
	DWORD ver = 0, size2 = 0, ret;
	PWLAN_INTERFACE_INFO_LIST list = NULL;
	UINT i;
	bool ok = false;

	if (ssid == NULL || size == 0)
	{
		return false;
	}
	ssid[0] = 0;

	if (WlanOpenHandle(2, NULL, &ver, &h) != ERROR_SUCCESS)
	{
		return false;
	}

	if (WlanEnumInterfaces(h, NULL, &list) == ERROR_SUCCESS && list != NULL)
	{
		for (i = 0;i < list->dwNumberOfItems;i++)
		{
			PWLAN_INTERFACE_INFO info = &list->InterfaceInfo[i];
			PWLAN_CONNECTION_ATTRIBUTES attr = NULL;

			if (info->isState != wlan_interface_state_connected)
			{
				continue;
			}
			if (WlanQueryInterface(h, &info->InterfaceGuid, wlan_intf_opcode_current_connection,
				NULL, &size2, (PVOID *)&attr, NULL) != ERROR_SUCCESS || attr == NULL)
			{
				continue;
			}

			if (attr->isState == wlan_interface_state_connected && attr->wlanAssociationAttributes.dot11Ssid.uSSIDLength > 0)
			{
				char tmp[64];
				UINT n = attr->wlanAssociationAttributes.dot11Ssid.uSSIDLength;
				if (n > sizeof(tmp) - 1)
				{
					n = sizeof(tmp) - 1;
				}
				Copy(tmp, attr->wlanAssociationAttributes.dot11Ssid.ucSSID, n);
				tmp[n] = 0;
				Utf8ToUni(ssid, size, tmp);
				ok = true;
			}
			WlanFreeMemory(attr);
			if (ok)
			{
				break;
			}
		}
		WlanFreeMemory(list);
	}

	WlanCloseHandle(h, NULL);
	return ok;
}

// The default gateway of the wired adapter (if a wired link is up).
bool NrGetWiredGateway(wchar_t *gw, UINT size)
{
	PMIB_IPFORWARDTABLE table = NULL;
	DWORD dwSize = 0, i;
	bool ok = false;

	if (gw == NULL || size == 0)
	{
		return false;
	}
	gw[0] = 0;

	if (GetIpForwardTable(NULL, &dwSize, FALSE) != ERROR_INSUFFICIENT_BUFFER)
	{
		return false;
	}

	table = Malloc(dwSize);
	if (GetIpForwardTable(table, &dwSize, FALSE) != NO_ERROR)
	{
		Free(table);
		return false;
	}

	for (i = 0;i < table->dwNumEntries;i++)
	{
		MIB_IPFORWARDROW *row = &table->table[i];
		MIB_IFROW ifrow;

		if (row->dwForwardDest != 0 || row->dwForwardMask != 0)
		{
			continue;		// only default routes
		}
		if (row->dwForwardType != MIB_IPROUTE_TYPE_INDIRECT)
		{
			continue;
		}

		Zero(&ifrow, sizeof(ifrow));
		ifrow.dwIndex = row->dwForwardIfIndex;
		if (GetIfEntry(&ifrow) != NO_ERROR)
		{
			continue;
		}
		if (ifrow.dwType != IF_TYPE_ETHERNET_CSMACD)
		{
			continue;		// not a wired link (802.11 is IF_TYPE_IEEE80211)
		}

		UniFormat(gw, size, L"%u.%u.%u.%u",
			(row->dwForwardNextHop >> 0) & 0xff,
			(row->dwForwardNextHop >> 8) & 0xff,
			(row->dwForwardNextHop >> 16) & 0xff,
			(row->dwForwardNextHop >> 24) & 0xff);
		ok = true;
		break;
	}

	Free(table);
	return ok;
}

// ------------------------------------------------------------- apply actions
// Connect or disconnect one connection without asking anything.
static void NrDoConnect(wchar_t *name)
{
	nr_auto_mode = true;
	CmConnect(cm->hMainWnd, name);
	nr_auto_mode = false;
}

static void NrDoDisconnect(wchar_t *name)
{
	nr_auto_mode = true;
	CmDisconnect(cm->hMainWnd, name);
	nr_auto_mode = false;
}

// Walk the connection list of the main window and apply the action to every
// entry whose name matches (an empty name matches everything).
static void NrApplyToConnections(bool connect, wchar_t *only)
{
	HWND hWnd = cm->hMainWnd;
	LIST *targets;
	UINT i, num;

	if (hWnd == NULL)
	{
		return;
	}

	targets = NewListFast(NULL);

	num = LvNum(hWnd, L_ACCOUNT);
	for (i = 0;i < num;i++)
	{
		wchar_t *name = LvGetStr(hWnd, L_ACCOUNT, i, 0);
		wchar_t *state = LvGetStr(hWnd, L_ACCOUNT, i, 1);
		bool online;

		if (name == NULL)
		{
			continue;
		}

		online = (state != NULL && (
			UniStrCmpi(state, _UU("CM_ACCOUNT_ONLINE")) == 0 ||
			UniStrCmpi(state, _UU("CM_ACCOUNT_CONNECTING")) == 0));

		if (online == connect)
		{
			// already in the wanted state
			Free(name);
			Free(state);
			continue;
		}

		if (IsEmptyStr(only) == false && UniStrCmpi(only, name) != 0)
		{
			Free(name);
			Free(state);
			continue;
		}

		Add(targets, name);
		Free(state);
	}

	for (i = 0;i < LIST_NUM(targets);i++)
	{
		wchar_t *name = LIST_DATA(targets, i);
		if (connect)
		{
			NrDoConnect(name);
		}
		else
		{
			NrDoDisconnect(name);
		}
		Free(name);
	}

	ReleaseList(targets);
}

// Evaluate every rule once and act on the matching ones.
void NrApplyRules()
{
	LIST *rules;
	wchar_t ssid[128], gw[64];

	if (cm == NULL || cm->hMainWnd == NULL)
	{
		return;
	}

	rules = NrLoadRules();
	if (rules == NULL)
	{
		return;
	}

	NrGetCurrentSsid(ssid, sizeof(ssid));
	NrGetWiredGateway(gw, sizeof(gw));

	if (LIST_NUM(rules) > 0 && (ssid[0] != 0 || gw[0] != 0))
	{
		UINT i;
		for (i = 0;i < LIST_NUM(rules);i++)
		{
			NR_RULE *r = LIST_DATA(rules, i);
			bool match = false;

			if (r->Enabled == false)
			{
				continue;
			}

			if (r->Kind == NR_KIND_WIRELESS)
			{
				match = (ssid[0] != 0) && NrSsidInRule(r, ssid);
			}
			else
			{
				match = (gw[0] != 0) && UniStrCmpi(r->Gateway, gw) == 0;
			}

			if (match)
			{
				NrApplyToConnections(r->Action == NR_ACTION_CONNECT, r->Target);
			}
		}
	}

	for (i = 0;i < LIST_NUM(rules);i++)
	{
		NrFreeRule(LIST_DATA(rules, i));
	}
	ReleaseList(rules);
}

// -------------------------------------------------------------------- thread
static void NrMonitorThread(THREAD *t, void *param)
{
	while (nr_stop == false)
	{
		NrApplyRules();
		Sleep(NR_MONITOR_MS);
	}
}

void NrMonitorStart()
{
	if (nr_thread != NULL)
	{
		return;
	}

	nr_stop = false;
	nr_thread = NewThread(NrMonitorThread, NULL);
}

void NrMonitorStop()
{
	if (nr_thread == NULL)
	{
		return;
	}

	nr_stop = true;
	WaitThread(nr_thread, INFINITE);
	ReleaseThread(nr_thread);
	nr_thread = NULL;
}

// ------------------------------------------------------------------ rule UI
// One window: the rule list on the left, the fields of the selected rule on the
// right. Nothing is modal, so the whole editor stays small and predictable.
#define	NRW_CLASS	L"NrRulesWindow"
#define	NRW_W		760
#define	NRW_H		420

#define	NR_ID_LIST		2001
#define	NR_ID_NEW_WIFI	2002
#define	NR_ID_NEW_WIRE	2003
#define	NR_ID_DELETE	2004
#define	NR_ID_SAVE		2005
#define	NR_ID_CLOSE		2006
#define	NR_ID_ENABLED	2007
#define	NR_ID_SSIDS		2008
#define	NR_ID_PICK		2009
#define	NR_ID_GW		2010
#define	NR_ID_USEGW		2011
#define	NR_ID_ACTION	2012
#define	NR_ID_TARGET	2013
#define	NR_ID_ALL		2014
#define	NR_ID_STATUS	2015

typedef struct NRW_STATE
{
	HWND hWnd;
	HWND hList;
	HWND hEnabled, hSsids, hPick, hGw, hUseGw, hAction, hTarget, hAll, hStatus;
	LIST *Rules;
	int Current;			// selected index, -1 when none
	bool Loading;			// true while filling the controls
} NRW_STATE;

static NRW_STATE *nrw = NULL;

static void NrwFillList()
{
	UINT i;

	SendMessageW(nrw->hList, LB_RESETCONTENT, 0, 0);
	for (i = 0;i < LIST_NUM(nrw->Rules);i++)
	{
		NR_RULE *r = LIST_DATA(nrw->Rules, i);
		wchar_t line[512];

		if (r->Kind == NR_KIND_WIRELESS)
		{
			wchar_t ssids[256] = L"";
			UINT j;
			for (j = 0;j < LIST_NUM(r->Ssids);j++)
			{
				if (j > 0)
				{
					UniStrCat(ssids, sizeof(ssids), L", ");
				}
				UniStrCat(ssids, sizeof(ssids), LIST_DATA(r->Ssids, j));
			}
			UniFormat(line, sizeof(line), L"%s  Wi-Fi [%s]  -> %s %s",
				r->Enabled ? L"(on)" : L"(off)",
				ssids,
				r->Action == NR_ACTION_CONNECT ? L"connect" : L"disconnect",
				IsEmptyStr(r->Target) ? L"ALL" : r->Target);
		}
		else
		{
			UniFormat(line, sizeof(line), L"%s  Wired gw %s  -> %s %s",
				r->Enabled ? L"(on)" : L"(off)",
				r->Gateway,
				r->Action == NR_ACTION_CONNECT ? L"connect" : L"disconnect",
				IsEmptyStr(r->Target) ? L"ALL" : r->Target);
		}
		SendMessageW(nrw->hList, LB_ADDSTRING, 0, (LPARAM)line);
	}
}

static void NrwLoadFields(int index)
{
	NR_RULE *r;
	wchar_t buf[512];

	nrw->Loading = true;
	nrw->Current = index;

	if (index < 0 || index >= (int)LIST_NUM(nrw->Rules))
	{
		EnableWindow(nrw->hEnabled, FALSE);
		EnableWindow(nrw->hSsids, FALSE);
		EnableWindow(nrw->hPick, FALSE);
		EnableWindow(nrw->hGw, FALSE);
		EnableWindow(nrw->hUseGw, FALSE);
		EnableWindow(nrw->hAction, FALSE);
		EnableWindow(nrw->hTarget, FALSE);
		EnableWindow(nrw->hAll, FALSE);
		SetWindowTextW(nrw->hSsids, L"");
		SetWindowTextW(nrw->hGw, L"");
		SetWindowTextW(nrw->hTarget, L"");
		nrw->Loading = false;
		return;
	}

	r = LIST_DATA(nrw->Rules, index);

	EnableWindow(nrw->hEnabled, TRUE);
	EnableWindow(nrw->hAction, TRUE);
	EnableWindow(nrw->hTarget, TRUE);
	EnableWindow(nrw->hAll, TRUE);

	SendMessageW(nrw->hEnabled, BM_SETCHECK, r->Enabled ? BST_CHECKED : BST_UNCHECKED, 0);
	SendMessageW(nrw->hAction, CB_SETCURSEL, r->Action == NR_ACTION_CONNECT ? 0 : 1, 0);
	SetWindowTextW(nrw->hTarget, r->Target);
	SendMessageW(nrw->hAll, BM_SETCHECK, IsEmptyStr(r->Target) ? BST_CHECKED : BST_UNCHECKED, 0);
	EnableWindow(nrw->hTarget, IsEmptyStr(r->Target) ? FALSE : TRUE);

	if (r->Kind == NR_KIND_WIRELESS)
	{
		UINT j;
		Zero(buf, sizeof(buf));
		for (j = 0;j < LIST_NUM(r->Ssids);j++)
		{
			if (j > 0)
			{
				UniStrCat(buf, sizeof(buf), L",");
			}
			UniStrCat(buf, sizeof(buf), LIST_DATA(r->Ssids, j));
		}
		SetWindowTextW(nrw->hSsids, buf);
		SetWindowTextW(nrw->hGw, L"");
		EnableWindow(nrw->hSsids, TRUE);
		EnableWindow(nrw->hPick, TRUE);
		EnableWindow(nrw->hGw, FALSE);
		EnableWindow(nrw->hUseGw, FALSE);
	}
	else
	{
		SetWindowTextW(nrw->hGw, r->Gateway);
		SetWindowTextW(nrw->hSsids, L"");
		EnableWindow(nrw->hSsids, FALSE);
		EnableWindow(nrw->hPick, FALSE);
		EnableWindow(nrw->hGw, TRUE);
		EnableWindow(nrw->hUseGw, TRUE);
	}

	nrw->Loading = false;
}

// Copy the fields back into the selected rule.
static void NrwStoreFields()
{
	NR_RULE *r;
	wchar_t buf[512], item[128], *p;
	UINT n;

	if (nrw->Loading || nrw->Current < 0 || nrw->Current >= (int)LIST_NUM(nrw->Rules))
	{
		return;
	}

	r = LIST_DATA(nrw->Rules, nrw->Current);

	r->Enabled = SendMessageW(nrw->hEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
	r->Action = SendMessageW(nrw->hAction, CB_GETCURSEL, 0, 0) == 0 ? NR_ACTION_CONNECT : NR_ACTION_DISCONNECT;
	GetWindowTextW(nrw->hTarget, r->Target, sizeof(r->Target) / sizeof(wchar_t));
	Trim(r->Target);

	if (IsEmptyStr(r->Target))
	{
		r->Target[0] = 0;
	}

	if (r->Kind == NR_KIND_WIRELESS)
	{
		// re-parse the SSID list
		GetWindowTextW(nrw->hSsids, buf, sizeof(buf) / sizeof(wchar_t));
		for (n = 0;n < LIST_NUM(r->Ssids);n++)
		{
			Free(LIST_DATA(r->Ssids, n));
		}
		ReleaseList(r->Ssids);
		r->Ssids = NewListFast(NULL);

		p = buf;
		while (*p != 0)
		{
			UINT k = 0;
			while (*p == L' ' || *p == L',')
			{
				p++;
			}
			while (*p != 0 && *p != L',' && k < (sizeof(item) / sizeof(wchar_t)) - 1)
			{
				item[k++] = *p++;
			}
			item[k] = 0;
			Trim(item);
			NrAddSsid(r, item);
		}
	}
	else
	{
		GetWindowTextW(nrw->hGw, r->Gateway, sizeof(r->Gateway) / sizeof(wchar_t));
		Trim(r->Gateway);
	}
}

static NR_RULE *NrwNewRule(UINT kind)
{
	NR_RULE *r = ZeroMalloc(sizeof(NR_RULE));
	r->Enabled = true;
	r->Kind = kind;
	r->Ssids = NewListFast(NULL);
	r->Action = NR_ACTION_CONNECT;
	return r;
}

static void NrwOnCommand(HWND hWnd, UINT id)
{
	switch (id)
	{
	case NR_ID_NEW_WIFI:
	case NR_ID_NEW_WIRE:
		NrwStoreFields();
		Add(nrw->Rules, NrwNewRule(id == NR_ID_NEW_WIFI ? NR_KIND_WIRELESS : NR_KIND_WIRED));
		NrwFillList();
		SendMessageW(nrw->hList, LB_SETCURSEL, LIST_NUM(nrw->Rules) - 1, 0);
		NrwLoadFields(LIST_NUM(nrw->Rules) - 1);
		SetFocus(nrw->hSsids);
		break;

	case NR_ID_DELETE:
		if (nrw->Current >= 0 && nrw->Current < (int)LIST_NUM(nrw->Rules))
		{
			NrFreeRule(LIST_DATA(nrw->Rules, nrw->Current));
			Delete(nrw->Rules, LIST_DATA(nrw->Rules, nrw->Current));
			NrwFillList();
			NrwLoadFields(nrw->Current < (int)LIST_NUM(nrw->Rules) ? nrw->Current : (int)LIST_NUM(nrw->Rules) - 1);
			SendMessageW(nrw->hList, LB_SETCURSEL, nrw->Current, 0);
		}
		break;

	case NR_ID_SAVE:
		NrwStoreFields();
		NrSaveRules(nrw->Rules);
		NrwFillList();
		SetWindowTextW(nrw->hStatus, L"Rules saved. The monitor applies them every 5 seconds.");
		break;

	case NR_ID_PICK:
	{
		wchar_t ssid[128];
		if (NrGetCurrentSsid(ssid, sizeof(ssid)) && ssid[0] != 0)
		{
			wchar_t buf[512];
			GetWindowTextW(nrw->hSsids, buf, sizeof(buf) / sizeof(wchar_t));
			if (buf[0] != 0)
			{
				UniStrCat(buf, sizeof(buf), L",");
			}
			UniStrCat(buf, sizeof(buf), ssid);
			SetWindowTextW(nrw->hSsids, buf);
		}
		else
		{
			SetWindowTextW(nrw->hStatus, L"No Wi-Fi link is connected at the moment.");
		}
		break;
	}

	case NR_ID_USEGW:
	{
		wchar_t gw[64];
		if (NrGetWiredGateway(gw, sizeof(gw)) && gw[0] != 0)
		{
			SetWindowTextW(nrw->hGw, gw);
		}
		else
		{
			SetWindowTextW(nrw->hStatus, L"No wired default gateway was found.");
		}
		break;
	}

	case NR_ID_ALL:
		EnableWindow(nrw->hTarget, SendMessageW(nrw->hAll, BM_GETCHECK, 0, 0) != BST_CHECKED);
		if (SendMessageW(nrw->hAll, BM_GETCHECK, 0, 0) == BST_CHECKED)
		{
			SetWindowTextW(nrw->hTarget, L"");
		}
		break;
	}
}

static UINT NrwProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CLOSE:
		DestroyWindow(hWnd);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	case WM_COMMAND:
		if (HIWORD(wParam) == LBN_SELCHANGE && LOWORD(wParam) == NR_ID_LIST)
		{
			int sel = (int)SendMessageW(nrw->hList, LB_GETCURSEL, 0, 0);
			if (sel != nrw->Current)
			{
				NrwStoreFields();
				NrwLoadFields(sel);
			}
			break;
		}
		if (HIWORD(wParam) == CBN_SELCHANGE)
		{
			break;
		}
		NrwOnCommand(hWnd, LOWORD(wParam));
		break;

	case WM_CTLCOLORSTATIC:
		SetBkMode((HDC)wParam, TRANSPARENT);
		return 0;
	}

	return 0;
}

// Show the rules editor. Blocks until the window is closed.
void NrRulesDialog(HWND hParent)
{
	MSG msg;
	HFONT font;
	HWND h, x;
	UINT i;
	WNDCLASSW wc;

	if (nrw != NULL)
	{
		SetForegroundWindow(nrw->hWnd);
		return;
	}

	nrw = ZeroMalloc(sizeof(NRW_STATE));
	nrw->Rules = NrLoadRules();
	nrw->Current = -1;

	Zero(&wc, sizeof(wc));
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = (WNDPROC)NrwProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.lpszClassName = NRW_CLASS;
	RegisterClassW(&wc);

	h = CreateWindowExW(WS_EX_DLGMODALFRAME, NRW_CLASS, L"Network connection rules",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
		CW_USEDEFAULT, CW_USEDEFAULT, NRW_W, NRW_H, hParent, NULL, wc.hInstance, NULL);
	if (h == NULL)
	{
		return;
	}
	nrw->hWnd = h;
	font = (HFONT)SendMessageW(hParent, WM_GETFONT, 0, 0);

	x = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
		10, 10, 380, 340, h, (HMENU)(UINT_PTR)NR_ID_LIST, NULL, NULL);
	nrw->hList = x;
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);

	x = CreateWindowExW(0, L"BUTTON", L"New Wi-Fi rule", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		10, 356, 120, 26, h, (HMENU)(UINT_PTR)NR_ID_NEW_WIFI, NULL, NULL);
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);
	x = CreateWindowExW(0, L"BUTTON", L"New wired rule", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		136, 356, 120, 26, h, (HMENU)(UINT_PTR)NR_ID_NEW_WIRE, NULL, NULL);
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);
	x = CreateWindowExW(0, L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		262, 356, 60, 26, h, (HMENU)(UINT_PTR)NR_ID_DELETE, NULL, NULL);
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);
	x = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
		328, 356, 62, 26, h, (HMENU)(UINT_PTR)NR_ID_SAVE, NULL, NULL);
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);

	// right hand side: the fields of the selected rule
	x = CreateWindowExW(0, L"BUTTON", L"Rule enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
		400, 12, 120, 22, h, (HMENU)(UINT_PTR)NR_ID_ENABLED, NULL, NULL);
	nrw->hEnabled = x;
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);

	x = CreateWindowExW(0, L"STATIC", L"Wi-Fi SSIDs (comma separated):", WS_CHILD | WS_VISIBLE,
		400, 44, 350, 18, h, NULL, NULL, NULL);
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);
	x = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 400, 64, 280, 24, h, (HMENU)(UINT_PTR)NR_ID_SSIDS, NULL, NULL);
	nrw->hSsids = x;
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);
	x = CreateWindowExW(0, L"BUTTON", L"Use current", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		686, 64, 64, 24, h, (HMENU)(UINT_PTR)NR_ID_PICK, NULL, NULL);
	nrw->hPick = x;
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);

	x = CreateWindowExW(0, L"STATIC", L"Wired default gateway:", WS_CHILD | WS_VISIBLE,
		400, 96, 350, 18, h, NULL, NULL, NULL);
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);
	x = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 400, 116, 280, 24, h, (HMENU)(UINT_PTR)NR_ID_GW, NULL, NULL);
	nrw->hGw = x;
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);
	x = CreateWindowExW(0, L"BUTTON", L"Use current", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		686, 116, 64, 24, h, (HMENU)(UINT_PTR)NR_ID_USEGW, NULL, NULL);
	nrw->hUseGw = x;
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);

	x = CreateWindowExW(0, L"STATIC", L"Action:", WS_CHILD | WS_VISIBLE,
		400, 152, 60, 18, h, NULL, NULL, NULL);
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);
	x = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
		400, 170, 160, 120, h, (HMENU)(UINT_PTR)NR_ID_ACTION, NULL, NULL);
	nrw->hAction = x;
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageW(x, CB_ADDSTRING, 0, (LPARAM)L"Connect the connection");
	SendMessageW(x, CB_ADDSTRING, 0, (LPARAM)L"Disconnect the connection");

	x = CreateWindowExW(0, L"BUTTON", L"All connections", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
		400, 200, 160, 22, h, (HMENU)(UINT_PTR)NR_ID_ALL, NULL, NULL);
	nrw->hAll = x;
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);

	x = CreateWindowExW(0, L"STATIC", L"Connection name:", WS_CHILD | WS_VISIBLE,
		400, 228, 350, 18, h, NULL, NULL, NULL);
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);
	x = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 400, 248, 300, 24, h, (HMENU)(UINT_PTR)NR_ID_TARGET, NULL, NULL);
	nrw->hTarget = x;
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);

	x = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
		400, 284, 350, 40, h, (HMENU)(UINT_PTR)NR_ID_STATUS, NULL, NULL);
	nrw->hStatus = x;
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);

	x = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		400, 356, 80, 26, h, (HMENU)(UINT_PTR)NR_ID_CLOSE, NULL, NULL);
	SendMessageW(x, WM_SETFONT, (WPARAM)font, TRUE);

	NrwFillList();
	if (LIST_NUM(nrw->Rules) > 0)
	{
		SendMessageW(nrw->hList, LB_SETCURSEL, 0, 0);
		NrwLoadFields(0);
	}
	else
	{
		NrwLoadFields(-1);
	}

	SetWindowTextW(nrw->hStatus, L"Rules are stored in the client configuration folder (net_rules.ini).");

	if (hParent != NULL)
	{
		EnableWindow(hParent, FALSE);
	}

	// modal loop
	while (GetMessageW(&msg, NULL, 0, 0) > 0)
	{
		if (msg.hwnd == h && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
		{
			DestroyWindow(h);
			continue;
		}
		if (IsDialogMessageW(h, &msg) == false)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	if (hParent != NULL)
	{
		EnableWindow(hParent, TRUE);
		SetActiveWindow(hParent);
	}

	NrwStoreFields();
	for (i = 0;i < LIST_NUM(nrw->Rules);i++)
	{
		NrFreeRule(LIST_DATA(nrw->Rules, i));
	}
	ReleaseList(nrw->Rules);
	Free(nrw);
	nrw = NULL;
}
