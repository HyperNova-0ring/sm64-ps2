#ifdef TARGET_PS2

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <tamtypes.h>
#include <kernel.h>
#include <timer.h>
#include <libmc.h>
#include <sjis.h>

#include "ps2_memcard.h"

// only memory card slot 1 (mc0) is used
#define MAX_PORTS 1

#define ICN_FILE  "sm64.icn"
#define META_FILE "icon.sys"
#define SAVE_FILE "save.bin"
#define ID_FILE   "id.bin"

// identifies the card a session is allowed to write to
#define ID_SIZE 5

#define THREAD_STACK_SIZE (16 * 1024)

extern unsigned int size_ps2_icon_data;
extern unsigned char ps2_icon_data;
extern void *_gp;

#define ALIGN1K(x) (((x) + 1023) >> 10)
// icon.sys + icon + save.bin + id.bin + directory entries, in 1k clusters
#define SAVE_SIZE (ALIGN1K(sizeof(mcIcon)) + ALIGN1K(size_ps2_icon_data) + 1 + 1 + 3)

// RAM copy of the EEPROM; the game thread writes it, the memcard thread snapshots it
static u8 eeprom[PS2_EEPROM_SIZE] __attribute__((aligned(64)));
// data being read from or written to the card by the memcard thread
static u8 io_buf[PS2_EEPROM_SIZE] __attribute__((aligned(64)));

// ID of the card this session loaded from or created; memcard thread only
static u8 session_id[ID_SIZE];
static bool session_has_id;

// per port state; memcard thread only
static int card_formatted[MAX_PORTS] = { -1 }; // -1 = unknown
static int card_free[MAX_PORTS];

static int lock_sema = -1; // guards eeprom and req_save
static int wake_sema = -1; // wakes the memcard thread
static int boot_sema = -1; // signaled when the boot sequence finishes
static int grant_sema = -1; // signaled when the game thread grants the IOP
static bool thread_ok;

static volatile bool audio_sync;
static volatile bool iop_requested;
static volatile bool iop_granted;

static volatile bool req_boot;
static volatile bool req_save;
static volatile bool boot_done;
static volatile enum Ps2McResult boot_result = PS2_MC_RES_NONE;

static volatile enum Ps2McActivity activity = PS2_MC_IDLE;
static volatile enum Ps2McResult last_result = PS2_MC_RES_NONE;
static volatile u32 result_seq;

static u8 thread_stack[THREAD_STACK_SIZE] __attribute__((aligned(16)));

/* file helpers */

static void make_path(char *buf, const int port, const char *file) {
    if (file)
        sprintf(buf, "mc%d:" PS2_SAVE_PATH "/%s", port, file);
    else
        sprintf(buf, "mc%d:" PS2_SAVE_PATH, port);
}

static int file_read(const int port, const char *name, void *dst, const int size) {
    char path[64];
    make_path(path, port, name);
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    const int ret = read(fd, dst, size);
    close(fd);
    return ret;
}

static bool file_write(const int port, const char *name, const void *src, const int size) {
    char path[64];
    make_path(path, port, name);
    const int fd = open(path, O_WRONLY | O_CREAT);
    if (fd < 0) return false;
    const int ret = write(fd, src, size);
    close(fd);
    return ret == size;
}

static bool file_exists(const int port, const char *name) {
    char path[64];
    make_path(path, port, name);
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    close(fd);
    return true;
}

/* card helpers */

// refreshes a port's state; true if a formatted PS2 card is present
static bool card_probe(const int port) {
    int type = 0, free = 0, format = 0, ret = -10;

    if (mcGetInfo(port, 0, &type, &free, &format) < 0) return false;
    mcSync(MC_WAIT, NULL, &ret);

    if (ret == -1) {
        card_formatted[port] = 1; // new formatted card
    } else if (ret == -2) {
        card_formatted[port] = 0; // new unformatted card
    } else if (ret != 0) {
        card_formatted[port] = -1; // no card or access error
        return false;
    } else if (card_formatted[port] < 0) {
        card_formatted[port] = 1; // same card as before, assume formatted
    }

    card_free[port] = free;

    return card_formatted[port] == 1 && type == MC_TYPE_PS2;
}

