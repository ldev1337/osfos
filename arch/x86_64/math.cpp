#include <stdlib.h>

extern "C" int abs(int j) {
  if (j < 0) {
    if (j == (-2147483647 - 1))
      return (-2147483647 - 1);
    return -j;
  }
  return j;
}

int atoi(const char *nptr) {
  int sign = 1;
  int val = 0;

  if (!nptr)
    return 0;

  while (*nptr == ' ' || *nptr == '\t' || *nptr == '\n' || *nptr == '\r' ||
         *nptr == '\f' || *nptr == '\v')
    nptr++;

  if (*nptr == '-') {
    sign = -1;
    nptr++;
  } else if (*nptr == '+') {
    nptr++;
  }

  while (*nptr >= '0' && *nptr <= '9') {
    val = val * 10 + (*nptr - '0');
    nptr++;
  }

  return sign * val;
}

extern "C" double atof(const char *nptr) {
  if (!nptr)
    return 0.0;

  while (*nptr == ' ' || *nptr == '\t' || *nptr == '\n' || *nptr == '\r' ||
         *nptr == '\f' || *nptr == '\v')
    nptr++;

  double sign = 1.0;
  if (*nptr == '-') {
    sign = -1.0;
    nptr++;
  } else if (*nptr == '+') {
    nptr++;
  }

  double val = 0.0;
  while (*nptr >= '0' && *nptr <= '9') {
    val = val * 10.0 + static_cast<double>(*nptr - '0');
    nptr++;
  }

  if (*nptr == '.') {
    nptr++;
    double scale = 0.1;
    while (*nptr >= '0' && *nptr <= '9') {
      val += static_cast<double>(*nptr - '0') * scale;
      scale *= 0.1;
      nptr++;
    }
  }

  if (*nptr == 'e' || *nptr == 'E') {
    nptr++;
    int exp_sign = 1;
    int exp_val = 0;
    if (*nptr == '-') {
      exp_sign = -1;
      nptr++;
    } else if (*nptr == '+') {
      nptr++;
    }
    while (*nptr >= '0' && *nptr <= '9') {
      exp_val = exp_val * 10 + (*nptr - '0');
      nptr++;
    }
    double factor = 1.0;
    double base = 10.0;
    int e = exp_val;
    while (e > 0) {
      if (e & 1)
        factor *= base;
      base *= base;
      e >>= 1;
    }
    if (exp_sign < 0)
      val /= factor;
    else
      val *= factor;
  }

  return sign * val;
}
