#include <windows.h>
#include <windowsx.h>
#include <ddraw.h>
#include <stdio.h>
#include <string.h>
#include "vecx.h"
#include "e6809.h"

#define einline __inline

/* a global string buffer user for message output */

char gbuffer[1024];

static const char *appname = "vecx";
static const char *romname = "rom.dat";

static HWND                hwnd;
static LPDIRECTDRAW        lpdd;
static LPDIRECTDRAWSURFACE lpdd_primary = 0;
static DDSURFACEDESC       ddsd;
static POINT			   scr_offset = {-1, -1};

static long screen_x;
static long screen_y;
static long scl_factor;

static long bytes_per_pixel;
static DWORD color_set[VECTREX_COLORS];

enum {
	EMU_TIMER = 20 /* the emulators heart beats at 20 milliseconds */
};

static void osint_updatescale (void)
{
	long sclx, scly;

	sclx = ALG_MAX_X / screen_x;
	scly = ALG_MAX_Y / screen_y;

	if (sclx > scly) {
		scl_factor = sclx;
	} else {
		scl_factor = scly;
	}
}

static int osint_defaults (void)
{
	unsigned b;
	FILE *rom_file;

	screen_x = 330*2;
	screen_y = 410*2;

	osint_updatescale ();

	/* load up the rom */

	rom_file = fopen (romname, "rb");

	if (rom_file == NULL) {
		sprintf (gbuffer, "cannot open '%s'", romname);
		MessageBox (NULL, gbuffer, NULL, MB_OK);
		return 1;
	}

	b = fread (rom, 1, sizeof (rom), rom_file);

	if (b < sizeof (rom)) {
		sprintf (gbuffer, "read %d bytes from '%s'. need %d bytes.",
			b, romname, sizeof (rom));
		MessageBox (NULL, gbuffer, NULL, MB_OK);
		return 1;
	}

	fclose (rom_file);

	/* the cart is empty by default */

	for (b = 0; b < sizeof (cart); b++) {
		cart[b] = 0;
	}

	return 0;
}

static void osint_parse_cmdline (char *cmdline)
{
	int c = 0;
	char ch;

	while ((ch = cmdline[c]) != '\0') {
		if (ch == '-') {
			/* a flag coming up */

			ch = cmdline[++c];

			if (ch == 'c' || ch == 'C') {
				int cartname_len = 0;
				char cartname[256];

				/* skip any blank space */

				do {
					ch = cmdline[++c];
				} while (ch != '\0' && isspace (ch));

				while (ch != '\0' && isspace (ch) == 0) {
					cartname[cartname_len++] = ch;
					ch = cmdline[++c];
				}

				cartname[cartname_len] = '\0';

				if (cartname_len > 0) {
					FILE *cartfile;

					cartfile = fopen (cartname, "rb");
				
					if (cartfile != NULL) {
						fread (cart, 1, sizeof (cart), cartfile);
						fclose (cartfile);
					} else {
						sprintf (gbuffer, "cannot open '%s'", cartname);
						MessageBox (NULL, gbuffer, NULL, MB_OK);
					}
				}
			} else if (ch == 'x' || ch == 'X') {
				long sizex = 0;

				/* skip any blank space */

				do {
					ch = cmdline[++c];
				} while (ch != '\0' && isspace (ch));

				while (ch != '\0' && isdigit (ch)) {
					sizex *= 10;
					sizex += ch - '0';
					ch = cmdline[++c];
				}

				if (sizex > 10 && sizex < 4000) {
					screen_x = sizex;
					osint_updatescale ();
				}
			} else if (ch == 'y' || ch == 'Y') {
				long sizey = 0;

				/* skip any blank space */

				do {
					ch = cmdline[++c];
				} while (ch != '\0' && isspace (ch));

				while (ch != '\0' && isdigit (ch)) {
					sizey *= 10;
					sizey += ch - '0';
					ch = cmdline[++c];
				}

				if (sizey > 10 && sizey < 4000) {
					screen_y = sizey;
					osint_updatescale ();
				}
			}
		} else {
			/* skip past a character that we don't understand */

			c++;
		}
	}
}

static void osint_maskinfo (DWORD mask, int *shift, int *precision)
{
	*shift = 0;

	while ((mask & 1L) == 0) {
		mask >>= 1;
		(*shift)++;
	}

	*precision = 0;

	while ((mask & 1L) != 0) {
		mask >>= 1;
		(*precision)++;
	}
}

