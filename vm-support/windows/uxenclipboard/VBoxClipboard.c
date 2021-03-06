/** @file
 *
 * VBoxClipboard - Shared clipboard
 *
 */

/*
 * Copyright (C) 2006-2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#include "VBoxTray.h"

#include "VBoxClipboardSvc.h"
#include "channel.h"
#include "clipboardformats.h"
#include "uxen_bmp_convert.h"
#include "proto.h"
#include "../common/debug-user.h"

typedef struct _VBOXCLIPBOARDCONTEXT
{
    const VBOXSERVICEENV *pEnv;

    uint32_t u32ClientID;

    ATOM     atomWindowClass;

    HWND     hwnd;

    HWND     hwndNextInChain;

    UINT     timerRefresh;

    bool     fCBChainPingInProcess;

    HANDLE openClipboardMutex;
    HANDLE closeDoneEvent;
    HANDLE inGetDataEvent;

//    bool     fOperational;

//    uint32_t u32LastSentFormat;
//    uint64_t u64LastSentCRC64;

} VBOXCLIPBOARDCONTEXT;

static char gachWindowClassName[] = "VBoxSharedClipboardClass";

enum { CBCHAIN_TIMEOUT = 5000 /* ms */ };

#define MAX_OPENCLIPBOARD_RETRIES 10
BOOL OpenClipboardWithRetry(HWND hwnd)
{
    int count = 0;
    while (!OpenClipboard(hwnd) && count < MAX_OPENCLIPBOARD_RETRIES) {
        /* Add PID so that other processes sleep different */
        Sleep((GetCurrentProcessId() & 0x7f) + 100);
        count++;
    }
    if (count < MAX_OPENCLIPBOARD_RETRIES)
        return TRUE;
    else {
        HWND owner = GetClipboardOwner();
        HWND root = GetAncestor(owner, GA_ROOT);
        char clsname[256] = { 0 };
        char clsname_root[256] = { 0 };

        GetClassNameA(owner, clsname, sizeof(clsname));
        GetClassNameA(root, clsname_root, sizeof(clsname_root));
        uxen_err("OpenClipboardWithRetry count %d, owner class %s, owner root class %s\n",
          count, clsname, clsname_root);

        return FALSE;
    }
}

