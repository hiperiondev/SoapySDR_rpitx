CPP_SRCS += \
../src/SoapyRPITX_IQ_dmaSync.cpp \
../src/SoapyRPITX_Registration.cpp \
../src/SoapyRPITX_Settings.cpp \
../src/SoapyRPITX_Streaming.cpp \
../src/SoapySDR_Sensors.cpp 

CPP_DEPS += \
./src/SoapyRPITX_IQ_dmaSync.d \
./src/SoapyRPITX_Registration.d \
./src/SoapyRPITX_Settings.d \
./src/SoapyRPITX_Streaming.d \
./src/SoapySDR_Sensors.d 

OBJS += \
./src/SoapyRPITX_IQ_dmaSync.o \
./src/SoapyRPITX_Registration.o \
./src/SoapyRPITX_Settings.o \
./src/SoapyRPITX_Streaming.o \
./src/SoapySDR_Sensors.o 


# Each subdirectory must supply rules for building sources it contributes
src/%.o: ../src/%.cpp src/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: G++ Compiler'
	g++ -I"../librpitx/src" -O3 -Wall -c -fmessage-length=0 -Wno-sign-compare -Wno-unused-variable -fPIC -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


clean: clean-src

clean-src:
	-$(RM) ./src/SoapyRPITX_IQ_dmaSync.d ./src/SoapyRPITX_IQ_dmaSync.o ./src/SoapyRPITX_Registration.d ./src/SoapyRPITX_Registration.o ./src/SoapyRPITX_Settings.d ./src/SoapyRPITX_Settings.o ./src/SoapyRPITX_Streaming.d ./src/SoapyRPITX_Streaming.o ./src/SoapySDR_Sensors.d ./src/SoapySDR_Sensors.o

.PHONY: clean-src