static void make_id(u8 *id) {
    u32 count;
    __asm__ __volatile__ ("mfc0 %0, $9" : "=r" (count));

    u64 x = GetTimerSystemTime() ^ ((u64)count << 32) ^ 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < ID_SIZE; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        id[i] = (u8)(x >> 24);
    }
}

static bool create_meta(const int port) {
    static const iconIVECTOR bgcolor[] = {
        {  68,  23, 116,  0 }, // top left
        { 255, 255, 255,  0 }, // top right
        { 255, 255, 255,  0 }, // bottom left
        {  68,  23, 116,  0 }, // bottom right
    };

    static const iconFVECTOR lightdir[] = {
        { 0.5, 0.5, 0.5, 0.0 },
        { 0.0,-0.4,-0.1, 0.0 },
        {-0.5,-0.5, 0.5, 0.0 },
    };

    static const iconFVECTOR lightcol[] = {
        { 0.3, 0.3, 0.3, 0.00 },
        { 0.4, 0.4, 0.4, 0.00 },
        { 0.5, 0.5, 0.5, 0.00 },
    };

    static const iconFVECTOR ambient = { 0.50, 0.50, 0.50, 0.00 };

    static mcIcon icon_sys __attribute__((aligned(64)));

    memset(&icon_sys, 0, sizeof(mcIcon));
    strcpy(icon_sys.head, "PS2D");
    strcpy_sjis((short *)&icon_sys.title, "Super\nMario 64");
    icon_sys.nlOffset = 16;
    icon_sys.trans = 0x60;
    memcpy(icon_sys.bgCol, bgcolor, sizeof(bgcolor));
    memcpy(icon_sys.lightDir, lightdir, sizeof(lightdir));
    memcpy(icon_sys.lightCol, lightcol, sizeof(lightcol));
    memcpy(icon_sys.lightAmbient, ambient, sizeof(ambient));
    strcpy(icon_sys.view, ICN_FILE); // these filenames are relative to the directory
    strcpy(icon_sys.copy, ICN_FILE); // in which icon.sys resides.
    strcpy(icon_sys.del,  ICN_FILE);

    if (!file_write(port, ICN_FILE, &ps2_icon_data, size_ps2_icon_data)) return false;
    return file_write(port, META_FILE, &icon_sys, sizeof(icon_sys));
}

// creates the save directory with the given data and a new ID, which the session adopts
static bool create_save(const int port, const u8 *data) {
    // TODO: dedicated "memory card full" icon instead of the generic error
    if (card_free[port] < (int)SAVE_SIZE) {
        printf("ps2_memcard: not enough space on port %d (%d < %u)\n", port, card_free[port], SAVE_SIZE);
        return false;
    }

    char path[64];
    make_path(path, port, NULL);
    mkdir(path, 0777); // may already exist

    if (!create_meta(port)) return false;

    // ID first: if the save write fails, the card is still seen as having no save
    u8 id[ID_SIZE];
    make_id(id);
    if (!file_write(port, ID_FILE, id, ID_SIZE)) return false;
    if (!file_write(port, SAVE_FILE, data, PS2_EEPROM_SIZE)) return false;

    memcpy(session_id, id, ID_SIZE);
    session_has_id = true;

    printf("ps2_memcard: created save on port %d\n", port);
    return true;
}

static void snapshot_eeprom(void) {
    WaitSema(lock_sema);
    memcpy(io_buf, eeprom, PS2_EEPROM_SIZE);
    req_save = false;
    SignalSema(lock_sema);
}

/* operations */

static enum Ps2McResult load_save(const int port) {
    memset(io_buf, 0, PS2_EEPROM_SIZE);

