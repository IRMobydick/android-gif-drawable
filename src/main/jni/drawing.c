#include "gif.h"

#if defined (__arm__)
    extern void arm_memset32(uint32_t* dst, uint32_t value, int count);
    #define MEMSET_ARGB(dst, value, count) arm_memset32(dst, value, (int) count)
#elif defined (__ARM_ARCH_7A__)
    extern void memset32_neon(uint32_t* dst, uint32_t value, int count);
    #define MEMSET_ARGB(dst, value, count) memset32_neon(dst, value, (int) count)
#else
#define MEMSET_ARGB(dst, value, count) memset(dst, value, count * sizeof(argb))
#endif

static void blitNormal(argb *bm, GifInfo *info, SavedImage *frame, ColorMapObject *cmap) {
    unsigned char *src = info->rasterBits;
    argb *dst = GET_ADDR(bm, info->stride, frame->ImageDesc.Left, frame->ImageDesc.Top);

    uint_fast16_t x, y = frame->ImageDesc.Height;
    const int_fast16_t transpIndex = info->infos[info->currentIndex].TransparentColor;
    if (transpIndex == NO_TRANSPARENT_COLOR) {
        for (; y > 0; y--) {
            MEMSET_ARGB((uint32_t *) dst, UINT32_MAX, frame->ImageDesc.Width);
            for (x = frame->ImageDesc.Width; x > 0; x--, src++, dst++)
                dst->rgb = cmap->Colors[*src];
            dst += info->stride - frame->ImageDesc.Width;
        }
    }
    else {
        for (; y > 0; y--) {
            for (x = frame->ImageDesc.Width; x > 0; x--, src++, dst++) {
                if (*src != transpIndex) {
                    dst->rgb = cmap->Colors[*src];
                    dst->alpha = 0xFF;
                }
            }
            dst += info->stride - frame->ImageDesc.Width;
        }
    }
}

static void drawFrame(argb *bm, GifInfo *info, SavedImage *frame) {
    ColorMapObject *cmap;

    if (frame->ImageDesc.ColorMap != NULL)
        cmap = frame->ImageDesc.ColorMap;// use local color table
    else if (info->gifFilePtr->SColorMap != NULL)
        cmap = info->gifFilePtr->SColorMap;
    else
        cmap = defaultCmap;

    blitNormal(bm, info, frame, cmap);
}

// return true if area of 'target' is completely covers area of 'covered'
static bool checkIfCover(const SavedImage *target, const SavedImage *covered) {
    if (target->ImageDesc.Left <= covered->ImageDesc.Left
        && covered->ImageDesc.Left + covered->ImageDesc.Width
           <= target->ImageDesc.Left + target->ImageDesc.Width
        && target->ImageDesc.Top <= covered->ImageDesc.Top
        && covered->ImageDesc.Top + covered->ImageDesc.Height
           <= target->ImageDesc.Top + target->ImageDesc.Height) {
        return true;
    }
    return false;
}

static inline void disposeFrameIfNeeded(argb *bm, GifInfo *info, int idx) {
    GifFileType *fGif = info->gifFilePtr;
    SavedImage *cur = &fGif->SavedImages[idx - 1];
    SavedImage *next = &fGif->SavedImages[idx];
    // We can skip disposal process if next frame is not transparent
    // and completely covers current area
    uint_fast8_t curDisposal = info->infos[idx - 1].DisposalMode;
    bool nextTrans = info->infos[idx].TransparentColor != NO_TRANSPARENT_COLOR;
    unsigned char nextDisposal = info->infos[idx].DisposalMode;

    if ((curDisposal == DISPOSE_PREVIOUS || nextDisposal == DISPOSE_PREVIOUS) && info->backupPtr == NULL) {
        info->backupPtr = malloc(info->stride * fGif->SHeight * sizeof(argb));
        if (!info->backupPtr) {
            JNIEnv *env = getEnv();
            if (!env) {
                abort();
            }
            throwException(env, OUT_OF_MEMORY_ERROR, OOME_MESSAGE);
            info->gifFilePtr->Error = D_GIF_ERR_NOT_ENOUGH_MEM;
            return;
        }
    }
    argb *backup = info->backupPtr;
    if (nextTrans || !checkIfCover(next, cur)) {
        if (curDisposal == DISPOSE_BACKGROUND) {// restore to background (under this image) color
            uint32_t *dst = (uint32_t *) GET_ADDR(bm, info->stride, cur->ImageDesc.Left, cur->ImageDesc.Top);
            uint_fast16_t copyWidth = cur->ImageDesc.Width;
            if (cur->ImageDesc.Left + copyWidth > fGif->SWidth) {
                copyWidth = fGif->SWidth - cur->ImageDesc.Left;
            }

            uint_fast16_t copyHeight = cur->ImageDesc.Height;
            if (cur->ImageDesc.Top + copyHeight > fGif->SHeight) {
                copyHeight = fGif->SHeight - cur->ImageDesc.Top;
            }
            for (; copyHeight > 0; copyHeight--) {
                MEMSET_ARGB(dst, 0, copyWidth);
                dst += info->stride;
            }
        }
        else if (curDisposal == DISPOSE_PREVIOUS && nextDisposal == DISPOSE_PREVIOUS) {// restore to previous
            argb *tmp = bm;
            bm = backup;
            backup = tmp;
        }
    }

    // Save current image if next frame's disposal method == DISPOSE_PREVIOUS
    if (nextDisposal == DISPOSE_PREVIOUS)
        memcpy(backup, bm, info->stride * fGif->SHeight * sizeof(argb));
}

uint_fast16_t const getBitmap(argb *bm, GifInfo *info) {
    if (info->currentIndex == 0) {
        if (info->gifFilePtr->SColorMap && info->infos[0].TransparentColor == NO_TRANSPARENT_COLOR) {
            argb bgColArgb;
            bgColArgb.rgb = info->gifFilePtr->SColorMap->Colors[info->gifFilePtr->SBackGroundColor];
            bgColArgb.alpha = 0xFF;
            MEMSET_ARGB((uint32_t *) bm, *(uint32_t *) &bgColArgb, info->stride * info->gifFilePtr->SHeight);
        }
        else {
            MEMSET_ARGB((uint32_t *) bm, 0, info->stride * info->gifFilePtr->SHeight);
        }
    }
    else {
        disposeFrameIfNeeded(bm, info, info->currentIndex);
    }
    drawFrame(bm, info, info->gifFilePtr->SavedImages + info->currentIndex);
    uint_fast32_t frameDuration = info->infos[info->currentIndex].DelayTime;

    if (++info->currentIndex >= info->gifFilePtr->ImageCount) {
        if (info->loopCount == 0 || info->currentLoop + 1 < info->loopCount) {
            if (info->rewindFunction(info) != 0)
                return 0;
            else if (info->loopCount > 0)
                info->currentLoop++;
            info->currentIndex = 0;
        }
        else {
            info->currentLoop++;
            --info->currentIndex;
            frameDuration = 0;
        }
    }
    return frameDuration;
}

ColorMapObject *genDefColorMap(void) {
    ColorMapObject *cmap = GifMakeMapObject(8, NULL);
    if (cmap != NULL) {
        uint_fast16_t iColor;
        for (iColor = 0; iColor < 256; iColor++) {
            cmap->Colors[iColor].Red = (GifByteType) iColor;
            cmap->Colors[iColor].Green = (GifByteType) iColor;
            cmap->Colors[iColor].Blue = (GifByteType) iColor;
        }
    }
    return cmap;
}