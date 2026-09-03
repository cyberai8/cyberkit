#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/soc.h"

// CyberVoc-Board-V2_0 / esp-vocat share the same soft power latch.
#define VOCAT_PG2_HOLD_GPIO 7

// Referenced by the bootloader link with -u so this component is retained.
void bootloader_hooks_include(void)
{
}

// Called before the second-stage bootloader initializes flash and clocks.
void bootloader_before_init(void)
{
    PIN_FUNC_SELECT(IO_MUX_GPIO7_REG, PIN_FUNC_GPIO);
    REG_WRITE(GPIO_OUT_W1TS_REG, BIT(VOCAT_PG2_HOLD_GPIO));
    REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(VOCAT_PG2_HOLD_GPIO));
}