    // unreadable or truncated save: leave it alone, the game starts with empty files
    if (file_read(port, SAVE_FILE, io_buf, PS2_EEPROM_SIZE) != PS2_EEPROM_SIZE) {
        printf("ps2_memcard: could not read save on port %d\n", port);
        return PS2_MC_RES_ERROR;
    }

    if (file_read(port, ID_FILE, session_id, ID_SIZE) == ID_SIZE) {
        session_has_id = true;
    } else {
        // save made before ID files existed, only add the ID
        u8 id[ID_SIZE];
        make_id(id);
        if (file_write(port, ID_FILE, id, ID_SIZE)) {
            memcpy(session_id, id, ID_SIZE);
            session_has_id = true;
        }
    }

    WaitSema(lock_sema);
    memcpy(eeprom, io_buf, PS2_EEPROM_SIZE);
    SignalSema(lock_sema);

    printf("ps2_memcard: loaded save from port %d\n", port);
    return PS2_MC_RES_LOADED;
}

static enum Ps2McResult do_boot(void) {
    int free_port = -1;

    // start from a blank session, as if no card was ever read
    session_has_id = false;
    WaitSema(lock_sema);
    memset(eeprom, 0, PS2_EEPROM_SIZE);
    SignalSema(lock_sema);

    for (int port = 0; port < MAX_PORTS; ++port) {
        if (!card_probe(port)) continue;
        if (file_exists(port, SAVE_FILE)) return load_save(port);
        if (free_port < 0) free_port = port;
    }

    if (free_port < 0) {
        printf("ps2_memcard: no usable memory card\n");
        return PS2_MC_RES_ERROR;
    }

    snapshot_eeprom();
    return create_save(free_port, io_buf) ? PS2_MC_RES_CREATED : PS2_MC_RES_ERROR;
}

static enum Ps2McResult do_save(void) {
    int blank_port = -1;

    snapshot_eeprom();

    for (int port = 0; port < MAX_PORTS; ++port) {
        if (!card_probe(port)) continue;

        u8 id[ID_SIZE];
        if (session_has_id && file_read(port, ID_FILE, id, ID_SIZE) == ID_SIZE && !memcmp(id, session_id, ID_SIZE))
            return file_write(port, SAVE_FILE, io_buf, PS2_EEPROM_SIZE) ? PS2_MC_RES_SAVED : PS2_MC_RES_ERROR;

        if (blank_port < 0 && !file_exists(port, SAVE_FILE))
            blank_port = port;
    }

    // no card with our ID; a card without any save can take it, anything else is left alone
    if (blank_port >= 0)
        return create_save(blank_port, io_buf) ? PS2_MC_RES_SAVED : PS2_MC_RES_ERROR;

    printf("ps2_memcard: no card matching this session\n");
    return PS2_MC_RES_ERROR;
}

static void iop_acquire(void) {
    if (!audio_sync) return;
    iop_requested = true;
    WaitSema(grant_sema);
}

static void iop_release(void) {
    iop_requested = false;
    iop_granted = false;
}

static void finish(const enum Ps2McResult res) {
    last_result = res;
    result_seq++;
}

static void memcard_thread(void *arg) {
    (void)arg;

    while (1) {
        WaitSema(wake_sema);

        if (!req_boot && !req_save) continue;

        iop_acquire();

        if (req_boot) {
            req_boot = false;
            activity = PS2_MC_SEARCHING;
            const enum Ps2McResult res = do_boot();
            boot_result = res;
            boot_done = true;
            finish(res);
            SignalSema(boot_sema);
        }

        while (req_save) {
            activity = PS2_MC_SAVING;
            finish(do_save());
        }

        iop_release();
        activity = PS2_MC_IDLE;
    }
}

static int make_sema(const int init) {
    ee_sema_t sema;
    sema.init_count = init;
    sema.max_count = 1;
    sema.option = 0;
    return CreateSema(&sema);
}

/* public API */

