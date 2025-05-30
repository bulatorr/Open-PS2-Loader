#include "types.h"
#include "loadcore.h"
#include "stdio.h"
#include "sifrpc.h"
#include "sysclib.h"
#include "usbd.h"
#include "usbd_macro.h"
#include "thbase.h"
#include "thsemap.h"
#include "ds34usb.h"

IRX_ID("ds34usb", 1, 1);

//#define DPRINTF(x...) printf(x)
#define DPRINTF(x...)

#define REQ_USB_OUT (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE)
#define REQ_USB_IN  (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE)

#define MAX_PADS 2

static u8 output_01_report[] =
    {
        0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x02,
        0xff, 0x27, 0x10, 0x00, 0x32,
        0xff, 0x27, 0x10, 0x00, 0x32,
        0xff, 0x27, 0x10, 0x00, 0x32,
        0xff, 0x27, 0x10, 0x00, 0x32,
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00};

static u8 led_patterns[][2] =
    {
        {0x1C, 0x02},
        {0x1A, 0x04},
        {0x16, 0x08},
        {0x0E, 0x10},
};

static u8 power_level[] =
    {
        0x00, 0x00, 0x02, 0x06, 0x0E, 0x1E};

static u8 rgbled_patterns[][2][3] =
    {
        {{0x00, 0x00, 0x10}, {0x00, 0x00, 0x7F}}, // light blue/blue
        {{0x00, 0x10, 0x00}, {0x00, 0x7F, 0x00}}, // light green/green/
        {{0x10, 0x10, 0x00}, {0x7F, 0x7F, 0x00}}, // light yellow/yellow
        {{0x00, 0x10, 0x10}, {0x00, 0x7F, 0x7F}}, // light cyan/cyan
};

static u8 link_key[] = // for ds4 authorisation
    {
        0x56, 0xE8, 0x81, 0x38, 0x08, 0x06, 0x51, 0x41,
        0xC0, 0x7F, 0x12, 0xAA, 0xD9, 0x66, 0x3C, 0xCE};

static u8 usb_buf[MAX_BUFFER_SIZE + 32] __attribute((aligned(4))) = {0};

int usb_probe(int devId);
int usb_connect(int devId);
int usb_disconnect(int devId);

static void usb_release(int pad);
static void usb_config_set(int result, int count, void *arg);

UsbDriver usb_driver = {NULL, NULL, "ds34usb", usb_probe, usb_connect, usb_disconnect};

static void DS3USB_init(int pad);
static void readReport(u8 *data, int pad);
static int LEDRumble(u8 *led, u8 lrum, u8 rrum, int pad);
static int Rumble(u8 lrum, u8 rrum, int pad);

ds34usb_device ds34pad[MAX_PADS];

int usb_probe(int devId)
{
    UsbDeviceDescriptor *device = NULL;

    DPRINTF("DS34USB: probe: devId=%i\n", devId);

    device = (UsbDeviceDescriptor *)UsbGetDeviceStaticDescriptor(devId, NULL, USB_DT_DEVICE);
    if (device == NULL) {
        DPRINTF("DS34USB: Error - Couldn't get device descriptor\n");
        return 0;
    }

    if (device->idVendor == DS34_VID && (device->idProduct == DS3_PID || device->idProduct == DS4_PID || device->idProduct == DS4_PID_SLIM))
        return 1;

    return 0;
}