#ifdef USE_OBSOLETE_FORMATS_CHANGE_MESSAGE
#ifdef ORIG_VBOX_CODE
static int vboxClipboardChanged(VBOXCLIPBOARDCONTEXT *pCtx)
{
    AssertPtr(pCtx);

    /* Query list of available formats and report to host. */
    int rc = VINF_SUCCESS;
    if (FALSE == OpenClipboardWithRetry(pCtx->hwnd))
    {
        rc = RTErrConvertFromWin32(GetLastError());
    }
    else
    {
        uint32_t u32Formats = 0;
        UINT format = 0;

        while ((format = EnumClipboardFormats (format)) != 0)
        {
            uxen_msg("BrHVTray: brhvClipboardChanged: format = 0x%08X\n", format);
            switch (format)
            {
                case CF_UNICODETEXT:
                case CF_TEXT:
                    u32Formats |= VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT;
                    break;

                case CF_DIB:
                case CF_BITMAP:
                    u32Formats |= VBOX_SHARED_CLIPBOARD_FMT_BITMAP;
                    break;

                default:
                    if (format >= 0xC000)
                    {
                        TCHAR szFormatName[256];

                        int cActual = GetClipboardFormatName(format, szFormatName, sizeof(szFormatName)/sizeof (TCHAR));
                        if (cActual)
                        {
                            if (strcmp (szFormatName, "HTML Format") == 0)
                            {
                                u32Formats |= VBOX_SHARED_CLIPBOARD_FMT_HTML;
                            }
                        }
                    }
                    break;
            }
        }

        CloseClipboard ();
        rc = VbglR3ClipboardReportFormats(pCtx->u32ClientID, u32Formats);
    }
    return rc;
}
#else /* ORIG_VBOX_CODE */
/*
Getting exclusive access to clipboard via OpenClipBoard
in WM_DRAWCLIPBOARD handler is a bad idea, it may starve other processes.
Particularly, Acrobat Reader is
broken - it does OpenClipBoard per each format announce, and it panics
upon first failed OpenClipBoard (instead retry).
Use IsClipboardFormatAvailable instead, that does not require OpenClipBoard.
*/
static int vboxClipboardChanged(VBOXCLIPBOARDCONTEXT *pCtx)
{
    uint32_t u32Formats = 0;
    int count;

    AssertPtr(pCtx);
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) ||
        IsClipboardFormatAvailable(CF_TEXT))
        u32Formats |= VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT;
    if (IsClipboardFormatAvailable(CF_DIB) ||
        IsClipboardFormatAvailable(CF_BITMAP))
        u32Formats |= VBOX_SHARED_CLIPBOARD_FMT_BITMAP;
    if (!u32Formats && (count = CountClipboardFormats()) > 0) {
        u32Formats = VBOX_SHARED_CLIPBOARD_FMT_UNSUPPORTED;
        uxen_msg("BrHVTray: CountClipboardFormats = %d\n", count);
    }
    return VbglR3ClipboardReportFormats(pCtx->u32ClientID, u32Formats);
}
#endif /* ORIG_VBOX_CODE */
#else /* USE_OBSOLETE_FORMATS_CHANGE_MESSAGE */
static int vboxClipboardChanged(VBOXCLIPBOARDCONTEXT *pCtx)
{
    char buf[16384];
    int len = uxenclipboard_prepare_format_announce(buf, sizeof(buf));
    if (len < 0) {
        uxen_err("BrHVTray:prepare_format_announce %d\n", len);
        return VERR_NO_MEMORY;
    }
    uxen_msg("BrHVTray:prepare_format_announce %d\n", len);
    return VbglR3ClipboardReportFormatsV2(pCtx->u32ClientID, buf, len);
}
#endif /* USE_OBSOLETE_FORMATS_CHANGE_MESSAGE */

static int send_one_format(VBOXCLIPBOARDCONTEXT *pCtx, uint32_t u32Format)
{
    int ret;
    char* data;
    unsigned int data_size;
    int vboxrc, err;

    ret = uxenclipboard_getdata(u32Format, &data, &data_size);

    if (ret) {
        int available;
        err = GetLastError();
        CloseClipboard();
        SetEvent(pCtx->closeDoneEvent);
        uxen_err("send_one_format CloseClipboard done\n");
        available = IsClipboardFormatAvailable(u32Format);
        uxen_err("send_one_format: GetClipboardData failed with 0x%x for fmt 0x%x lasterror 0x%x IsClipboardFormatAvailable 0x%x\n", ret, u32Format, err, available);
        return ret;
    }

    CloseClipboard();
    SetEvent(pCtx->closeDoneEvent);
    uxen_msg("send_one_format CloseClipboard done\n");
    uxen_msg("BrHVTray: brhvClipboardProcessMsg: WM_USER + 1: 0x%x\n", u32Format);
    vboxrc = VbglR3ClipboardWriteData(pCtx->u32ClientID, u32Format,
        data, data_size);
    if (vboxrc)
        uxen_err("BrHVTray: VbglR3ClipboardWriteData failed with rc=0x%x\n", vboxrc);
    free(data);
    return ret;
}

static bool get_openclipboard_mutex(VBOXCLIPBOARDCONTEXT *pCtx)
{
    DWORD err = WaitForSingleObject(pCtx->openClipboardMutex, 10 * 1000);
    if (err != WAIT_OBJECT_0) {
        uxen_err("WaitForSingleObject(pCtx->mutex ret 0x%x\n", err);
        return false;
    } else
        return true;
}

