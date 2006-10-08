/************************************************************************ 
 * This file is part of PDCurses. PDCurses is public domain software;	*
 * you may use it for any purpose. This software is provided AS IS with	*
 * NO WARRANTY whatsoever.						*
 *									*
 * If you use PDCurses in an application, an acknowledgement would be	*
 * appreciated, but is not mandatory. If you make corrections or	*
 * enhancements to PDCurses, please forward them to the current		*
 * maintainer for the benefit of other users.				*
 *									*
 * No distribution of modified PDCurses code may be made under the name	*
 * "PDCurses", except by the current maintainer. (Although PDCurses is	*
 * public domain, the name is a trademark.)				*
 *									*
 * See the file maintain.er for details of the current maintainer.	*
 ************************************************************************/

#include "pdcwin.h"

RCSID("$Id: pdcscrn.c,v 1.63 2006/10/08 23:46:54 wmcbrine Exp $");

#define PDC_RESTORE_NONE     0
#define PDC_RESTORE_BUFFER   1
#define PDC_RESTORE_WINDOW   2

/* Struct for storing console registry keys, and for use with the 
   undocumented WM_SETCONSOLEINFO message. Originally by James Brown, 
   www.catch22.net. */

static struct
{
	ULONG		Length;
	COORD		ScreenBufferSize;
	COORD		WindowSize;
	ULONG		WindowPosX;
	ULONG		WindowPosY;

	COORD		FontSize;
	ULONG		FontFamily;
	ULONG		FontWeight;
	WCHAR		FaceName[32];

	ULONG		CursorSize;
	ULONG		FullScreen;
	ULONG		QuickEdit;
	ULONG		AutoPosition;
	ULONG		InsertMode;
	
	USHORT		ScreenColors;
	USHORT		PopupColors;
	ULONG		HistoryNoDup;
	ULONG		HistoryBufferSize;
	ULONG		NumberOfHistoryBuffers;
	
	COLORREF	ColorTable[16];

	ULONG		CodePage;
	HWND		Hwnd;

	WCHAR		ConsoleTitle[0x100];
} console_info;

HANDLE hConOut = INVALID_HANDLE_VALUE;
HANDLE hConIn = INVALID_HANDLE_VALUE;

static CONSOLE_SCREEN_BUFFER_INFO orig_scr;

static CHAR_INFO *ciSaveBuffer = NULL;
static DWORD dwConsoleMode = 0;

static bool isNT;

static HWND FindConsoleHandle(void)
{
	TCHAR orgtitle[1024], temptitle[1024];
	HWND wnd;

	GetConsoleTitle(orgtitle, 1024);

	wsprintf(temptitle, TEXT("%d/%d"),
		GetTickCount(), GetCurrentProcessId());
	SetConsoleTitle(temptitle);

	Sleep(40);

	wnd = FindWindow(NULL, temptitle);

	SetConsoleTitle(orgtitle);

	return wnd;
}

/* Undocumented console message */

#define WM_SETCONSOLEINFO	(WM_USER + 201)

/* Wrapper around WM_SETCONSOLEINFO. We need to create the necessary 
   section (file-mapping) object in the context of the process which 
   owns the console, before posting the message. Originally by JB. */

