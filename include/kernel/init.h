#pragma once

#define __init __attribute__((section(".os_init"), __used__))
#define __initdata __attribute__((section(".os_initdata"), __used__))