static void release_openclipboard_mutex(VBOXCLIPBOARDCONTEXT *pCtx)
{
    BOOL ret;
    
    ret = ReleaseMutex(pCtx->openClipboardMutex);

    if (!ret) {
        DWORD err = GetLastError();
        uxen_err("ReleaseMutex(pCtx->mutex) error 0x%x\n", err);
    }
}

static wchar_t no_formats_warning[] = L"No allowed clipboard formats could have been pasted.";
void insert_warning(HWND hwnd)
{
    char * locked;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(no_formats_warning));
    if (!hMem)
        return;
    locked = GlobalLock(hMem);
    if (!locked)
        return;
    memcpy(locked, no_formats_warning, sizeof(no_formats_warning));
    GlobalUnlock(hMem);
    SetClipboardData(CF_UNICODETEXT, hMem);
}

#ifndef WM_CLIPBOARDUPDATE
#define WM_CLIPBOARDUPDATE 0x031D
#endif

static LRESULT vboxClipboardProcessMsg(VBOXCLIPBOARDCONTEXT *pCtx, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    LRESULT rc = 0;
    switch (msg)
    {
        case WM_CLIPBOARDUPDATE:
        {
            uxen_msg("BrHVTray: WM_CLIPBOARDUPDATE, hwnd %p\n", pCtx->hwnd);

            if (GetClipboardOwner () != hwnd)
            {
                /* Clipboard was updated by another application. */
                /* WM_CLIPBOARDUPDATE always expects a return code of 0, so don't change "rc" here. */
                int vboxrc = vboxClipboardChanged(pCtx);
                if (RT_FAILURE(vboxrc))
                    uxen_err("BrHVTray: brhvClipboardProcessMsg: brhvClipboardChanged failed, rc = %Rrc\n", vboxrc);
            }

        } break;

        case WM_CLOSE:
        {
            /* Do nothing. Ignore the message. */
        } break;

        case WM_RENDERFORMAT:
        {
            /* Insert the requested clipboard format data into the clipboard. */
            UINT format = (UINT)wParam;
            uint32_t u32Format = uxenclipboard_translate_announced_format(format);
            if (!u32Format) {
                uxen_err("BrHVTray: translate_announced_format is zero\n");
                break;
            }

            uxen_msg("BrHVTray: WM_RENDERFORMAT, format = %x u32format 0x%x\n", format, u32Format);

            if (u32Format == 0)
            {
                /* Unsupported clipboard format is requested. */
                uxen_err("BrHVTray: brhvClipboardProcessMsg: Unsupported clipboard format requested: %ld\n", u32Format);
                EmptyClipboard();
            }
            else
            {
                /* Make the cbPrealloc be 16MB. Does not impact phys mem usage,
                   helps avoid message retransmit that triggers clipboard
                   security warning. */
                const uint32_t cbPrealloc = 16 * 1024 * 1024; /* @todo r=andy Make it dynamic for supporting larger text buffers! */
                uint32_t cb = 0;

                /* Preallocate a buffer, most of small text transfers will fit into it. */
                HANDLE hMem = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, cbPrealloc);
                uxen_debug("BrHVTray: brhvClipboardProcessMsg: Preallocated handle hMem = %p\n", hMem);

                if (hMem)
                {
                    void *pMem = GlobalLock(hMem);
                    uxen_debug("BrHVTray: brhvClipboardProcessMsg: Locked pMem = %p, GlobalSize = %ld\n", pMem, GlobalSize(hMem));

                    if (pMem)
                    {
                        /* Read the host data to the preallocated buffer. */
                        int vboxrc = VbglR3ClipboardReadData(pCtx->u32ClientID, u32Format, pMem, cbPrealloc, &cb);
                        uxen_debug("BrHVTray: brhvClipboardProcessMsg: VbglR3ClipboardReadData returned with rc = %Rrc\n",  vboxrc);

                        if (RT_SUCCESS(vboxrc))
                        {
                            if (cb == 0)
                            {
                                /* 0 bytes returned means the clipboard is empty.
                                 * Deallocate the memory and set hMem to NULL to get to
                                 * the clipboard empty code path. */
                                GlobalUnlock(hMem);
                                GlobalFree(hMem);
                                hMem = NULL;
                            }
                            else if (cb > cbPrealloc)
                            {
                                GlobalUnlock(hMem);

                                /* The preallocated buffer is too small, adjust the size. */
                                hMem = GlobalReAlloc(hMem, cb, 0);
                                uxen_debug("BrHVTray: brhvClipboardProcessMsg: Reallocated hMem = %p\n", hMem);

                                if (hMem)
                                {
                                    pMem = GlobalLock(hMem);
                                    uxen_debug("BrHVTray: brhvClipboardProcessMsg: Locked pMem = %p, GlobalSize = %ld\n", pMem, GlobalSize(hMem));

                                    if (pMem)
                                    {
                                        /* Read the host data to the preallocated buffer. */
                                        uint32_t cbNew = 0;
                                        vboxrc = VbglR3ClipboardReadData(pCtx->u32ClientID, u32Format, pMem, cb, &cbNew);
                                        uxen_debug("BrHVTray: VbglR3ClipboardReadData returned with rc = %Rrc, cb = %d, cbNew = %d\n", vboxrc, cb, cbNew);

                                        if (RT_SUCCESS (vboxrc) && cbNew <= cb)
                                        {
                                            cb = cbNew;
                                        }
                                        else
                                        {
                                            GlobalUnlock(hMem);
                                            GlobalFree(hMem);
                                            hMem = NULL;
                                        }
                                    }
                                    else
                                    {
                                        GlobalFree(hMem);
                                        hMem = NULL;
                                    }
                                }
                            }

                            if (hMem)
                            {
                                /* pMem is the address of the data. cb is the size of returned data. */
                                /* Verify the size of returned text, the memory block for clipboard
                                 * must have the exact string size.
                                 */
                                if (0 && u32Format == VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT)
                                {
                                    size_t cbActual = 2 * wcslen((LPWSTR)pMem);
#if 0
                                    HRESULT hrc = StringCbLengthW((LPWSTR)pMem, cb, &cbActual);
                                    if (FAILED (hrc))
#endif
                                    if (0)
                                    {
                                        /* Discard invalid data. */
                                        GlobalUnlock(hMem);
                                        GlobalFree(hMem);
                                        hMem = NULL;
                                    }
                                    else
                                    {
                                        /* cbActual is the number of bytes, excluding those used
                                         * for the terminating null character.
                                         */
                                        cb = (uint32_t)(cbActual + 2);
                                    }
                                }
                            }

                            if (hMem)
                            {
                                GlobalUnlock(hMem);

                                hMem = GlobalReAlloc(hMem, cb, 0);
                                uxen_debug("BrHVTray: brhvClipboardProcessMsg: Reallocated hMem = %p\n", hMem);

                                if (hMem)
                                {
                                    /* 'hMem' contains the host clipboard data.
                                     * size is 'cb' and format is 'format'. */
                                    HANDLE hClip = SetClipboardData(format, hMem);
                                    uxen_debug("BrHVTray: brhvClipboardProcessMsg: WM_RENDERFORMAT hClip = %p\n", hClip);

                                    if (hClip)
                                    {
                                        /* The hMem ownership has gone to the system. Finish the processing. */
                                        break;
                                    }

                                    /* Cleanup follows. */
                                }
                            }
                        }
                        if (hMem)
                            GlobalUnlock(hMem);
                    }
                    if (hMem)
                        GlobalFree(hMem);
                }

                /* Something went wrong. */
                EmptyClipboard();
            }
        } break;

        case WM_RENDERALLFORMATS:
        {
            /* Do nothing. The clipboard formats will be unavailable now, because the
             * windows is to be destroyed and therefore the guest side becomes inactive.
             */
            if (OpenClipboardWithRetry(hwnd))
            {
                EmptyClipboard();
                CloseClipboard();
            }
        } break;

        case WM_USER:
        {
            /* Announce available formats. Do not insert data, they will be inserted in WM_RENDER*. */
            if (FALSE == OpenClipboardWithRetry(hwnd))
            {
                uxen_err("BrHVTray: brhvClipboardProcessMsg: WM_USER: Failed to open clipboard! Last error = %ld\n", GetLastError());
            }
            else
            {
                int i = 0;
                int ok = 0;
                EmptyClipboard();
                unsigned int local, remote;

                while (!uxenclipboard_get_announced_format(i, &local, &remote)) {
                    uxen_debug("BrHVTray: got format 0x%d\n", local);
                    SetClipboardData(local, NULL);
                    ok = 1;
                    i++;
                }
                if (!ok)
                    insert_warning(hwnd);
                CloseClipboard();
                uxen_debug("WM_USER CloseClipboard done\n");
            }
        } break;

        case WM_USER + 1:
        {
            /* Send data in the specified format to the host. */
            uint32_t u32Format = (uint32_t)lParam;
            int ret = -1;

            uxen_debug("vboxClipboardProcessMsg WM_USER in format=0x%x\n", 
                u32Format);
            if (!get_openclipboard_mutex(pCtx))
                break;
            if (FALSE == OpenClipboardWithRetry(hwnd)) {
                uxen_err("BrHVTray: brhvClipboardProcessMsg: WM_USER: Failed to open clipboard! Last error = %ld\n", GetLastError());
                release_openclipboard_mutex(pCtx);
            } else {
                ResetEvent(pCtx->closeDoneEvent);
                SetEvent(pCtx->inGetDataEvent);
                release_openclipboard_mutex(pCtx);
                ret = send_one_format(pCtx, u32Format);
                if (get_openclipboard_mutex(pCtx)) {
                    ResetEvent(pCtx->inGetDataEvent);
                    release_openclipboard_mutex(pCtx);
                }
            }

            if (ret)
            {
                /* Requested clipboard format is not available, send empty data. */
                uxen_err("empty data for fmt 0x%x\n", u32Format);
                VbglR3ClipboardWriteData(pCtx->u32ClientID, 0, NULL, 0);
            }
        } break;

        default:
        {
            rc = DefWindowProc(hwnd, msg, wParam, lParam);
        }
    }

    uxen_debug("BrHVTray: returned with rc = %ld\n", rc);

    return rc;
}

