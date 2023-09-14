################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../librpitx/src/dma.cpp \
../librpitx/src/dsp.cpp \
../librpitx/src/gpio.cpp \
../librpitx/src/iqdmasync.cpp \
../librpitx/src/mailbox.cpp \
../librpitx/src/raspberry_pi_revision.cpp \
../librpitx/src/rpi.cpp \
../librpitx/src/util.cpp 

CPP_DEPS += \
./librpitx/src/dma.d \
./librpitx/src/dsp.d \
./librpitx/src/gpio.d \
./librpitx/src/iqdmasync.d \
./librpitx/src/mailbox.d \
./librpitx/src/raspberry_pi_revision.d \
./librpitx/src/rpi.d \
./librpitx/src/util.d 

OBJS += \
./librpitx/src/dma.o \
./librpitx/src/dsp.o \
./librpitx/src/gpio.o \
./librpitx/src/iqdmasync.o \
./librpitx/src/mailbox.o \
./librpitx/src/raspberry_pi_revision.o \
./librpitx/src/rpi.o \
./librpitx/src/util.o 


# Each subdirectory must supply rules for building sources it contributes
librpitx/src/%.o: ../librpitx/src/%.cpp librpitx/src/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: G++ Compiler'
	g++ -I"../librpitx/src" -O3 -Wall -c -fmessage-length=0 -Wno-sign-compare -Wno-unused-variable -fPIC -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean-librpitx-2f-src

clean-librpitx-2f-src:
	-$(RM) ./librpitx/src/dma.d ./librpitx/src/dma.o ./librpitx/src/dsp.d ./librpitx/src/dsp.o ./librpitx/src/gpio.d ./librpitx/src/gpio.o ./librpitx/src/iqdmasync.d ./librpitx/src/iqdmasync.o ./librpitx/src/mailbox.d ./librpitx/src/mailbox.o ./librpitx/src/raspberry_pi_revision.d ./librpitx/src/raspberry_pi_revision.o ./librpitx/src/rpi.d ./librpitx/src/rpi.o ./librpitx/src/util.d ./librpitx/src/util.o

.PHONY: clean-librpitx-2f-src