bool ps2_memcard_init(void) {
    int ret = mcInit(MC_TYPE_XMC);
    if (ret < 0) ret = mcInit(MC_TYPE_MC);
    if (ret < 0) {
        printf("ps2_memcard: mcInit failed: %d\n", ret);
        return false;
    }

    lock_sema = make_sema(1);
    wake_sema = make_sema(0);
    boot_sema = make_sema(0);
    grant_sema = make_sema(0);
    if (lock_sema < 0 || wake_sema < 0 || boot_sema < 0 || grant_sema < 0) {
        printf("ps2_memcard: could not create semaphores\n");
        return false;
    }

    // the memcard thread only runs while the game thread is waiting
    ee_thread_status_t status;
    const int main_tid = GetThreadId();
    ReferThreadStatus(main_tid, &status);
    int prio = status.current_priority;
    if (prio >= 127) {
        prio = 126;
        ChangeThreadPriority(main_tid, prio);
    }

    ee_thread_t thread;
    memset(&thread, 0, sizeof(thread));
    thread.func = memcard_thread;
    thread.stack = thread_stack;
    thread.stack_size = sizeof(thread_stack);
    thread.gp_reg = &_gp;
    thread.initial_priority = prio + 1;

    const int tid = CreateThread(&thread);
    if (tid < 0 || StartThread(tid, NULL) < 0) {
        printf("ps2_memcard: could not start thread\n");
        return false;
    }

    printf("ps2_memcard: SAVE_SIZE = %u, thread priority %d\n", SAVE_SIZE, prio + 1);

    thread_ok = true;
    return true;
}

void ps2_memcard_boot_start(void) {
    if (!thread_ok) {
        boot_result = PS2_MC_RES_ERROR;
        boot_done = true;
        finish(PS2_MC_RES_ERROR);
        return;
    }
    boot_done = false;
    boot_result = PS2_MC_RES_NONE;
    activity = PS2_MC_SEARCHING;
    req_boot = true;
    SignalSema(wake_sema);
}

bool ps2_memcard_boot_done(void) {
    return boot_done;
}

enum Ps2McResult ps2_memcard_boot_wait(void) {
    if (thread_ok && !boot_done)
        WaitSema(boot_sema);
    return boot_result;
}

void ps2_memcard_read(void *dst, const uint32_t ofs, const uint32_t size) {
    if (ofs >= PS2_EEPROM_SIZE) return;
    const uint32_t len = (ofs + size > PS2_EEPROM_SIZE) ? PS2_EEPROM_SIZE - ofs : size;

    if (thread_ok) WaitSema(lock_sema);
    memcpy(dst, eeprom + ofs, len);
    if (thread_ok) SignalSema(lock_sema);
}

void ps2_memcard_write(const void *src, const uint32_t ofs, const uint32_t size) {
    if (ofs >= PS2_EEPROM_SIZE) return;
    const uint32_t len = (ofs + size > PS2_EEPROM_SIZE) ? PS2_EEPROM_SIZE - ofs : size;

    if (!thread_ok) {
        memcpy(eeprom + ofs, src, len);
        finish(PS2_MC_RES_ERROR);
        return;
    }

    WaitSema(lock_sema);
    memcpy(eeprom + ofs, src, len);
    req_save = true;
    SignalSema(lock_sema);

    SignalSema(wake_sema);
}

void ps2_memcard_set_audio_sync(bool enable) {
    audio_sync = enable;
}

bool ps2_memcard_iop_requested(void) {
    return iop_requested && !iop_granted;
}

void ps2_memcard_iop_grant(void) {
    if (!iop_requested || iop_granted) return;
    iop_granted = true;
    SignalSema(grant_sema);
}

bool ps2_memcard_iop_in_use(void) {
    return iop_granted;
}

enum Ps2McActivity ps2_memcard_activity(void) {
    // a queued save counts as saving even before the thread picks it up
    if (req_save && activity == PS2_MC_IDLE) return PS2_MC_SAVING;
    return activity;
}

uint32_t ps2_memcard_last_result(enum Ps2McResult *res) {
    const u32 seq = result_seq;
    if (res) *res = last_result;
    return seq;
}

#endif // TARGET_PS2
