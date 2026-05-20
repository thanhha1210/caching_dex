#pragma once

#include <cstdint>
#include <map>
#include <utility>

template <class T, class P> class tree_api;

extern "C" void *create_tree();

// used to define the interface of all benchmarking trees
template <class T, class P> class tree_api {
public:
    virtual bool insert(T key, P value) = 0;
    virtual bool lookup(T key, P &value) = 0;
    virtual bool update(T key, P value) = 0;
    virtual bool remove(T key) = 0;
    // range query, return #keys really scanned
    virtual int range_scan(T key, uint32_t num, std::pair<T, P> *&result) = 0;
    virtual void clear_statistic() = 0;

    virtual void reset_buffer_pool(bool flush_dirty) {}

    virtual void set_rpc_ratio(double ratio) {}
    virtual void set_admission_ratio(double ratio) {}
    virtual double get_rpc_ratio() { return 0; }

    virtual void get_statistic() {}

    // Do most initialization work here
    tree_api *create_tree() { return nullptr; }
};