int usb_connect(int devId)
{
    int pad, epCount;
    UsbDeviceDescriptor *device;
    UsbConfigDescriptor *config;
    UsbInterfaceDescriptor *interface;
    UsbEndpointDescriptor *endpoint;

    DPRINTF("DS34USB: connect: devId=%i\n", devId);

    for (pad = 0; pad < MAX_PADS; pad++) {
        if (ds34pad[pad].devId == -1 && ds34pad[pad].enabled)
            break;
    }

    if (pad >= MAX_PADS) {
        DPRINTF("DS34USB: Error - only %d device allowed !\n", MAX_PADS);
        return 1;
    }

    PollSema(ds34pad[pad].sema);

    ds34pad[pad].devId = devId;

    ds34pad[pad].status = DS34USB_STATE_AUTHORIZED;

    ds34pad[pad].controlEndp = UsbOpenEndpoint(devId, NULL);

    device = (UsbDeviceDescriptor *)UsbGetDeviceStaticDescriptor(devId, NULL, USB_DT_DEVICE);
    config = (UsbConfigDescriptor *)UsbGetDeviceStaticDescriptor(devId, device, USB_DT_CONFIG);
    interface = (UsbInterfaceDescriptor *)((char *)config + config->bLength);

    if (device->idProduct == DS3_PID) {
        ds34pad[pad].type = DS3;
        epCount = interface->bNumEndpoints - 1;
    } else {
        ds34pad[pad].type = DS4;
        epCount = 20; // ds4 v2 returns interface->bNumEndpoints as 0
    }

    endpoint = (UsbEndpointDescriptor *)UsbGetDeviceStaticDescriptor(devId, NULL, USB_DT_ENDPOINT);

    do {
        if (endpoint->bmAttributes == USB_ENDPOINT_XFER_INT) {
            if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN && ds34pad[pad].interruptEndp < 0) {
                ds34pad[pad].interruptEndp = UsbOpenEndpointAligned(devId, endpoint);
                DPRINTF("DS34USB: register Event endpoint id =%i addr=%02X packetSize=%i\n", ds34pad[pad].interruptEndp, endpoint->bEndpointAddress, (unsigned short int)endpoint->wMaxPacketSizeHB << 8 | endpoint->wMaxPacketSizeLB);
            }
            if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT && ds34pad[pad].outEndp < 0) {
                ds34pad[pad].outEndp = UsbOpenEndpointAligned(devId, endpoint);
                DPRINTF("DS34USB: register Output endpoint id =%i addr=%02X packetSize=%i\n", ds34pad[pad].outEndp, endpoint->bEndpointAddress, (unsigned short int)endpoint->wMaxPacketSizeHB << 8 | endpoint->wMaxPacketSizeLB);
            }
        }

        endpoint = (UsbEndpointDescriptor *)((char *)endpoint + endpoint->bLength);

    } while (epCount--);

    if (ds34pad[pad].interruptEndp < 0 || ds34pad[pad].outEndp < 0) {
        usb_release(pad);
        return 1;
    }

    ds34pad[pad].status |= DS34USB_STATE_CONNECTED;

    UsbSetDeviceConfiguration(ds34pad[pad].controlEndp, config->bConfigurationValue, usb_config_set, (void *)pad);
    SignalSema(ds34pad[pad].sema);

    return 0;
}

int usb_disconnect(int devId)
{
    u8 pad;

    DPRINTF("DS34USB: disconnect: devId=%i\n", devId);

    for (pad = 0; pad < MAX_PADS; pad++) {
        if (ds34pad[pad].devId == devId)
            break;
    }

    if (pad < MAX_PADS)
        usb_release(pad);

    return 0;
}

static void usb_release(int pad)
{
    PollSema(ds34pad[pad].sema);

    if (ds34pad[pad].interruptEndp >= 0)
        UsbCloseEndpoint(ds34pad[pad].interruptEndp);

    if (ds34pad[pad].outEndp >= 0)
        UsbCloseEndpoint(ds34pad[pad].outEndp);

    ds34pad[pad].controlEndp = -1;
    ds34pad[pad].interruptEndp = -1;
    ds34pad[pad].outEndp = -1;
    ds34pad[pad].devId = -1;
    ds34pad[pad].status = DS34USB_STATE_DISCONNECTED;

    SignalSema(ds34pad[pad].sema);
}

static int usb_resulCode;

static void usb_data_cb(int resultCode, int bytes, void *arg)
{
    int pad = (int)arg;

    // DPRINTF("DS34USB: usb_data_cb: res %d, bytes %d, arg %p \n", resultCode, bytes, arg);

    usb_resulCode = resultCode;

    SignalSema(ds34pad[pad].sema);
}

