#include <pspmoduleexport.h>
#define NULL ((void *) 0)

extern int module_start;
extern int module_stop;
extern int module_info;
static const unsigned int __syslib_exports[6] __attribute__((section(".rodata.sceResident"))) = {
    0xD632ACDB,
    0xCEE8593C,
    0xF01D73A7,
    (unsigned int) &module_start,
    (unsigned int) &module_stop,
    (unsigned int) &module_info,
};

extern int pspDveMgrCheckVideoOut;
extern int pspDveMgrSetVideoOut;
extern int pspDveMgrSetEdramSize;
extern int pspDveMgrGetModel;
static const unsigned int __sm64DveMgr_exports[8] __attribute__((section(".rodata.sceResident"))) = {
    0x2ACFCB6D,
    0xF9C86C73,
    0xC8CCE483,
    0x6D0DE1A1,
    (unsigned int) &pspDveMgrCheckVideoOut,
    (unsigned int) &pspDveMgrSetVideoOut,
    (unsigned int) &pspDveMgrSetEdramSize,
    (unsigned int) &pspDveMgrGetModel,
};

const struct _PspLibraryEntry __library_exports[2] __attribute__((section(".lib.ent"), used)) = {
    { NULL, 0x0000, 0x8000, 4, 1, 2, (unsigned int *) &__syslib_exports },
    { "sm64DveMgr", 0x0000, 0x4001, 4, 0, 4, (unsigned int *) &__sm64DveMgr_exports },
};
