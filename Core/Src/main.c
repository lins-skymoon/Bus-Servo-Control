#include "actions.h"
#include "controller.h"
#include "flash.h"
#include "usb_hid.h"

int main(void)
{
    Board_Init();
    Board_Delay(200);
    Usb_Init();
    Links_Init();
    Board_Delay(150);
    if (!Flash_Init()) {
        Board_LatchAlarm();
    }
    Board_CheckStartupVoltage();
    Actions_Init();

    for (;;) {
        Controller_Service();
        Actions_Service();
        Board_Service();
    }
}