static void SetConsoleInfo(void)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi;
        CONSOLE_CURSOR_INFO cci;
	DWORD   dwConsoleOwnerPid;
	HANDLE  hProcess;
	HANDLE	hSection, hDupSection;
	PVOID   ptrView = 0;
	
	/* Each-time initialization for console_info */

	GetConsoleCursorInfo(hConOut, &cci);
	console_info.CursorSize = cci.dwSize;

	GetConsoleScreenBufferInfo(hConOut, &csbi);
	console_info.ScreenBufferSize = csbi.dwSize;

	console_info.WindowSize.X = csbi.srWindow.Right - 
		csbi.srWindow.Left + 1;

	console_info.WindowSize.Y = csbi.srWindow.Bottom - 
		csbi.srWindow.Top + 1;

	console_info.WindowPosX = csbi.srWindow.Left;
	console_info.WindowPosY = csbi.srWindow.Top;

	/* Open the process which "owns" the console */

	GetWindowThreadProcessId(console_info.Hwnd, &dwConsoleOwnerPid);
	
	hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwConsoleOwnerPid);

	/* Create a SECTION object backed by page-file, then map a view 
	   of this section into the owner process so we can write the 
	   contents of the CONSOLE_INFO buffer into it */

	hSection = CreateFileMapping(INVALID_HANDLE_VALUE, 0,
		PAGE_READWRITE, 0, sizeof(console_info), 0);

	/* Copy our console structure into the section-object */

	ptrView = MapViewOfFile(hSection, FILE_MAP_WRITE|FILE_MAP_READ,
		0, 0, sizeof(console_info));

	memcpy(ptrView, &console_info, sizeof(console_info));

	UnmapViewOfFile(ptrView);

	/* Map the memory into owner process */

	DuplicateHandle(GetCurrentProcess(), hSection, hProcess,
		&hDupSection, 0, FALSE, DUPLICATE_SAME_ACCESS);

	/* Send console window the "update" message */

	SendMessage(console_info.Hwnd, WM_SETCONSOLEINFO, 
		(WPARAM)hDupSection, 0);

	CloseHandle(hSection);
	CloseHandle(hProcess);
}

/* One-time initialization for console_info -- color table and font 
   info from the registry; other values from functions. */

static void init_console_info(void)
{
	DWORD scrnmode, len;
	HKEY reghnd;
	int i;

	console_info.Length = sizeof(console_info);

	GetConsoleMode(hConIn, &scrnmode);
	console_info.QuickEdit = !!(scrnmode & 0x0040);
	console_info.InsertMode = !!(scrnmode & 0x0020);

	console_info.FullScreen = FALSE;
	console_info.AutoPosition = 0x10000;
	console_info.ScreenColors = MAKEWORD(SP->orig_fore, SP->orig_back);
	console_info.PopupColors = MAKEWORD(0x5, 0xf);
	
	console_info.HistoryNoDup = FALSE;
	console_info.HistoryBufferSize = 50;
	console_info.NumberOfHistoryBuffers = 4;

	console_info.CodePage = GetConsoleOutputCP();

	RegOpenKeyEx(HKEY_CURRENT_USER, TEXT("Console"), 0, 
		KEY_QUERY_VALUE, &reghnd);

	len = sizeof(DWORD);

	/* Default color table */

	for (i = 0; i < 16; i++)
	{
		char tname[13];

		sprintf(tname, "ColorTable%02d", i);
		RegQueryValueExA(reghnd, tname, NULL, NULL,
			(LPBYTE)(&(console_info.ColorTable[i])), &len);
	}

	/* Font info */

	RegQueryValueEx(reghnd, TEXT("FontSize"), NULL, NULL,
		(LPBYTE)(&console_info.FontSize), &len);
	RegQueryValueEx(reghnd, TEXT("FontFamily"), NULL, NULL,
		(LPBYTE)(&console_info.FontFamily), &len);
	RegQueryValueEx(reghnd, TEXT("FontWeight"), NULL, NULL,
		(LPBYTE)(&console_info.FontWeight), &len);

	len = sizeof(WCHAR) * 32;
	RegQueryValueExW(reghnd, L"FaceName", NULL, NULL,
		(LPBYTE)(console_info.FaceName), &len);

	RegCloseKey(reghnd);
}

/*man-start**************************************************************

  PDC_scr_close() - Internal low-level binding to close the physical screen

  PDCurses Description:
	May restore the screen to its state before PDC_scr_open();
	miscellaneous cleanup.

  PDCurses Return Value:
	This function returns OK on success, otherwise an ERR is returned.

  Portability:
	PDCurses  void PDC_scr_close(void);

**man-end****************************************************************/