static void usb_cmd_cb(int resultCode, int bytes, void *arg)
{
    int pad = (int)arg;

    // DPRINTF("DS34USB: usb_cmd_cb: res %d, bytes %d, arg %p \n", resultCode, bytes, arg);

    SignalSema(ds34pad[pad].cmd_sema);
}

static void usb_config_set(int result, int count, void *arg)
{
    int pad = (int)arg;
    u8 led[4];

    PollSema(ds34pad[pad].sema);

    ds34pad[pad].status |= DS34USB_STATE_CONFIGURED;

    if (ds34pad[pad].type == DS3) {
        DS3USB_init(pad);
        DelayThread(10000);
        led[0] = led_patterns[pad][1];
        led[3] = 0;
    } else if (ds34pad[pad].type == DS4) {
        led[0] = rgbled_patterns[pad][1][0];
        led[1] = rgbled_patterns[pad][1][1];
        led[2] = rgbled_patterns[pad][1][2];
        led[3] = 0;
    }

    LEDRumble(led, 0, 0, pad);
    DelayThread(20000);

    ds34pad[pad].status |= DS34USB_STATE_RUNNING;

    SignalSema(ds34pad[pad].sema);
}

static void DS3USB_init(int pad)
{
    usb_buf[0] = 0x42;
    usb_buf[1] = 0x0c;
    usb_buf[2] = 0x00;
    usb_buf[3] = 0x00;

    UsbControlTransfer(ds34pad[pad].controlEndp, REQ_USB_OUT, USB_REQ_SET_REPORT, (HID_USB_GET_REPORT_FEATURE << 8) | 0xF4, 0, 4, usb_buf, NULL, NULL);
}

