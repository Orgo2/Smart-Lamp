#ifndef USB_CLI_H
#define USB_CLI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void USB_CLI_Init(void);
void USB_CLI_Task(void);

// CDC write function for use by MiniPascal and other modules
void cdc_write_str(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* USB_CLI_H */