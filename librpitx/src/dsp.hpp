#ifndef DEF_DSP
#define DEF_DSP

#include "stdint.h"
#include <iostream>
#include <math.h>
#include <complex>

class dsp {
protected:
    double prev_phase = 0;

    double constrainAngle(double x);
    double angleConv(double angle);
    double angleDiff(double a, double b);
    double unwrap(double previousAngle, double newAngle);
    int arctan2(int y, int x);

public:
    uint32_t samplerate = 0;
    //double phase;
    double amplitude = 0;
    double frequency = 0;

    dsp();
    dsp(uint32_t samplerate);
    void pushsample(std::complex<float> sample);
};
#endif