static void readReport(u8 *data, int pad)
{
    if (data[0]) {

        if (ds34pad[pad].type == DS3) {
            struct ds3report *report;

            report = (struct ds3report *)&data[2];

            ds34pad[pad].data[0] = ~report->ButtonStateL;
            ds34pad[pad].data[1] = ~report->ButtonStateH;

            ds34pad[pad].data[2] = report->RightStickX; // rx
            ds34pad[pad].data[3] = report->RightStickY; // ry
            ds34pad[pad].data[4] = report->LeftStickX;  // lx
            ds34pad[pad].data[5] = report->LeftStickY;  // ly

            ds34pad[pad].data[6] = report->PressureRight; // right
            ds34pad[pad].data[7] = report->PressureLeft;  // left
            ds34pad[pad].data[8] = report->PressureUp;    // up
            ds34pad[pad].data[9] = report->PressureDown;  // down

            ds34pad[pad].data[10] = report->PressureTriangle; // triangle
            ds34pad[pad].data[11] = report->PressureCircle;   // circle
            ds34pad[pad].data[12] = report->PressureCross;    // cross
            ds34pad[pad].data[13] = report->PressureSquare;   // square

            ds34pad[pad].data[14] = report->PressureL1; // L1
            ds34pad[pad].data[15] = report->PressureR1; // R1
            ds34pad[pad].data[16] = report->PressureL2; // L2
            ds34pad[pad].data[17] = report->PressureR2; // R2

            if (report->PSButton && report->Power != 0xEE) // display battery level
                ds34pad[pad].oldled[0] = power_level[report->Power];
            else
                ds34pad[pad].oldled[0] = led_patterns[pad][1];

            if (report->Power == 0xEE) // charging
                ds34pad[pad].oldled[3] = 1;
            else
                ds34pad[pad].oldled[3] = 0;

        } else if (ds34pad[pad].type == DS4) {
            struct ds4report *report;
            u8 up = 0, down = 0, left = 0, right = 0;

            report = (struct ds4report *)data;

            switch (report->Dpad) {
                case 0:
                    up = 1;
                    break;
                case 1:
                    up = 1;
                    right = 1;
                    break;
                case 2:
                    right = 1;
                    break;
                case 3:
                    down = 1;
                    right = 1;
                    break;
                case 4:
                    down = 1;
                    break;
                case 5:
                    down = 1;
                    left = 1;
                    break;
                case 6:
                    left = 1;
                    break;
                case 7:
                    up = 1;
                    left = 1;
                    break;
                case 8:
                    up = 0;
                    down = 0;
                    left = 0;
                    right = 0;
                    break;
            }

            if (report->TPad) {
                if (!report->nFinger1Active) {
                    if (report->Finger1X < 960)
                        report->Share = 1;
                    else
                        report->Option = 1;
                }

                if (!report->nFinger2Active) {
                    if (report->Finger2X < 960)
                        report->Share = 1;
                    else
                        report->Option = 1;
                }
            }

            ds34pad[pad].data[0] = ~(report->Share | report->L3 << 1 | report->R3 << 2 | report->Option << 3 | up << 4 | right << 5 | down << 6 | left << 7);
            ds34pad[pad].data[1] = ~(report->L2 | report->R2 << 1 | report->L1 << 2 | report->R1 << 3 | report->Triangle << 4 | report->Circle << 5 | report->Cross << 6 | report->Square << 7);

            ds34pad[pad].data[2] = report->RightStickX; // rx
            ds34pad[pad].data[3] = report->RightStickY; // ry
            ds34pad[pad].data[4] = report->LeftStickX;  // lx
            ds34pad[pad].data[5] = report->LeftStickY;  // ly

            ds34pad[pad].data[6] = right * 255; // right
            ds34pad[pad].data[7] = left * 255;  // left
            ds34pad[pad].data[8] = up * 255;    // up
            ds34pad[pad].data[9] = down * 255;  // down

            ds34pad[pad].data[10] = report->Triangle * 255; // triangle
            ds34pad[pad].data[11] = report->Circle * 255;   // circle
            ds34pad[pad].data[12] = report->Cross * 255;    // cross
            ds34pad[pad].data[13] = report->Square * 255;   // square

            ds34pad[pad].data[14] = report->L1 * 255;   // L1
            ds34pad[pad].data[15] = report->R1 * 255;   // R1
            ds34pad[pad].data[16] = report->PressureL2; // L2
            ds34pad[pad].data[17] = report->PressureR2; // R2

            if (report->PSButton) { // display battery level
                ds34pad[pad].oldled[0] = report->Battery;
                ds34pad[pad].oldled[1] = 0;
                ds34pad[pad].oldled[2] = 0;
            } else {
                ds34pad[pad].oldled[0] = rgbled_patterns[pad][1][0];
                ds34pad[pad].oldled[1] = rgbled_patterns[pad][1][1];
                ds34pad[pad].oldled[2] = rgbled_patterns[pad][1][2];
            }

            if (report->Power != 0xB && report->Usb_plugged) // charging
                ds34pad[pad].oldled[3] = 1;
            else
                ds34pad[pad].oldled[3] = 0;
        }
    }
}

