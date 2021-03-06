//sio2man hook code taken from opl mcemu:
/*
   Copyright 2006-2008, Romz
   Copyright 2010, Polo
   Licenced under Academic Free License version 3.0
   Review OpenUsbLd README & LICENSE files for further details.
*/

#include "pademu.h"

#ifdef BT

#include "ds3bt.h"

#define PAD_INIT ds3bt_init
#define PAD_GET_STATUS ds3bt_get_status
#define PAD_RESET ds3bt_reset
#define PAD_GET_DATA ds3bt_get_data
#define PAD_SET_RUMBLE ds3bt_set_rumble

#elif defined(USB)

#include "ds3usb.h"

#define PAD_INIT ds3usb_init
#define PAD_GET_STATUS ds3usb_get_status
#define PAD_RESET ds3usb_reset
#define PAD_GET_DATA ds3usb_get_data
#define PAD_SET_RUMBLE ds3usb_set_rumble

#else
#error "must define mode"
#endif

//#define DPRINTF(x...) printf(x)
#define DPRINTF(x...)

typedef struct
{
    uint8_t config_mode;
    uint8_t mode;
    uint8_t enabled;
    uint8_t vibration;
    uint8_t mask[4];
    uint8_t actalign[6];
} pad_status_t;

#define DIGITAL_MODE 0x41
#define ANALOG_MODE 0x73
#define ANALOGP_MODE 0x79
#define CONFIG_MODE 0xF3

#define MAX_PORTS 2

#define PAD_STATE_RUNNING 0x08

IRX_ID("pademu", 1, 1);

#define PADEMU_BIND_RPC_ID 0x18E3878D
#define PADEMU_RESET 1

//#define OLD_PADMAN

PtrRegisterLibraryEntires pRegisterLibraryEntires; /* Pointer to RegisterLibraryEntires routine */
Sio2McProc pSio2man25, pSio2man51;                 /* Pointers to SIO2MAN routines */
pad_status_t pad[MAX_PORTS];

#ifdef OLD_PADMAN
void (*pSio2man23)();
void hookSio2man23();
#endif

int install_sio2hook();

int hookRegisterLibraryEntires(iop_library_t *lib);
void hookSio2man25(sio2_transfer_data_t *sd);
void hookSio2man51(sio2_transfer_data_t *sd);
void InstallSio2manHook(void *exp);

void pademu_hookSio2man(sio2_transfer_data_t *td, Sio2McProc sio2proc);
void pademu_setup(uint8_t ports, uint8_t vib);
void pademu(sio2_transfer_data_t *td);
void pademu_cmd(int port, uint8_t *in, uint8_t *out, uint8_t out_size);
void pademu_reset();

static void rpc_thread(void *data);
static void *rpc_sf(int cmd, void *data, int size);

static SifRpcDataQueue_t rpc_que __attribute__((aligned(16)));
static SifRpcServerData_t rpc_svr __attribute__((aligned(16)));

//static int rpc_buf[64] __attribute((aligned(16)));

void rpc_thread(void *data)
{
    if (sceSifCheckInit() == 0) {
        DPRINTF("BT: Sif not initialized \n");
        sceSifInit();
    }

    SifInitRpc(0);
    SifSetRpcQueue(&rpc_que, GetThreadId());
    SifRegisterRpc(&rpc_svr, PADEMU_BIND_RPC_ID, rpc_sf, NULL, NULL, NULL, &rpc_que);
    SifRpcLoop(&rpc_que);
}

void *rpc_sf(int cmd, void *data, int size)
{
    if (cmd == PADEMU_RESET)
        pademu_reset();

    return data;
}

int _start(int argc, char *argv[])
{
    uint8_t enable = 0xFF, vibration = 0xFF;

    if (argc > 1) {
        enable = argv[1][0];
        vibration = argv[1][1];
    }

    if (!PAD_INIT(enable)) {
        return MODULE_NO_RESIDENT_END;
    }

#ifndef VMC
    if (!install_sio2hook()) {
        return MODULE_NO_RESIDENT_END;
    }
#endif

    pademu_setup(enable, vibration);

    iop_thread_t rpc_th;

    rpc_th.attr = TH_C;
    rpc_th.thread = rpc_thread;
    rpc_th.priority = 40;
    rpc_th.stacksize = 0x800;
    rpc_th.option = 0;

    int thid = CreateThread(&rpc_th);

    if (thid > 0) {
        StartThread(thid, NULL);
        return MODULE_RESIDENT_END;
    }

    return MODULE_NO_RESIDENT_END;
}