static void osint_gencolors (void)
{
	int c;
	int rcomp, gcomp, bcomp;
	int rsh, rpr;
	int gsh, gpr;
	int bsh, bpr;

	DDPIXELFORMAT ddpf;

	ddpf.dwSize = sizeof (ddpf);
	IDirectDrawSurface_GetPixelFormat (lpdd_primary, &ddpf);

	bytes_per_pixel = ddpf.dwRGBBitCount / 8;
	
	osint_maskinfo (ddpf.dwRBitMask, &rsh, &rpr);
	osint_maskinfo (ddpf.dwGBitMask, &gsh, &gpr);
	osint_maskinfo (ddpf.dwBBitMask, &bsh, &bpr);

	for (c = 0; c < VECTREX_COLORS; c++) {
		rcomp = c * 256 / VECTREX_COLORS;
		gcomp = c * 256 / VECTREX_COLORS;
		bcomp = c * 256 / VECTREX_COLORS;

		color_set[c] =	(((DWORD) rcomp >> (8 - rpr)) << rsh) |
						(((DWORD) gcomp >> (8 - gpr)) << gsh) |
						(((DWORD) bcomp >> (8 - bpr)) << bsh);
	}
}

static einline unsigned char *osint_pixelptr (long x, long y)
{
	unsigned char *ptr;

	ptr = (unsigned char *)ddsd.lpSurface;
	ptr += (scr_offset.y + y) * ddsd.lPitch;
	ptr += (scr_offset.x + x) * bytes_per_pixel;

	return ptr;
}

static einline void osint_clearscreen (void)
{
	long y;
	unsigned char *ptr;

	ptr = osint_pixelptr (0, 0);

	for (y = 0; y < screen_y; y++) {
		memset (ptr, 0, screen_x * bytes_per_pixel);
		ptr += ddsd.lPitch;
	}
}

/* draw a line with a slope between 0 and 1.
 * x is the "driving" axis. x0 < x1 and y0 < y1.
 */

static void osint_linep01 (long x0, long y0, long x1, long y1, unsigned char color)
{
	long dx, dy;
	long i0, i1;
	long j, e;
	unsigned char *ptr;

	dx = x1 - x0;
	dy = y1 - y0;

	i0 = x0 / scl_factor;
	i1 = x1 / scl_factor;
	j  = y0 / scl_factor;

	e = dy * (scl_factor - (x0 % scl_factor)) -
		dx * (scl_factor - (y0 % scl_factor));

	dx *= scl_factor;
	dy *= scl_factor;

	ptr = osint_pixelptr (i0, j);

	for (; i0 <= i1; i0++) {
		memcpy (ptr, color_set + color, bytes_per_pixel);

		if (e >= 0) {
			ptr += ddsd.lPitch;
			e -= dx;
		}

		e += dy;
		ptr += bytes_per_pixel;
	}
}

/* draw a line with a slope between 1 and +infinity.
 * y is the "driving" axis. y0 < y1 and x0 < x1.
 */

static void osint_linep1n (long x0, long y0, long x1, long y1, unsigned char color)
{
	long dx, dy;
	long i0, i1;
	long j, e;
	unsigned char *ptr;

	dx = x1 - x0;
	dy = y1 - y0;

	i0 = y0 / scl_factor;
	i1 = y1 / scl_factor;
	j  = x0 / scl_factor;

	e = dx * (scl_factor - (y0 % scl_factor)) -
		dy * (scl_factor - (x0 % scl_factor));

	dx *= scl_factor;
	dy *= scl_factor;

	ptr = osint_pixelptr (j, i0);

	for (; i0 <= i1; i0++) {
		memcpy (ptr, color_set + color, bytes_per_pixel);

		if (e >= 0) {
			ptr += bytes_per_pixel;
			e -= dy;
		}

		e += dx;
		ptr += ddsd.lPitch;
	}
}

/* draw a line with a slope between 0 and -1.
 * x is the "driving" axis. x0 < x1 and y1 < y0.
 */

static void osint_linen01 (long x0, long y0, long x1, long y1, unsigned char color)
{
	long dx, dy;
	long i0, i1;
	long j, e;
	unsigned char *ptr;

	dx = x1 - x0;
	dy = y0 - y1;

	i0 = x0 / scl_factor;
	i1 = x1 / scl_factor;
	j  = y0 / scl_factor;

	e = dy * (scl_factor - (x0 % scl_factor)) -
		dx * (y0 % scl_factor);

	dx *= scl_factor;
	dy *= scl_factor;

	ptr = osint_pixelptr (i0, j);

	for (; i0 <= i1; i0++) {
		memcpy (ptr, color_set + color, bytes_per_pixel);

		if (e >= 0) {
			ptr -= ddsd.lPitch;
			e -= dx;
		}

		e += dy;
		ptr += bytes_per_pixel;
	}
}