static int LEDRumble(u8 *led, u8 lrum, u8 rrum, int pad)
{
    int ret = 0;

    PollSema(ds34pad[pad].cmd_sema);

    mips_memset(usb_buf, 0, sizeof(usb_buf));

    if (ds34pad[pad].type == DS3) {
        mips_memcpy(usb_buf, output_01_report, sizeof(output_01_report));

        usb_buf[1] = 0xFE; // rt
        usb_buf[2] = rrum; // rp
        usb_buf[3] = 0xFE; // lt
        usb_buf[4] = lrum; // lp

        usb_buf[9] = led[0] & 0x7F; // LED Conf

        if (led[3]) { // means charging, so blink
            usb_buf[13] = 0x32;
            usb_buf[18] = 0x32;
            usb_buf[23] = 0x32;
            usb_buf[28] = 0x32;
        }

        ret = UsbControlTransfer(ds34pad[pad].controlEndp, REQ_USB_OUT, USB_REQ_SET_REPORT, (HID_USB_SET_REPORT_OUTPUT << 8) | 0x01, 0, sizeof(output_01_report), usb_buf, usb_cmd_cb, (void *)pad);
    } else if (ds34pad[pad].type == DS4) {
        usb_buf[0] = 0x02;
        usb_buf[1] = 0xFF;

        usb_buf[4] = rrum; // ds4 has full control
        usb_buf[5] = lrum;

        usb_buf[6] = led[0]; // r
        usb_buf[7] = led[1]; // g
        usb_buf[8] = led[2]; // b

        if (led[3]) {           // means charging, so blink
            usb_buf[9] = 0x80;  // Time to flash bright (255 = 2.5 seconds)
            usb_buf[10] = 0x80; // Time to flash dark (255 = 2.5 seconds)
        }

        ret = UsbInterruptTransfer(ds34pad[pad].outEndp, usb_buf, 32, usb_cmd_cb, (void *)pad);
    }

    ds34pad[pad].oldled[0] = led[0];
    ds34pad[pad].oldled[1] = led[1];
    ds34pad[pad].oldled[2] = led[2];
    ds34pad[pad].oldled[3] = led[3];

    ds34pad[pad].lrum = lrum;
    ds34pad[pad].rrum = rrum;

    return ret;
}

static unsigned int timeout(void *arg)
{
    int sema = (int)arg;
    iSignalSema(sema);
    return 0;
}

static void TransferWait(int sema)
{
    iop_sys_clock_t cmd_timeout;

    cmd_timeout.lo = 200000;
    cmd_timeout.hi = 0;

    if (SetAlarm(&cmd_timeout, timeout, (void *)sema) == 0) {
        WaitSema(sema);
        CancelAlarm(timeout, NULL);
    }
}

static int LEDRUM(u8 *led, u8 lrum, u8 rrum, int pad)
{
    int ret = 0;

    WaitSema(ds34pad[pad].sema);

    if (ds34pad[pad].update_rum) {
        ret = LEDRumble(led, lrum, rrum, pad);

        if (ret == USB_RC_OK)
            TransferWait(ds34pad[pad].cmd_sema);
        else
            DPRINTF("DS34USB: LEDRUM usb transfer error 0x%02X\n", ret);
    }

    SignalSema(ds34pad[pad].sema);

    return ret;
}

static int LED(u8 *led, int pad)
{
    return LEDRUM(led, ds34pad[pad].lrum, ds34pad[pad].rrum, pad);
}

static int Rumble(u8 lrum, u8 rrum, int pad)
{
    return LEDRUM(ds34pad[pad].oldled, lrum, rrum, pad);
}

void ds34usb_set_rumble(u8 lrum, u8 rrum, int port)
{
    if (port >= MAX_PADS)
        return;

    Rumble(lrum, rrum, port);
}

void ds34usb_set_led(u8 *led, int port)
{
    if (port >= MAX_PADS)
        return;

    LED(led, port);
}

void ds34usb_get_data(char *dst, int size, int port)
{
    int ret;

    if (port >= MAX_PADS)
        return;

    WaitSema(ds34pad[port].sema);

    PollSema(ds34pad[port].sema);

    ret = UsbInterruptTransfer(ds34pad[port].interruptEndp, usb_buf, MAX_BUFFER_SIZE, usb_data_cb, (void *)port);

    if (ret == USB_RC_OK) {
        TransferWait(ds34pad[port].sema);
        if (!usb_resulCode)
            readReport(usb_buf, port);

        usb_resulCode = 1;
    } else {
        DPRINTF("DS34USB: ds34usb_get_data usb transfer error %d\n", ret);
    }

    mips_memcpy(dst, ds34pad[port].data, size);

    SignalSema(ds34pad[port].sema);
}