static LRESULT CALLBACK vboxClipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static int createIPCEvents(VBOXCLIPBOARDCONTEXT *pCtx)
{
    pCtx->openClipboardMutex = CreateMutexW(NULL, FALSE,
        L"uxenclipboard_mutex");
    pCtx->closeDoneEvent = CreateEventW(NULL, FALSE /* auto reset */,
        FALSE /*init state*/, L"uxenclipboard_closedone");
    pCtx->inGetDataEvent = CreateEventW(NULL, TRUE /* manual reset */,
        FALSE /*init state*/, L"uxenclipboard_ingetdata");
    if (!pCtx->openClipboardMutex || !pCtx->closeDoneEvent ||
        !pCtx->inGetDataEvent) {
        uxen_err("uxenclipboard: cannot create IPC events!\n");
        if (pCtx->openClipboardMutex)
            CloseHandle(pCtx->openClipboardMutex);
        if (pCtx->closeDoneEvent)
            CloseHandle(pCtx->closeDoneEvent);
        if (pCtx->inGetDataEvent)
            CloseHandle(pCtx->inGetDataEvent);
        return VERR_NOT_SUPPORTED;
    } else
        return VINF_SUCCESS;
}

static int vboxClipboardInit (VBOXCLIPBOARDCONTEXT *pCtx)
{
    /* Register the Window Class. */
    WNDCLASS wc;

    wc.style         = CS_NOCLOSE;
    wc.lpfnWndProc   = vboxClipboardWndProc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0;
    wc.hInstance     = pCtx->pEnv->hInstance;
    wc.hIcon         = NULL;
    wc.hCursor       = NULL;
    wc.hbrBackground = (HBRUSH)(COLOR_BACKGROUND + 1);
    wc.lpszMenuName  = NULL;
    wc.lpszClassName = gachWindowClassName;

    pCtx->atomWindowClass = RegisterClass (&wc);

    int rc = VINF_SUCCESS;
    if (pCtx->atomWindowClass == 0)
    {
        rc = VERR_NOT_SUPPORTED;
    }
    else
    {
        /* Create the window. */
        pCtx->hwnd = CreateWindowEx (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                                     gachWindowClassName, gachWindowClassName,
                                     WS_POPUPWINDOW,
                                     -200, -200, 100, 100, NULL, NULL, pCtx->pEnv->hInstance, NULL);
        uxen_debug("CreateWindowEx ret %p, pCtx=%p\n", pCtx->hwnd,
            pCtx);
        if (pCtx->hwnd == NULL)
        {
            rc = VERR_NOT_SUPPORTED;
        }
        else
        {
            SetWindowPos(pCtx->hwnd, HWND_TOPMOST, -200, -200, 0, 0,
                         SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSIZE);

            if (!mingw_AddClipboardFormatListener(pCtx->hwnd)) {
                int err = GetLastError();
                uxen_err("AddClipboardFormatListener error %d\n", err);
                rc = VERR_NOT_SUPPORTED;
            } else
                rc = createIPCEvents(pCtx);
        }
    }

    if (rc)
        uxen_err("BrHVTray: brhvClipboardInit returned with rc = %Rrc\n", rc);
    return rc;
}

