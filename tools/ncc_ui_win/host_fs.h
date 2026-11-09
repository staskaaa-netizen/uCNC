#ifndef HOST_FS_H
#define HOST_FS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the desktop filesystem on uCNC drive /D. `root` is a Windows directory
   (created if missing); NULL means ".\nc-files" next to the working directory. */
bool host_fs_mount(const char *root);

#ifdef __cplusplus
}
#endif

#endif