int install_sio2hook()
{
    register void *exp;

    /* looking for LOADCORE's library entry table */
    exp = GetExportTable("loadcore", 0x100);
    if (exp == NULL) {
        DPRINTF("Unable to find loadcore exports.\n");
        return 0;
    }

    /* hooking LOADCORE's RegisterLibraryEntires routine */
    pRegisterLibraryEntires = (PtrRegisterLibraryEntires)HookExportEntry(exp, 6, hookRegisterLibraryEntires);

    /* searching for a SIO2MAN export table */
    exp = GetExportTable("sio2man", 0x201);
    if (exp != NULL) {
        /* hooking SIO2MAN's routines */
        InstallSio2manHook(exp);
    } else {
        DPRINTF("SIO2MAN exports not found.\n");
    }

    return 1;
}

void InstallSio2manHook(void *exp)
{
    /* hooking SIO2MAN entry #25 (used by MCMAN and old PADMAN) */
    pSio2man25 = HookExportEntry(exp, 25, hookSio2man25);
    /* hooking SIO2MAN entry #51 (used by MC2_* modules and PADMAN) */
    pSio2man51 = HookExportEntry(exp, 51, hookSio2man51);
#ifdef OLD_PADMAN
    pSio2man23 = HookExportEntry(exp, 23, hookSio2man23);
#endif
}

/* Hook for the LOADCORE's RegisterLibraryEntires call */
int hookRegisterLibraryEntires(iop_library_t *lib)
{
    if (!strncmp(lib->name, "sio2man", 8)) {
        /* hooking SIO2MAN's routines */
        InstallSio2manHook(&lib[1]);
    }

    DPRINTF("registering library %s\n", lib->name);

    return pRegisterLibraryEntires(lib);
}

/* Hook for SIO2MAN entry #25 */
void hookSio2man25(sio2_transfer_data_t *sd)
{
    pademu_hookSio2man(sd, pSio2man25);
}

/* Hook for SIO2MAN entry #51 */
void hookSio2man51(sio2_transfer_data_t *sd)
{
    pademu_hookSio2man(sd, pSio2man51);
}

#ifdef OLD_PADMAN
void hookSio2man23()
{
    return;
}
#endif

void pademu_hookSio2man(sio2_transfer_data_t *td, Sio2McProc sio2proc)
{
    register u32 ctrl, port1, port2;

    ctrl = td->regdata[0];
    port1 = td->regdata[0] & 0x03;
    port2 = td->regdata[1] & 0x03;

    if ((ctrl & 0xF0) == 0x40 && td->in[0] == 0x01 && td->port_ctrl2[port1] != 0x00030064) {

        if (port2 == 1) //2 pads at once
        {
            if (pad[0].enabled && pad[1].enabled) //emulating 2 pads
            {
                sio2proc = pademu;
            } else if (pad[0].enabled || pad[1].enabled) // only one
            {
#ifdef OLD_PADMAN
                pSio2man23();
#endif
                sio2proc(td);
                sio2proc = pademu;
            } else {
#ifdef OLD_PADMAN
                pSio2man23();
#endif
            }
        } else {
            if (pad[port1].enabled) //emulating this port
                sio2proc = pademu;
#ifdef OLD_PADMAN
            else
                pSio2man23();
#endif
        }
    }

    sio2proc(td);
}

void pademu_setup(uint8_t ports, uint8_t vib)
{
    uint8_t i;

    for (i = 0; i < MAX_PORTS; i++) {
        pad[i].config_mode = 0;
        pad[i].mode = DIGITAL_MODE;
        pad[i].enabled = ((ports >> i) & 1);
        pad[i].vibration = ((vib >> i) & 1);

        mips_memset(pad[i].actalign, 0xFF, 6);

        pad[i].mask[0] = 0xFF;
        pad[i].mask[1] = 0xFF;
        pad[i].mask[2] = 0x03;
        pad[i].mask[3] = 0x00;
    }
}

