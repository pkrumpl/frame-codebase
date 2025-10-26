// cpp_example.h - Header for C++ example (C-compatible)

#ifndef CPP_EXAMPLE_H
#define CPP_EXAMPLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Functions callable from C code
int cpp_example_get_counter(void);
void cpp_example_increment(void);
int cpp_example_add(int a, int b);

#ifdef __cplusplus
}
#endif

#endif // CPP_EXAMPLE_H