#pragma once

#include <3ds.h>
#include <citro3d.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads a PNG file from the romfs into a C3D_Tex.
// Returns 0 on success, -1 on failure.
int texture_load_to_tex(const char* path, C3D_Tex* out_tex, int* out_w, int* out_h);

// Frees the VRAM data of a C3D_Tex.
void texture_destroy_tex(C3D_Tex* tex);

#ifdef __cplusplus
}
#endif
