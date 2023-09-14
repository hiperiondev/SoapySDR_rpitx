################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../src/SoapyRPITX_Registration.cpp \
../src/SoapyRPITX_Settings.cpp \
../src/SoapyRPITX_Streaming.cpp \
../src/SoapySDR_Sensors.cpp \
../src/libRPITX.cpp 

CPP_DEPS += \
./src/SoapyRPITX_Registration.d \
./src/SoapyRPITX_Settings.d \
./src/SoapyRPITX_Streaming.d \
./src/SoapySDR_Sensors.d \
./src/libRPITX.d 

OBJS += \
./src/SoapyRPITX_Registration.o \
./src/SoapyRPITX_Settings.o \
./src/SoapyRPITX_Streaming.o \
./src/SoapySDR_Sensors.o \
./src/libRPITX.o 


# Each subdirectory must supply rules for building sources it contributes
src/%.o: ../src/%.cpp src/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: G++ Compiler'
	g++ -I"../librpitx/src" -O3 -Wall -c -fmessage-length=0 -Wno-sign-compare -Wno-unused-variable -fPIC -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean-src

clean-src:
	-$(RM) ./src/SoapyRPITX_Registration.d ./src/SoapyRPITX_Registration.o ./src/SoapyRPITX_Settings.d ./src/SoapyRPITX_Settings.o ./src/SoapyRPITX_Streaming.d ./src/SoapyRPITX_Streaming.o ./src/SoapySDR_Sensors.d ./src/SoapySDR_Sensors.o ./src/libRPITX.d ./src/libRPITX.o

.PHONY: clean-src

