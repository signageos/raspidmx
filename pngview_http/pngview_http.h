//
// Created by Michal Artazov on 06.08.18.
//

#ifndef RASPIDMX_PNGVIEW_H
#define RASPIDMX_PNGVIEW_H

#include "bcm_host.h"

#define PNGVIEW_DISPLAY_NUMBER 0

void showImage(
        const char * imagePath,
        DISPMANX_DISPLAY_HANDLE_T display,
        DISPMANX_MODEINFO_T info,
        const int timeoutInSeconds
);

#endif //RASPIDMX_PNGVIEW_H