void PDC_scr_close(void)
{
	COORD origin;
	SMALL_RECT rect;

	PDC_LOG(("PDC_scr_close() - called\n"));

	SetConsoleScreenBufferSize(hConOut, orig_scr.dwSize);
	SetConsoleWindowInfo(hConOut, TRUE, &orig_scr.srWindow);
	SetConsoleScreenBufferSize(hConOut, orig_scr.dwSize);
	SetConsoleWindowInfo(hConOut, TRUE, &orig_scr.srWindow);

	if (SP->_restore != PDC_RESTORE_NONE)
	{
		if (SP->_restore == PDC_RESTORE_WINDOW)
		{
			rect.Top = orig_scr.srWindow.Top;
			rect.Left = orig_scr.srWindow.Left;
			rect.Bottom = orig_scr.srWindow.Bottom;
			rect.Right = orig_scr.srWindow.Right;
		}
		else	/* PDC_RESTORE_BUFFER */
		{
			rect.Top = rect.Left = 0;
			rect.Bottom = orig_scr.dwSize.Y - 1;
			rect.Right = orig_scr.dwSize.X - 1;
		}

		origin.X = origin.Y = 0;

		if (!WriteConsoleOutput(hConOut, ciSaveBuffer, 
		    orig_scr.dwSize, origin, &rect))
			return;
	}

	SetConsoleActiveScreenBuffer(hConOut);
	SetConsoleMode(hConIn, dwConsoleMode);

	if (SP->visibility != 1)
		curs_set(1);

	/* Position cursor to the bottom left of the screen. */

	PDC_gotoyx(PDC_get_buffer_rows() - 2, 0);
}

void PDC_scr_exit(void)
{
	if (SP)
		free(SP);
	if (pdc_atrtab)
		free(pdc_atrtab);
}

/*man-start**************************************************************

  PDC_scr_open()  - Internal low-level binding to open the physical screen

  PDCurses Description:
	The platform-specific part of initscr() -- allocates SP, does
	miscellaneous intialization, and may save the existing screen
	for later restoration.

  PDCurses Return Value:
	This function returns OK on success, otherwise an ERR is returned.

  Portability:
	PDCurses  int PDC_scr_open(int argc, char **argv);

**man-end****************************************************************/