/* draw a line with a slope between -1 and -infinity.
 * y is the "driving" axis. y0 < y1 and x1 < x0.
 */

static void osint_linen1n (long x0, long y0, long x1, long y1, unsigned char color)
{
	long dx, dy;
	long i0, i1;
	long j, e;
	unsigned char *ptr;

	dx = x0 - x1;
	dy = y1 - y0;

	i0 = y0 / scl_factor;
	i1 = y1 / scl_factor;
	j  = x0 / scl_factor;

	e = dx * (scl_factor - (y0 % scl_factor)) -
		dy * (x0 % scl_factor);

	dx *= scl_factor;
	dy *= scl_factor;

	ptr = osint_pixelptr (j, i0);

	for (; i0 <= i1; i0++) {
		memcpy (ptr, color_set + color, bytes_per_pixel);

		if (e >= 0) {
			ptr -= bytes_per_pixel;
			e -= dy;
		}

		e += dx;
		ptr += ddsd.lPitch;
	}
}

static void osint_line (long x0, long y0, long x1, long y1, unsigned char color)
{
	if (x1 > x0) {
		if (y1 > y0) {
			if ((x1 - x0) > (y1 - y0)) {
				osint_linep01 (x0, y0, x1, y1, color);
			} else {
				osint_linep1n (x0, y0, x1, y1, color);
			}
		} else {
			if ((x1 - x0) > (y0 - y1)) {
				osint_linen01 (x0, y0, x1, y1, color);
			} else {
				osint_linen1n (x1, y1, x0, y0, color);
			}
		}
	} else {
		if (y1 > y0) {
			if ((x0 - x1) > (y1 - y0)) {
				osint_linen01 (x1, y1, x0, y0, color);
			} else {
				osint_linen1n (x0, y0, x1, y1, color);
			}
		} else {
			if ((x0 - x1) > (y0 - y1)) {
				osint_linep01 (x1, y1, x0, y0, color);
			} else {
				osint_linep1n (x1, y1, x0, y0, color);
			}
		}
	}
}

void osint_render (void)
{
	long v;
	POINT prev_scr_offset;

	IDirectDrawSurface_Lock (lpdd_primary, NULL, &ddsd, DDLOCK_WAIT, NULL);

	prev_scr_offset = scr_offset;
	scr_offset.x = 0;
	scr_offset.y = 0;
	ClientToScreen (hwnd, &scr_offset);

	if (scr_offset.x != prev_scr_offset.x ||
		scr_offset.y != prev_scr_offset.y) {
		osint_clearscreen ();
	} else {
		for (v = 0; v < vector_erse_cnt; v++) {
			if (vectors_erse[v].color != VECTREX_COLORS) {
				osint_line (vectors_erse[v].x0, vectors_erse[v].y0,
							vectors_erse[v].x1, vectors_erse[v].y1, 0);
			}
		}
	}

	for (v = 0; v < vector_draw_cnt; v++) {
		osint_line (vectors_draw[v].x0, vectors_draw[v].y0,
					vectors_draw[v].x1, vectors_draw[v].y1,
					vectors_draw[v].color);
	}

	IDirectDrawSurface_Unlock (lpdd_primary, NULL);
}

void osint_emuloop (void)
{
	MSG Msg;
	unsigned timeout; /* timing flag */

	/* reset the vectrex hardware */

	vecx_reset ();

	/* startup the emulator's heart beat */

	SetTimer (hwnd, 1, EMU_TIMER, NULL);

	while (1) {
		timeout = 0;

		vecx_emu ((VECTREX_MHZ / 1000) * EMU_TIMER, 0);

		while (PeekMessage (&Msg, NULL, 0, 0, PM_REMOVE)) {
			switch (Msg.message) {
			case WM_QUIT:
				/* loop done */

				goto exit_emuloop;
			case WM_TIMER:
				timeout = 1;
				break;
			default:
				TranslateMessage (&Msg);
				DispatchMessage (&Msg);
			}
		}

		if (timeout == 0) {
			/* the timeout hasn't been received yet ... just hang here
			 * until EMU_TIMER milliseconds have elapsed.
			 */

			if (GetMessage (&Msg, NULL, WM_TIMER, WM_TIMER) == 0) {
				/* WM_QUIT received */

				goto exit_emuloop;
			}
		}
	}

exit_emuloop:

	KillTimer (hwnd, 1);
}

