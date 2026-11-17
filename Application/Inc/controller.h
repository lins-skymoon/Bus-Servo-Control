#ifndef BUS_CONTROLLER_H
#define BUS_CONTROLLER_H
#include "links.h"
void Controller_Service(void);
void Controller_Command(Link source, const BusFrame *frame);
#endif