int PDC_scr_open(int argc, char **argv)
{
	COORD bufsize, origin;
	SMALL_RECT rect;
	const char *str;
	CONSOLE_SCREEN_BUFFER_INFO csbi;

	PDC_LOG(("PDC_scr_open() - called\n"));

	SP = calloc(1, sizeof(SCREEN));
	pdc_atrtab = calloc(MAX_ATRTAB, 1);

	if (!SP || !pdc_atrtab)
		return ERR;

	hConOut = GetStdHandle(STD_OUTPUT_HANDLE);
	hConIn = GetStdHandle(STD_INPUT_HANDLE);

	isNT = !(GetVersion() & 0x80000000);

	GetConsoleScreenBufferInfo(hConOut, &csbi);
	GetConsoleScreenBufferInfo(hConOut, &orig_scr);
	GetConsoleMode(hConIn, &dwConsoleMode);

	SP->lines = ((str = getenv("LINES")) != NULL) ?
		atoi(str) : PDC_get_rows();

	SP->cols = ((str = getenv("COLS")) != NULL) ?
		atoi(str) : PDC_get_columns();

	if (SP->lines < 2 || SP->lines > csbi.dwMaximumWindowSize.Y)
	{
		fprintf(stderr, "LINES value must be >= 2 and <= %d: got %d\n",
			csbi.dwMaximumWindowSize.Y, SP->lines);

		return ERR;
	}

	if (SP->cols < 2 || SP->cols > csbi.dwMaximumWindowSize.X)
	{
		fprintf(stderr, "COLS value must be >= 2 and <= %d: got %d\n",
			csbi.dwMaximumWindowSize.X, SP->cols);

		return ERR;
	}

	SP->orig_fore = csbi.wAttributes & 0x0f;
	SP->orig_back = (csbi.wAttributes & 0xf0) >> 4;

	SP->orig_attr = TRUE;

	SP->_restore = PDC_RESTORE_NONE;

	if (getenv("PDC_RESTORE_SCREEN") != NULL)
	{
		/* Attempt to save the complete console buffer */

		ciSaveBuffer = malloc(orig_scr.dwSize.X * 
		    orig_scr.dwSize.Y * sizeof(CHAR_INFO));

		if (!ciSaveBuffer)
		{
		    PDC_LOG(("PDC_scr_open() - malloc failure (1)\n"));

		    return ERR;
		}

		bufsize.X = orig_scr.dwSize.X;
		bufsize.Y = orig_scr.dwSize.Y;

		origin.X = origin.Y = 0;

		rect.Top = rect.Left = 0;
		rect.Bottom = orig_scr.dwSize.Y  - 1;
		rect.Right = orig_scr.dwSize.X - 1;

		if (!ReadConsoleOutput(hConOut, ciSaveBuffer, bufsize, 
		    origin, &rect))
		{
		    /* We can't save the complete buffer, so try and 
		       save just the displayed window */

		    free(ciSaveBuffer);
		    ciSaveBuffer = NULL;

		    bufsize.X = orig_scr.srWindow.Right - 
			orig_scr.srWindow.Left + 1;
		    bufsize.Y = orig_scr.srWindow.Bottom - 
			orig_scr.srWindow.Top + 1;

		    ciSaveBuffer = malloc(bufsize.X * bufsize.Y *
			sizeof(CHAR_INFO));

		    if (!ciSaveBuffer)
		    {
			PDC_LOG(("PDC_scr_open() - malloc failure (2)\n"));

			return ERR;
		    }

		    origin.X = origin.Y = 0;

		    rect.Top = orig_scr.srWindow.Top;
		    rect.Left = orig_scr.srWindow.Left;
		    rect.Bottom = orig_scr.srWindow.Bottom;
		    rect.Right = orig_scr.srWindow.Right;

		    if (!ReadConsoleOutput(hConOut, ciSaveBuffer, 
			bufsize, origin, &rect))
		    {
#ifdef PDCDEBUG
			CHAR LastError[256];

			FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, 
			    GetLastError(), MAKELANGID(LANG_NEUTRAL, 
			    SUBLANG_DEFAULT), LastError, 256, NULL);

			PDC_LOG(("PDC_scr_open() - %s\n", LastError));
#endif
			free(ciSaveBuffer);
			ciSaveBuffer = NULL;

			return ERR;
		    }

		    SP->_restore = PDC_RESTORE_WINDOW;
		}
		else
		    SP->_restore = PDC_RESTORE_BUFFER;
	}

	SP->_preserve = (getenv("PDC_PRESERVE_SCREEN") != NULL);

	bufsize.X = orig_scr.srWindow.Right - orig_scr.srWindow.Left + 1;
	bufsize.Y = orig_scr.srWindow.Bottom - orig_scr.srWindow.Top + 1;

	rect.Top = rect.Left = 0;
	rect.Bottom = bufsize.Y - 1;
	rect.Right = bufsize.X - 1;

	SetConsoleScreenBufferSize(hConOut, bufsize);
	SetConsoleWindowInfo(hConOut, TRUE, &rect);
	SetConsoleScreenBufferSize(hConOut, bufsize);
	SetConsoleActiveScreenBuffer(hConOut);

	PDC_reset_prog_mode();

	PDC_get_cursor_pos(&SP->cursrow, &SP->curscol);

	SP->mono = FALSE;

	SP->orig_cursor = PDC_get_cursor_mode();

	SP->orgcbr = PDC_get_ctrl_break();

	if (isNT)
	{
		console_info.Hwnd = FindConsoleHandle();
		init_console_info();
	}

	return OK;
}

 /* Calls SetConsoleWindowInfo with the given parameters, but fits them 
    if a scoll bar shrinks the maximum possible value. The rectangle 
    must at least fit in a half-sized window. */

static BOOL FitConsoleWindow(HANDLE hConOut, CONST SMALL_RECT *rect)
{
	SMALL_RECT run;
	SHORT mx, my;

	if (SetConsoleWindowInfo(hConOut, TRUE, rect))
		return TRUE;

	run = *rect;
	run.Right /= 2;
	run.Bottom /= 2;

	mx = run.Right;
	my = run.Bottom;

	if (!SetConsoleWindowInfo(hConOut, TRUE, &run))
		return FALSE;

	for (run.Right = rect->Right; run.Right >= mx; run.Right--)
		if (SetConsoleWindowInfo(hConOut, TRUE, &run))
			break;

	if (run.Right < mx)
		return FALSE;

	for (run.Bottom = rect->Bottom; run.Bottom >= my; run.Bottom--)
		if (SetConsoleWindowInfo(hConOut, TRUE, &run))
			return TRUE;

	return FALSE;
}

