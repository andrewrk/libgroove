#ifndef GROOVE_ATOMIC_VALUE_HPP
#define GROOVE_ATOMIC_VALUE_HPP

#include "atomics.hpp"

// single reader, single writer atomic value

template<typename T>
struct AtomicValue {
    void init() {
        _current_read_index.store(0);
        _next_read_index.store(0);
    }

    T *write_begin() {
        int current_read_index = _current_read_index.load();
        int next_read_index = _next_read_index.load();
        if (current_read_index != 0 && next_read_index != 0)
            _write_index = 0;
        else if (current_read_index != 1 && next_read_index != 1)
            _write_index = 1;
        else
            _write_index = 2;
        return &_values[_write_index];
    }

    void write_end() {
        _next_read_index.store(_write_index);
    }

    T *get_read_ptr() {
        _current_read_index.store(_next_read_index.load());
        return &_values[_current_read_index];
    }

    T *write(const T &value) {
        T *ptr = write_begin();
        *ptr = value;
        write_end();
        return ptr;
    }

    T _values[3];
    atomic_int _current_read_index;
    atomic_int _next_read_index;
    int _write_index;
};

#endif