static void vboxClipboardDestroy(VBOXCLIPBOARDCONTEXT *pCtx)
{
    if (pCtx->hwnd)
    {
        DestroyWindow (pCtx->hwnd);
        pCtx->hwnd = NULL;
    }

    if (pCtx->atomWindowClass != 0)
    {
        UnregisterClass(gachWindowClassName, pCtx->pEnv->hInstance);
        pCtx->atomWindowClass = 0;
    }
}

/* Static since it is the single instance. Directly used in the windows proc. */
static VBOXCLIPBOARDCONTEXT gCtx = { NULL };

static LRESULT CALLBACK vboxClipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* Forward with proper context. */
    return vboxClipboardProcessMsg(&gCtx, hwnd, msg, wParam, lParam);
}

int VBoxClipboardInit(const VBOXSERVICEENV *pEnv, void **ppInstance, bool *pfStartThread)
{
    uxen_msg("BrHVTray: ClipboardInit\n");
    if (gCtx.pEnv)
    {
        /* Clipboard was already initialized. 2 or more instances are not supported. */
        return VERR_NOT_SUPPORTED;
    }

    RT_ZERO (gCtx);
    gCtx.pEnv = pEnv;

    uxenclipboard_init_formats_critical_section();

    int rc = VbglR3ClipboardConnect(&gCtx.u32ClientID);
    if (RT_SUCCESS (rc))
    {
        rc = vboxClipboardInit(&gCtx);
        if (RT_SUCCESS (rc))
        {
            /* Always start the thread for host messages. */
            *pfStartThread = true;
        }
        else
        {
            VbglR3ClipboardDisconnect(gCtx.u32ClientID);
        }
    }

    if (RT_SUCCESS(rc))
        *ppInstance = &gCtx;
    return rc;
}

