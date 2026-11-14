#ifndef UM_ENUM_H
#define UM_ENUM_H

#include "usbmon.h"
#include "um_toast_ui.h"

#define UM_EVID_MAX UM_MAX_DEV
#define UM_EVID_ID_LEN 256

typedef struct {
    char instance_id[UM_EVID_ID_LEN];
    char model[UM_MODEL_MAX];
    char serial[UM_SERIAL_MAX];
    char mount[UM_MOUNT_MAX];
    unsigned tags;
    int disk_number;       /* -1 for a device without a verified disk number */
    int bus_type;
    int removable;
    int media_absent;      /* positively identified empty reader */
    int ejectable;        /* PnP node has the removable capability */
    unsigned letters;
    unsigned accessible_letters;
    int volumes;
    unsigned long long size, fs_total, fs_free;
    um_dev_kind kind;
} um_evidence;

int um_enum_collect(um_evidence *out, int max);
int um_scan_windows(um_snapshot *snap, um_evidence *out, int max);
void um_enum_snapshot(const um_evidence *out, int n, um_snapshot *snap);
int um_enum_to_model(const um_evidence *evs, int n, um_toast_model *out);
int um_enum_letter_disk(char letter);
unsigned long um_enum_eject(const char *instance_id);

void um_gui_win_refresh(um_gui *g, const um_evidence *evidence, int n,
                        int storage_added, int storage_removed);

#endif
