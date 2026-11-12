// Camera stub until Waveshare's OV5640 PIO capture (C/02-CAM in their demo
// pack) is ported. Returning nullptr makes the scan screen wait.
#include "../app/qrscan.h"

const uint8_t *platform_camera_frame(int *, int *) { return nullptr; }