uint8_t pademu_data[6][6] =
    {
        {0x00, 0x00, 0x02, 0x00, 0x00, 0x5A},
        {0x03, 0x02, 0x00, 0x02, 0x01, 0x00},
        {0x00, 0x00, 0x01, 0x02, 0x00, 0x0A},
        {0x00, 0x00, 0x01, 0x01, 0x01, 0x14},
        {0x00, 0x00, 0x02, 0x00, 0x01, 0x00},
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

void pademu(sio2_transfer_data_t *td)
{
    int port;
    uint8_t port1, port2, cmd_size;
    uint8_t *in, *out;

    port1 = td->regdata[0] & 0x03;
    port2 = td->regdata[1] & 0x03;

    td->stat6c = 0x1100; //?
    td->stat70 = 0x0F;   //?

    if (port2 == 1) {
        //find next cmd
        for (cmd_size = 5; cmd_size < td->in_size - 3; cmd_size++) {
            if (td->in[cmd_size] == 0x01 && (td->in[cmd_size + 1] & 0xF0) == 0x40 && td->in[cmd_size + 2] == 0x00) {
                if (cmd_size != 5 && cmd_size != 9 && cmd_size != 21)
                    continue;
                else
                    break;
            }
        }

        if (cmd_size + 3 == td->in_size) {
            DPRINTF("Second cmd not found!\n");
            return;
        }

        if (pad[0].enabled) {
            in = td->in;
            out = td->out;
            port = 0;

            if (pad[1].enabled) //emulating ports 0 & 1
            {
                pademu_cmd(1, (uint8_t *)&td->in[cmd_size], (uint8_t *)&td->out[cmd_size], td->in_size - cmd_size);
            }
        } else //emulating only port 1
        {
            in = (uint8_t *)&td->in[cmd_size];
            out = (uint8_t *)&td->out[cmd_size];
            port = 1;
            cmd_size = td->in_size - cmd_size;
        }
    } else {
        in = td->in;
        out = td->out;
        port = port1;
        cmd_size = td->in_size;
    }

    pademu_cmd(port, in, out, cmd_size);
}

void pademu_cmd(int port, uint8_t *in, uint8_t *out, uint8_t out_size)
{
    uint8_t rrum = 0, lrum = 0;
    uint8_t i;

    mips_memset(out, 0x00, out_size);

    if (!(PAD_GET_STATUS(port) & PAD_STATE_RUNNING)) {
        return;
    }

    out[0] = 0xFF;
    out[1] = pad[port].config_mode ? CONFIG_MODE : pad[port].mode;
    out[2] = 0x5A;

    switch (in[1]) {
        case 0x40: //set vref param
            mips_memcpy(&out[3], &pademu_data[0], 6);
            break;

        case 0x41: //query button mask
            if (pad[port].mode != DIGITAL_MODE) {
                out[3] = pad[port].mask[0];
                out[4] = pad[port].mask[1];
                out[5] = pad[port].mask[2];
                out[6] = pad[port].mask[3];
                out[7] = 0x00;
                out[8] = 0x5A;
            }
            break;

        case 0x42: //read data
            PAD_GET_DATA(&out[3], out_size - 3, port);

            if (pad[port].vibration) //disable/enable vibration
            {
                for (i = 0; i < 6; i++) //vibration
                {
                    if (pad[port].actalign[i] == 0x00)
                        rrum = in[i + 3];

                    if (pad[port].actalign[i] == 0x01)
                        lrum = in[i + 3];
                }
                PAD_SET_RUMBLE(lrum, rrum, port);
            }

            break;

        case 0x43: //enter/exit config mode
            pad[port].config_mode = (in[3] == 0x01);
            break;

        case 0x44: //set mode and lock
            pad[port].mode = (in[3] == 0x01) ? ANALOG_MODE : DIGITAL_MODE;
            break;

        case 0x45: //query model and mode
            mips_memcpy(&out[3], &pademu_data[1], 6);
            out[5] = (pad[port].mode & 0xF) != 1;
            break;

        case 0x46: //query act
            if (in[3] == 0x00)
                mips_memcpy(&out[3], &pademu_data[2], 6);
            else
                mips_memcpy(&out[3], &pademu_data[3], 6);

            break;

        case 0x47: //query comb
            mips_memcpy(&out[3], &pademu_data[4], 6);
            break;

        case 0x4C: //query mode
            if (in[3] == 0x00)
                out[6] = 0x04;
            else
                out[6] = 0x07;

            break;

        case 0x4D: //set act align
            mips_memcpy(pad[port].actalign, &in[3], 6);
            mips_memcpy(&out[3], &pademu_data[5], 6);
            break;

        case 0x4F: //set button info
            pad[port].mode = ANALOGP_MODE;

            out[8] = 0x5A;

            pad[port].mask[0] = in[3];
            pad[port].mask[1] = in[4];
            pad[port].mask[2] = in[5];
            pad[port].mask[3] = in[6];
            break;
    }
}

void pademu_reset()
{
    PAD_RESET();
}