static DWORD WINAPI VBoxClipboardThread(void *pInstance);
VBOXSERVICEENV vboxEnv;
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    bool startThread = false;
    void * instance;
    int ret = -1;
    int attempts = 30;

    uxen_ud_set_progname("uxenclipboard");
    uxen_ud_mask = UXEN_UD_ERR | UXEN_UD_MSG;

    uxenclipboard_gdi_startup();
    while (attempts-- > 0) {
        ret = ChannelConnect();
        if (!ret)
            break;
        Sleep(1000);
    }
    if (ret) {
        uxen_err("BrHVTray: ChannelConnect error 0x%x\n", ret);
        goto out;
    }
    vboxEnv.hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    vboxEnv.hInstance = hInstance;
    if (VBoxClipboardInit(&vboxEnv, &instance, &startThread))
        goto out;
    if (!CreateThread(NULL, 0x10000, VBoxClipboardThread, instance, 0, NULL)) {
        uxen_err("BrHVTray: ClipboardThread create failed\n");
        ret = 0;
        goto out;
    }
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    ret = 0;
out:
    ChannelClose();
    uxenclipboard_gdi_shutdown();
    return ret;
}

static DWORD WINAPI
VBoxClipboardThread(void *pInstance)
{
    uxen_debug("BrHVTray: BrHVClipboardThread\n");
    int ret;

    VBOXCLIPBOARDCONTEXT *pCtx = (VBOXCLIPBOARDCONTEXT *)pInstance;
    AssertPtr(pCtx);
    uxen_debug("VBoxClipboardThread, pCtx=%p, hwnd=%p\n", pCtx, pCtx->hwnd);
    // notify guest connected
    ChannelSendNotify("X", 1);
    /* The thread waits for incoming messages from the host. */
    for (;;) {
        struct clip_notify_data *ndata;
        void *msg = NULL;
        int msglen;
        int rc = ChannelRecvNotify(&msg, &msglen);

        if (rc) {
            uxen_err("BrHVTray: notification receive error %d\n", rc);
       } else {
            ndata = (struct clip_notify_data*) msg;
            uxen_debug("BrHVTray: BrHVClipboardThread: VbglR3ClipboardGetHostMsg type %d len %d\n",
                 ndata->type, ndata->len);
            switch (ndata->type) {
            case VBOX_SHARED_CLIPBOARD_HOST_MSG_FORMATS:
                /* The host has announced available clipboard formats.
                 * Forward the information to the window, so it can later
                 * respond to WM_RENDERFORMAT message. */
                ret = uxenclipboard_parse_remote_format_announce(
                    (char*)ndata->data, ndata->len);
                if (ret) {
                    uxen_err("BrHVTray: uxenclipboard_parse_remote_format_announce error %d", ret);
                } else
                    PostMessage (pCtx->hwnd, WM_USER, 0, 0);
                break;
                
            case VBOX_SHARED_CLIPBOARD_HOST_MSG_READ_DATA: {
                uint32_t *p = (uint32_t*)ndata->data;
                /* The host needs data in the specified format. */
                PostMessage (pCtx->hwnd, WM_USER + 1, 0, *p);
                break;
            }
            case VBOX_SHARED_CLIPBOARD_HOST_MSG_QUIT:
                /* The host is terminating. */
                rc = VERR_INTERRUPTED;
                break;

            default:
                uxen_err("BrHVTray: BrHVClipboardThread: Unsupported message from host!\n");
                break;
            }
            free(msg);
        }
    }
    return 0;
}

void VBoxClipboardDestroy(const VBOXSERVICEENV *pEnv, void *pInstance)
{
    VBOXCLIPBOARDCONTEXT *pCtx = (VBOXCLIPBOARDCONTEXT *)pInstance;
    if (pCtx != &gCtx)
    {
        uxen_err("BrHVTray: BrHVClipboardDestroy: invalid instance %p (our = %p)!\n", pCtx, &gCtx);
        pCtx = &gCtx;
    }

    vboxClipboardDestroy (pCtx);
    VbglR3ClipboardDisconnect(pCtx->u32ClientID);
    memset (pCtx, 0, sizeof (*pCtx));
    return;
}

int uxenclipboard_is_allowed_format(int dir, unsigned int fmt, wchar_t *name)
{
    return 1;
}

