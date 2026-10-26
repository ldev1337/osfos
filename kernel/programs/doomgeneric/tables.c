//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// DESCRIPTION:
//  Lookup tables runtime generator (Freestanding / Bare-Metal).
//

#include "tables.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif

// Self-contained, highly accurate math helpers for freestanding environment (zero libm dependency)

static double my_sin(double x) {
    while (x > M_PI) x -= 2.0 * M_PI;
    while (x < -M_PI) x += 2.0 * M_PI;
    
    double x2 = x * x;
    double term = x;
    double sum = term;
    term *= -x2 / (2.0 * 3.0); sum += term;
    term *= -x2 / (4.0 * 5.0); sum += term;
    term *= -x2 / (6.0 * 7.0); sum += term;
    term *= -x2 / (8.0 * 9.0); sum += term;
    term *= -x2 / (10.0 * 11.0); sum += term;
    term *= -x2 / (12.0 * 13.0); sum += term;
    term *= -x2 / (14.0 * 15.0); sum += term;
    return sum;
}

static double my_cos(double x) {
    return my_sin(x + M_PI / 2.0);
}

static double my_tan(double x) {
    double c = my_cos(x);
    if (c > -1e-12 && c < 1e-12) {
        return (my_sin(x) >= 0.0) ? 1e8 : -1e8;
    }
    double t = my_sin(x) / c;
    if (t > 1e8) return 1e8;
    if (t < -1e8) return -1e8;
    return t;
}

static double my_atan_small(double x) {
    double x2 = x * x;
    double term = x;
    double sum = term;
    double sign = -1.0;
    for (int i = 3; i <= 35; i += 2) {
        term *= x2;
        sum += sign * (term / i);
        sign = -sign;
    }
    return sum;
}

static double my_atan(double x) {
    if (x > 1.0) {
        return (M_PI / 2.0) - my_atan_small(1.0 / x);
    } else if (x < -1.0) {
        return -(M_PI / 2.0) - my_atan_small(1.0 / (-x));
    } else {
        return my_atan_small(x);
    }
}

static double my_log(double x) {
    if (x <= 0.0) return -1e308;
    int e = 0;
    while (x < 1.0) {
        x *= 2.0;
        e--;
    }
    while (x >= 2.0) {
        x *= 0.5;
        e++;
    }
    double y = (x - 1.0) / (x + 1.0);
    double y2 = y * y;
    double term = y;
    double sum = term;
    double denom = 3.0;
    for (int i = 0; i < 20; i++) {
        term *= y2;
        sum += term / denom;
        denom += 2.0;
    }
    return 2.0 * sum + e * M_LN2;
}

static double my_exp(double z) {
    if (z < -700.0) return 0.0;
    if (z > 700.0) return 1e308;
    
    double k_d = z / M_LN2;
    int k = (int)(k_d >= 0.0 ? k_d + 0.5 : k_d - 0.5);
    double f = z - k * M_LN2;
    
    double sum = 1.0;
    double term = 1.0;
    for (int i = 1; i <= 25; i++) {
        term *= f / i;
        sum += term;
    }
    
    double p2 = 1.0;
    if (k >= 0) {
        for (int i = 0; i < k; i++) p2 *= 2.0;
    } else {
        for (int i = 0; i < -k; i++) p2 *= 0.5;
    }
    return sum * p2;
}

static double my_pow(double base, double exp) {
    if (base <= 0.0) return 0.0;
    return my_exp(exp * my_log(base));
}

// Allocate storage for tables in BSS instead of code segment
fixed_t finesine[5 * FINEANGLES / 4];
const fixed_t *finecosine = &finesine[FINEANGLES / 4];
fixed_t finetangent[FINEANGLES / 2];
byte gammatable[5][256];
angle_t tantoangle[SLOPERANGE + 1];

int SlopeDiv(unsigned int num, unsigned int den)
{
    unsigned ans;
    
    if (den < 512)
    {
        return SLOPERANGE;
    }
    else
    {
        ans = (num << 3) / (den >> 8);

        if (ans <= SLOPERANGE)
        {
            return ans;
        }
        else
        {
            return SLOPERANGE;
        }
    }
}

void R_InitTables(void)
{
    int i;

    // 1. Initialize finesine and finecosine
    for (i = 0; i < 5 * FINEANGLES / 4; i++)
    {
        finesine[i] = (fixed_t)(my_sin(i * (2.0 * M_PI / FINEANGLES)) * FRACUNIT);
    }

    // 2. Initialize finetangent
    for (i = 0; i < FINEANGLES / 2; i++)
    {
        double angle = (i - FINEANGLES / 4) * (2.0 * M_PI / FINEANGLES);
        double t = my_tan(angle);
        if (t > 1e8) t = 1e8;
        if (t < -1e8) t = -1e8;
        finetangent[i] = (fixed_t)(t * FRACUNIT);
    }

    // 3. Initialize tantoangle (ArcTan LUT)
    for (i = 0; i <= SLOPERANGE; i++)
    {
        double slope = (double)i / (double)SLOPERANGE;
        double rad = my_atan(slope);
        double f = rad / (2.0 * M_PI);
        tantoangle[i] = (angle_t)(4294967296.0 * f + 0.5);
    }

    // 4. Initialize gammatable
    {
        int g, j;
        for (j = 0; j < 256; j++)
        {
            gammatable[0][j] = (byte)j;
        }
        for (g = 1; g < 5; g++)
        {
            gammatable[g][0] = 0;
            for (j = 1; j < 256; j++)
            {
                double v = my_pow((double)j / 255.0, 1.0 / (double)(g + 1));
                if (v < 0.0) v = 0.0;
                if (v > 1.0) v = 1.0;
                gammatable[g][j] = (byte)(v * 255.0 + 0.5);
            }
        }
    }
}