int ds34usb_get_bdaddr(u8 *data, int port)
{
    int i, ret;

    if (port >= MAX_PADS)
        return 0;

    if (ds34pad[port].update_rum) {
        ds34pad[port].update_rum = 0;
        return 0;
    }

    WaitSema(ds34pad[port].sema);

    PollSema(ds34pad[port].cmd_sema);

    if (ds34pad[port].type == DS3) {
        ret = UsbControlTransfer(ds34pad[port].controlEndp, REQ_USB_IN, USB_REQ_GET_REPORT, (HID_USB_GET_REPORT_FEATURE << 8) | 0xF5, 0, 8, usb_buf, usb_cmd_cb, (void *)port);

        if (ret == USB_RC_OK) {
            TransferWait(ds34pad[port].cmd_sema);

            for (i = 0; i < 6; i++)
                data[5 - i] = usb_buf[2 + i];

            ret = 1;
        } else {
            DPRINTF("DS34USB: ds3usb_get_bdaddr usb transfer error %d\n", ret);
            ret = 0;
        }
    } else {
        ret = UsbControlTransfer(ds34pad[port].controlEndp, REQ_USB_IN, USB_REQ_GET_REPORT, (HID_USB_GET_REPORT_FEATURE << 8) | 0x09, 0, 8, usb_buf, usb_cmd_cb, (void *)port);

        if (ret == USB_RC_OK) {
            TransferWait(ds34pad[port].cmd_sema);

            for (i = 0; i < 6; i++)
                data[5 - i] = usb_buf[2 + i];

            ret = 1;
        } else {
            DPRINTF("DS34USB: ds3usb_get_bdaddr usb transfer error %d\n", ret);
            ret = 0;
        }
    }

    ds34pad[port].update_rum = 1;
    SignalSema(ds34pad[port].sema);

    return ret;
}

void ds34usb_set_bdaddr(u8 *data, int port)
{
    int i, ret;

    if (port >= MAX_PADS)
        return;

    WaitSema(ds34pad[port].sema);

    PollSema(ds34pad[port].cmd_sema);

    if (ds34pad[port].type == DS3) {
        usb_buf[0] = 0x01;
        usb_buf[1] = 0x00;

        for (i = 0; i < 6; i++)
            usb_buf[i + 2] = data[5 - i];

        ret = UsbControlTransfer(ds34pad[port].controlEndp, REQ_USB_OUT, USB_REQ_SET_REPORT, (HID_USB_GET_REPORT_FEATURE << 8) | 0xF5, 0, 8, usb_buf, usb_cmd_cb, (void *)port);
    } else {
        usb_buf[0] = 0x13;

        for (i = 0; i < 6; i++)
            usb_buf[i + 1] = data[i];

        for (i = 0; i < sizeof(link_key); i++)
            usb_buf[i + 7] = link_key[i];

        ret = UsbControlTransfer(ds34pad[port].controlEndp, REQ_USB_OUT, USB_REQ_SET_REPORT, (HID_USB_GET_REPORT_FEATURE << 8) | 0x13, 0, 24, usb_buf, usb_cmd_cb, (void *)port);
    }

    if (ret == USB_RC_OK)
        TransferWait(ds34pad[port].cmd_sema);
    else
        DPRINTF("DS34USB: ds3usb_set_bdaddr usb transfer error %d\n", ret);

    SignalSema(ds34pad[port].sema);
}

void ds34usb_reset()
{
    int pad;

    for (pad = 0; pad < MAX_PADS; pad++)
        usb_release(pad);
}

int ds34usb_get_status(int port)
{
    int ret;

    if (port >= MAX_PADS)
        return 0;

    WaitSema(ds34pad[port].sema);
    ret = ds34pad[port].status;
    SignalSema(ds34pad[port].sema);

    return ret;
}

void ds34usb_init(u8 pads)
{
    u8 pad;

    for (pad = 0; pad < MAX_PADS; pad++) {
        WaitSema(ds34pad[pad].sema);
        ds34pad[pad].enabled = (pads >> pad) & 1;
        SignalSema(ds34pad[pad].sema);
    }
}

static void rpc_thread(void *data);
static void *rpc_sf(int cmd, void *data, int size);

static SifRpcDataQueue_t rpc_que __attribute__((aligned(16)));
static SifRpcServerData_t rpc_svr __attribute__((aligned(16)));