LRESULT CALLBACK WindowProc (HWND hwnd, UINT uMsg, WPARAM wParam,
							 LPARAM lParam)
{
	switch (uMsg) {
	case WM_SETCURSOR:
		/* don't display a cursor */

		SetCursor (NULL);
		return TRUE;
	case WM_KEYDOWN:
		/* terminate application if ESC was hit */

		switch (wParam) {  //Ville: Use same keys as in MESS
		case VK_ESCAPE:
			PostMessage (hwnd, WM_CLOSE, 0, 0);
			break;
    case 'A': case VK_CONTROL :
			snd_regs[14] &= ~0x01;
			break;
    case 'S': case 'C' :
			snd_regs[14] &= ~0x02;
			break;
    case 'D': case VK_SPACE :
			snd_regs[14] &= ~0x04;
			break;
		case 'F': case VK_SHIFT :
			snd_regs[14] &= ~0x08;
			break;
		case VK_LEFT:
			alg_jch0 = 0x00;
			break;
		case VK_RIGHT:
			alg_jch0 = 0xff;
			break;
		case VK_UP:
			alg_jch1 = 0xff;
			break;
		case VK_DOWN:
			alg_jch1 = 0x00;
			break;
		}

		break;
	case WM_KEYUP:
		switch (wParam) {
    case 'A': case VK_CONTROL :
			snd_regs[14] |= 0x01;
			break;
    case 'S': case 'C' :
			snd_regs[14] |= 0x02;
			break;
    case 'D': case VK_SPACE :
			snd_regs[14] |= 0x04;
			break;
		case 'F': case VK_SHIFT :
			snd_regs[14] |= 0x08;
			break;
    case 'R':  //Ville: R for reset
      e6809_reset();
      break;
    case VK_LEFT:
			alg_jch0 = 0x80;
			break;
		case VK_RIGHT:
			alg_jch0 = 0x80;
			break;
		case VK_UP:
			alg_jch1 = 0x80;
			break;
		case VK_DOWN:
			alg_jch1 = 0x80;
			break;
		}

		break;
	case WM_PAINT:
		/* some other program obscured the display... just draw a blank window */

		if (lpdd_primary) {
			IDirectDrawSurface_Lock (lpdd_primary, NULL, &ddsd, DDLOCK_WAIT, NULL);

			scr_offset.x = 0;
			scr_offset.y = 0;
			ClientToScreen (hwnd, &scr_offset);

			osint_clearscreen ();

			IDirectDrawSurface_Unlock (lpdd_primary, NULL);
		}

		break;
	case WM_DESTROY:
		/* termination, free up objects */

		IDirectDrawSurface_Release (lpdd_primary);
		IDirectDraw_Release (lpdd);

		lpdd_primary = 0; /* to prevent the emulator from drawing */

		PostQuitMessage (0);
		break;
	}

	return DefWindowProc (hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                   LPSTR lpCmdLine, int nCmdShow) {
	WNDCLASS            wc;
	RECT                rect;

	if (osint_defaults ()) {
		return 1;
	}

	osint_parse_cmdline (lpCmdLine);

	/* set up and register a window class */

	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WindowProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hInstance;
	wc.hIcon = LoadIcon (hInstance, IDI_APPLICATION);
	wc.hCursor = LoadCursor (NULL, IDC_ARROW);
	wc.hbrBackground = NULL;
	wc.lpszMenuName = NULL;
	wc.lpszClassName = appname;
	RegisterClass (&wc);
    
	/* create a window */

	SetRect (&rect, 0, 0, screen_x, screen_y);

	AdjustWindowRectEx (&rect, WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX, FALSE,
						WS_EX_CLIENTEDGE);

	hwnd = CreateWindowEx (WS_EX_CLIENTEDGE, appname, appname,
						   WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX, 50, 100,
						   rect.right - rect.left, rect.bottom - rect.top,
						   NULL, NULL, hInstance, NULL);

	if (!hwnd) {
		return FALSE;
	}

	ShowWindow (hwnd, nCmdShow);
	UpdateWindow (hwnd);

	/* create the main DirectDraw object */

  DirectDrawCreate ( NULL, &lpdd, NULL);

	/* get exclusive mode */
  
	IDirectDraw_SetCooperativeLevel (lpdd, hwnd, DDSCL_NORMAL);

	/* create the primary surface */

	ddsd.dwSize = sizeof (ddsd);
	ddsd.dwFlags = DDSD_CAPS;
	ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
	IDirectDraw_CreateSurface (lpdd, &ddsd, &lpdd_primary, NULL);

	/* determine a set of colors to use based */

	osint_gencolors ();

	/* message loop handler and emulator code */

	osint_emuloop ();

  dumpRecord();

	return 0;
}
