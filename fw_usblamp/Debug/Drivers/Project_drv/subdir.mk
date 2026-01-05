################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Drivers/Project_drv/alarm.c \
../Drivers/Project_drv/analog.c \
../Drivers/Project_drv/bme280.c \
../Drivers/Project_drv/charger.c \
../Drivers/Project_drv/led.c \
../Drivers/Project_drv/mic.c \
../Drivers/Project_drv/rtc.c 

OBJS += \
./Drivers/Project_drv/alarm.o \
./Drivers/Project_drv/analog.o \
./Drivers/Project_drv/bme280.o \
./Drivers/Project_drv/charger.o \
./Drivers/Project_drv/led.o \
./Drivers/Project_drv/mic.o \
./Drivers/Project_drv/rtc.o 

C_DEPS += \
./Drivers/Project_drv/alarm.d \
./Drivers/Project_drv/analog.d \
./Drivers/Project_drv/bme280.d \
./Drivers/Project_drv/charger.d \
./Drivers/Project_drv/led.d \
./Drivers/Project_drv/mic.d \
./Drivers/Project_drv/rtc.d 


# Each subdirectory must supply rules for building sources it contributes
Drivers/Project_drv/%.o Drivers/Project_drv/%.su Drivers/Project_drv/%.cyclo: ../Drivers/Project_drv/%.c Drivers/Project_drv/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m0plus -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U073xx -DUX_INCLUDE_USER_DEFINE_FILE -c -I../Core/Inc -I"C:/Users/orgo/Documents/Rado/lampa/fw_usblamp/Drivers/Project_drv" -I../Drivers/STM32U0xx_HAL_Driver/Inc -I../Drivers/STM32U0xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32U0xx/Include -I../Drivers/CMSIS/Include -I../USBX/App -I../USBX/Target -I../Middlewares/ST/usbx/common/core/inc -I../Middlewares/ST/usbx/ports/generic/inc -I../Middlewares/ST/usbx/common/usbx_stm32_device_controllers -I../Middlewares/ST/usbx/common/usbx_device_classes/inc -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Drivers-2f-Project_drv

clean-Drivers-2f-Project_drv:
	-$(RM) ./Drivers/Project_drv/alarm.cyclo ./Drivers/Project_drv/alarm.d ./Drivers/Project_drv/alarm.o ./Drivers/Project_drv/alarm.su ./Drivers/Project_drv/analog.cyclo ./Drivers/Project_drv/analog.d ./Drivers/Project_drv/analog.o ./Drivers/Project_drv/analog.su ./Drivers/Project_drv/bme280.cyclo ./Drivers/Project_drv/bme280.d ./Drivers/Project_drv/bme280.o ./Drivers/Project_drv/bme280.su ./Drivers/Project_drv/charger.cyclo ./Drivers/Project_drv/charger.d ./Drivers/Project_drv/charger.o ./Drivers/Project_drv/charger.su ./Drivers/Project_drv/led.cyclo ./Drivers/Project_drv/led.d ./Drivers/Project_drv/led.o ./Drivers/Project_drv/led.su ./Drivers/Project_drv/mic.cyclo ./Drivers/Project_drv/mic.d ./Drivers/Project_drv/mic.o ./Drivers/Project_drv/mic.su ./Drivers/Project_drv/rtc.cyclo ./Drivers/Project_drv/rtc.d ./Drivers/Project_drv/rtc.o ./Drivers/Project_drv/rtc.su

.PHONY: clean-Drivers-2f-Project_drv

