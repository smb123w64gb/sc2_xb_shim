#ifndef NAMCO_RENDERER_H
#define NAMCO_RENDERER_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Install hooks into Namco primitive dispatch table at 0x0019BDC0
void NamcoRenderer_InstallHooks(void);

#ifdef __cplusplus
}
#endif

#endif // NAMCO_RENDERER_H