static int rpc_buf[64] __attribute((aligned(16)));

#define DS34USB_INIT       1
#define DS34USB_GET_STATUS 2
#define DS34USB_GET_BDADDR 3
#define DS34USB_SET_BDADDR 4
#define DS34USB_SET_RUMBLE 5
#define DS34USB_SET_LED    6
#define DS34USB_GET_DATA   7
#define DS34USB_RESET      8

#define DS34USB_BIND_RPC_ID 0x18E3878E

void rpc_thread(void *data)
{
    SifInitRpc(0);
    SifSetRpcQueue(&rpc_que, GetThreadId());
    SifRegisterRpc(&rpc_svr, DS34USB_BIND_RPC_ID, rpc_sf, rpc_buf, NULL, NULL, &rpc_que);
    SifRpcLoop(&rpc_que);
}

void *rpc_sf(int cmd, void *data, int size)
{
    switch (cmd) {
        case DS34USB_INIT:
            ds34usb_init(*(u8 *)data);
            break;
        case DS34USB_GET_STATUS:
            *(u8 *)data = ds34usb_get_status(*(u8 *)data);
            break;
        case DS34USB_GET_BDADDR:
            *(u8 *)data = ds34usb_get_bdaddr(((u8 *)data + 1), *(u8 *)data);
            break;
        case DS34USB_SET_BDADDR:
            ds34usb_set_bdaddr(((u8 *)data + 1), *(u8 *)data);
            break;
        case DS34USB_SET_RUMBLE:
            ds34usb_set_rumble(*((u8 *)data + 1), *((u8 *)data + 2), *(u8 *)data);
            break;
        case DS34USB_SET_LED:
            ds34usb_set_led(((u8 *)data + 1), *(u8 *)data);
            break;
        case DS34USB_GET_DATA:
            ds34usb_get_data((char *)data, 18, *(u8 *)data);
            break;
        case DS34USB_RESET:
            ds34usb_reset();
            break;
        default:
            break;
    }

    return data;
}

int _start(int argc, char *argv[])
{
    int pad;
    u8 enable = 0xFF;

    if (argc > 1) {
        enable = argv[1][0];
    }

    for (pad = 0; pad < MAX_PADS; pad++) {
        ds34pad[pad].status = 0;
        ds34pad[pad].devId = -1;
        ds34pad[pad].oldled[0] = 0;
        ds34pad[pad].oldled[1] = 0;
        ds34pad[pad].oldled[2] = 0;
        ds34pad[pad].oldled[3] = 0;
        ds34pad[pad].lrum = 0;
        ds34pad[pad].rrum = 0;
        ds34pad[pad].update_rum = 1;
        ds34pad[pad].sema = -1;
        ds34pad[pad].cmd_sema = -1;
        ds34pad[pad].controlEndp = -1;
        ds34pad[pad].interruptEndp = -1;
        ds34pad[pad].outEndp = -1;
        ds34pad[pad].enabled = (enable >> pad) & 1;
        ds34pad[pad].type = 0;

        ds34pad[pad].data[0] = 0xFF;
        ds34pad[pad].data[1] = 0xFF;

        mips_memset(&ds34pad[pad].data[2], 0x7F, 4);
        mips_memset(&ds34pad[pad].data[6], 0x00, 12);

        ds34pad[pad].sema = CreateMutex(IOP_MUTEX_UNLOCKED);
        ds34pad[pad].cmd_sema = CreateMutex(IOP_MUTEX_UNLOCKED);

        if (ds34pad[pad].sema < 0 || ds34pad[pad].cmd_sema < 0) {
            DPRINTF("DS34USB: Failed to allocate I/O semaphore.\n");
            return MODULE_NO_RESIDENT_END;
        }
    }

    if (UsbRegisterDriver(&usb_driver) != USB_RC_OK) {
        DPRINTF("DS34USB: Error registering USB devices\n");
        return MODULE_NO_RESIDENT_END;
    }

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
