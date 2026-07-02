#include <nei/sys/win/utils.h>
#include <nei/core/encoding.h>

#include <windows.h>
#include <shlobj.h>

int nei_win_resolve_shortcut(const char *lnk_path,
                             char *buf, size_t size) {
    IShellLinkW *psl = NULL;
    IPersistFile *ppf = NULL;
    wchar_t wlnk[MAX_PATH];
    wchar_t target[MAX_PATH];
    int result = -1;
    int com_was_init = 0;

    if (lnk_path == NULL || buf == NULL || size == 0) return -1;

    /* Convert UTF-8 path to wide. */
    if (nei_utf8_to_wstr(lnk_path, wlnk, MAX_PATH) < 0) return -1;

    /* Initialise COM (may already be initialised by the caller). */
    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        com_was_init = (hr == S_OK || hr == S_FALSE);
    }

    /* Create ShellLink COM object. */
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IShellLinkW, (void **)&psl))) {

        /* Load the .lnk file. */
        if (SUCCEEDED(psl->lpVtbl->QueryInterface(
                psl, &IID_IPersistFile, (void **)&ppf))) {
            if (SUCCEEDED(ppf->lpVtbl->Load(ppf, wlnk, STGM_READ))) {

                /* Retrieve the target path. */
                if (SUCCEEDED(psl->lpVtbl->GetPath(
                        psl, target, MAX_PATH, NULL, SLGP_RAWPATH))) {
                    result = nei_wstr_to_utf8(target, -1, buf, size);
                }
            }
            ppf->lpVtbl->Release(ppf);
        }
        psl->lpVtbl->Release(psl);
    }

    if (com_was_init) CoUninitialize();
    return result;
}