/*man-start**************************************************************

  PDC_resize_screen()   - Internal low-level function to resize screen

  PDCurses Description:
	This function provides a means for the application program to 
	resize the overall dimensions of the screen.  Under DOS and OS/2 
	the application can tell PDCurses what size to make the screen; 
	under X11, resizing is done by the user and this function simply 
	adjusts its internal structures to fit the new size. This 
	function doesn't set LINES, COLS, SP->lines or SP->cols; that
	must be done by resize_term. The functions fails if one of the 
	arguments is less then 2.

  PDCurses Return Value:
	This function returns OK on success, otherwise an ERR is 
	returned.

  Portability:
	PDCurses  int PDC_resize_screen(int, int);

**man-end****************************************************************/

int PDC_resize_screen(int nlines, int ncols)
{
	SMALL_RECT rect;
	COORD size, max;

	if (nlines < 2 || ncols < 2)
		return ERR;

	max = GetLargestConsoleWindowSize(hConOut);

	rect.Left = rect.Top = 0;
	rect.Right = ncols - 1;

	if (rect.Right > max.X)
		rect.Right = max.X;

	rect.Bottom = nlines - 1;

	if (rect.Bottom > max.Y)
		rect.Bottom = max.Y;

	size.X = rect.Right + 1;
	size.Y = rect.Bottom + 1;

	FitConsoleWindow(hConOut, &rect);
	SetConsoleScreenBufferSize(hConOut, size);
	FitConsoleWindow(hConOut, &rect);
	SetConsoleScreenBufferSize(hConOut, size);
	SetConsoleActiveScreenBuffer(hConOut);

	return OK;
}

void PDC_reset_prog_mode(void)
{
	PDC_LOG(("PDC_reset_prog_mode() - called.\n"));

	SetConsoleMode(hConIn, ENABLE_MOUSE_INPUT);
}

void PDC_reset_shell_mode(void)
{
	PDC_LOG(("PDC_reset_shell_mode() - called.\n"));

	SetConsoleMode(hConIn, dwConsoleMode);
}

void PDC_restore_screen_mode(int i)
{
}

void PDC_save_screen_mode(int i)
{
}

bool PDC_can_change_color(void)
{
	return isNT;
}

int PDC_color_content(short color, short *red, short *green, short *blue)
{
	DWORD col = console_info.ColorTable[color];

	*red = (double)GetRValue(col) * 1000 / 255 + 0.5;
	*green = (double)GetGValue(col) * 1000 / 255 + 0.5;
	*blue = (double)GetBValue(col) * 1000 / 255 + 0.5;

	return OK;
}

int PDC_init_color(short color, short red, short green, short blue)
{
	console_info.ColorTable[color] =
		RGB((double)red * 255 / 1000 + 0.5,
		    (double)green * 255 / 1000 + 0.5,
		    (double)blue * 255 / 1000 + 0.5);

	SetConsoleInfo();

	return OK;
}

#ifdef PDC_DLL_BUILD

BOOL WINAPI DllMain(HINSTANCE hDLL, DWORD dwReason, LPVOID pReserved)
{
	switch(dwReason)
	{
	case DLL_PROCESS_ATTACH:
/*		fprintf(stderr, "DLL_PROCESS_ATTACH\n"); */
		break;
	case DLL_PROCESS_DETACH:
/*		fprintf(stderr, "DLL_PROCESS_DETACH\n"); */
		break;
	case DLL_THREAD_ATTACH:
/*		fprintf(stderr, "DLL_THREAD_ATTACH\n"); */
		break;
	case DLL_THREAD_DETACH:
/*		fprintf(stderr, "DLL_THREAD_DETACH\n"); */
		;
	}
	return TRUE;
}

#endif
