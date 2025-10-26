// cpp_example.cpp - Simple C++ example for Frame

// Use extern "C" to make functions callable from C code
extern "C" {
    #include <stdint.h>
}

// Simple C++ class
class Counter {
private:
    int value;

public:
    Counter() : value(0) {}  // Constructor

    void increment() {
        value++;
    }

    int get_value() const {
        return value;
    }
};

// Global instance (tests static constructors)
static Counter global_counter;

// C-style functions callable from C code
extern "C" {

int cpp_example_get_counter(void) {
    return global_counter.get_value();
}

void cpp_example_increment(void) {
    global_counter.increment();
}

// Test function with simple computation
int cpp_example_add(int a, int b) {
    Counter temp;
    for (int i = 0; i < a; i++) {
        temp.increment();
    }
    for (int i = 0; i < b; i++) {
        temp.increment();
    }
    return temp.get_value();
}

} // extern "C"