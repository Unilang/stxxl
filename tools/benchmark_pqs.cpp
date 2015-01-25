/***************************************************************************
 *  tools/benchmark_pqs.cpp
 *
 *  Part of the STXXL. See http://stxxl.sourceforge.net
 *
 *  Copyright (C) 2014-2015 Thomas Keh <thomas.keh@student.kit.edu>
 *  Copyright (C) 2014-2015 Timo Bingmann <tb@panthema.net>
 *
 *  Distributed under the Boost Software License, Version 1.0.
 *  (See accompanying file LICENSE_1_0.txt or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 **************************************************************************/

static const char* description =
    "Benchmark different priority queue implementations using a sequence "
    "of operations. The PQ contains of 24 byte values. "
    "The operation sequence is either a simple fill/delete "
    "cycle or fill/intermixed inserts/deletes.";

#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <queue>

#include <stxxl/bits/common/tuple.h>
#include <stxxl/bits/containers/priority_queue.h>
#include <stxxl/bits/containers/parallel_priority_queue.h>
#include <stxxl/bits/verbose.h>
#include <stxxl/bits/unused.h>
#include <stxxl/cmdline>
#include <stxxl/random>
#include <stxxl/sorter>
#include <stxxl/timer>

#if STXXL_PARALLEL
  #include <omp.h>
#endif

using stxxl::uint32;
using stxxl::uint64;

using stxxl::scoped_print_timer;
using stxxl::random_number32_r;

static const uint64 KiB = 1024L;
static const uint64 MiB = 1024L * KiB;
static const uint64 GiB = 1024L * MiB;

// process indicator
static const bool g_progress = false;
static const uint64 g_printmod = 16 * MiB;

// value size must be > 16
static const unsigned value_size = 24;

// constant values for STXXL PQ
const uint64 _RAM = 16 * GiB;
const uint64 _volume = 1024 * GiB;
const uint64 _num_elements = _volume / value_size;
const uint64 mem_for_queue = _RAM / 2;
const uint64 mem_for_prefetch_pool = _RAM / 4;
const uint64 mem_for_write_pool = _RAM / 4;

uint64 RAM = _RAM;
uint64 volume = _volume;
uint64 num_elements = _num_elements;

uint64 single_heap_ram = 1 * MiB;
uint64 extract_buffer_ram = 0;
unsigned num_write_buffers = 14;
unsigned num_prefetchers = 14;
unsigned num_read_blocks = 1;
uint64 block_size = STXXL_DEFAULT_BLOCK_SIZE(value_type);

#if STXXL_PARALLEL
const unsigned g_max_threads = omp_get_max_threads();
#else
const unsigned g_max_threads = 1;
#endif

static inline int get_thread_num()
{
#if STXXL_PARALLEL
    return omp_get_thread_num();
#else
    return 0;
#endif
}

uint64 ram_write_buffers;
uint64 bulk_size;

uint64 num_arrays = 0;
uint64 max_merge_buffer_ram_stat = 0;

uint64 value_universe_size = 100000000;

//const unsigned int seed = 12345;
unsigned int seed = 12345;

random_number32_r g_rand(seed);

void calculate_depending_parameters()
{
    num_elements = volume / value_size;
    ram_write_buffers = num_write_buffers * block_size;
}

/*
 * The value type and corresponding functions-
 */

struct value_type {
    typedef uint64 key_type;
    key_type first;
    key_type second;
    char data[value_size - 2 * sizeof(key_type)];

    value_type()
    { }

    value_type(const key_type& _key, const key_type& _data = 0)
        : first(_key), second(_data)
    {
#ifdef STXXL_VALGRIND_AVOID_UNINITIALIZED_WRITE_ERRORS
        memset(data, 0, sizeof(data));
#endif
    }

    friend std::ostream& operator << (std::ostream& o, const value_type& obj)
    {
        o << obj.first;
        return o;
    }
};

struct value_type_cmp_smaller
    : public std::binary_function<value_type, value_type, bool>
{
    bool operator () (const value_type& a, const value_type& b) const
    {
        return a.first < b.first;
    }
    value_type min_value() const
    {
        return value_type(0);
    }
    value_type max_value() const
    {
        return std::numeric_limits<value_type::key_type>::max();
    }
};

struct value_type_cmp_greater
    : public std::binary_function<value_type, value_type, bool>
{
    bool operator () (const value_type& a, const value_type& b) const
    {
        return a.first > b.first;
    }
    value_type min_value() const
    {
        return std::numeric_limits<value_type::key_type>::max();
    }
};

template <typename ValueType>
struct less_min_max : public std::binary_function<ValueType, ValueType, bool>
{
    bool operator () (const ValueType& a, const ValueType& b) const
    {
        return a < b;
    }
    ValueType min_value() const
    {
        return std::numeric_limits<ValueType>::min();
    }
    ValueType max_value() const
    {
        return std::numeric_limits<ValueType>::max();
    }
};

/*
 * Progress messages
 */

static inline void progress(const char* text, uint64 i, uint64 nelements)
{
    if (!g_progress) return;

    if ((i % g_printmod) == 0) {
        STXXL_MSG(text << " " << i << " (" << std::setprecision(5) <<
                  (static_cast<double>(i) * 100. / static_cast<double>(nelements)) << " %)");
    }
}

/*!
 * Abstract Container class with template specialized functions for sorter and
 * more.
 */
template <typename BackendType>
class Container
{
public:
    static std::string name();

    static std::string shortname();

    BackendType& backend;

    Container(BackendType& b)
        : backend(b)
    { }

    //! Simple single push() operation
    void push(const value_type& v)
    {
        backend.push(v);
    }

    //! Simple single pop() operation
    void pop()
    {
        backend.pop();
    }

    //! Simple single top() and pop() operation.
    value_type top_pop()
    {
        value_type top = backend.top();
        backend.pop();
        return top;
    }

    //! Operation called after filling the container.
    void fill_end()
    { }

    //! Test if the container is empty.
    bool empty() const
    {
        return backend.empty();
    }

    //! Return number of items still in container.
    size_t size() const
    {
        return backend.size();
    }

    void bulk_push_begin(size_t /* this_bulk_size */)
    {
        std::cout << name() << ": bulk_push_begin not supported\n";
    }

    void bulk_push_end()
    {
        std::cout << name() << ": bulk_push_end not supported\n";
    }

    void bulk_push(const value_type& v, int /* thread_num */)
    {
        backend.push(v);
    }

    //! Output additional stats
    void print_stats()
    { }
};

// *** Specialization for STXXL PQ

typedef stxxl::PRIORITY_QUEUE_GENERATOR<
        value_type,
        value_type_cmp_greater,
        mem_for_queue,
        _num_elements / 1024> stxxlpq_generator_type;

typedef stxxlpq_generator_type::result stxxlpq_type;

template <>
std::string Container<stxxlpq_type>::name()
{
    return "STXXL PQ";
}

template <>
std::string Container<stxxlpq_type>::shortname()
{
    return "oldpq";
}

// *** Specialization for STXXL Parallel PQ

typedef stxxl::parallel_priority_queue<value_type, value_type_cmp_greater> ppq_type;

template <>
std::string Container<ppq_type>::name()
{
    return "Parallel PQ";
}

template <>
std::string Container<ppq_type>::shortname()
{
    return "ppq";
}

template <>
void Container<ppq_type>::bulk_push_begin(size_t this_bulk_size)
{
    backend.bulk_push_begin(this_bulk_size);
}

template <>
void Container<ppq_type>::bulk_push_end()
{
    backend.bulk_push_end();
}

template <>
void Container<ppq_type>::bulk_push(const value_type& v, int thread_num)
{
    backend.bulk_push(v, thread_num);
}

template <>
void Container<ppq_type>::print_stats()
{
    backend.print_stats();
}

// *** Specialization for STL PQ

typedef std::priority_queue<value_type, std::vector<value_type>, value_type_cmp_greater> stlpq_type;

template <>
std::string Container<stlpq_type>::name()
{
    return "STL PQ";
}

template <>
std::string Container<stlpq_type>::shortname()
{
    return "stlpq";
}

typedef stxxl::sorter<value_type, value_type_cmp_smaller> sorter_type;

// *** Specialization for STL Sorter

template <>
std::string Container<sorter_type>::name()
{
    return "STXXL Sorter";
}

template <>
std::string Container<sorter_type>::shortname()
{
    return "sorter";
}

template <>
void Container<sorter_type>::pop()
{
    ++backend;
}

template <>
value_type Container<sorter_type>::top_pop()
{
    value_type top = *backend;
    ++backend;
    return top;
}

template <>
void Container<sorter_type>::fill_end()
{
    backend.sort(); // switch state
}

/*
 * Timer and Statistics
 */

class scoped_stats : public stxxl::scoped_print_timer
{
protected:

    //! short name of tested PQ
    std::string m_shortname;

    //! operation on PQ being timed
    std::string m_op;

    //! stxxl stats at start of operation
    stxxl::stats_data m_stxxl_stats;

public:
    template <typename ContainerType>
    scoped_stats(ContainerType& c, const std::string& op,
                 const std::string& message,
                 unsigned int rwcycles = 1)
        : stxxl::scoped_print_timer(message,
                                    rwcycles * num_elements * value_size),
          m_shortname(c.shortname()),
          m_op(op),
          m_stxxl_stats(*stxxl::stats::get_instance())
    {
    }

    ~scoped_stats()
    {
        stxxl::stats_data s =
            stxxl::stats_data(*stxxl::stats::get_instance()) - m_stxxl_stats;

        std::cout << "RESULT"
                  << " pqname=" << m_shortname
                  << " op=" << m_op
                  << " value_size=" << value_size
                  << " num_elements=" << num_elements
                  << " volume=" << num_elements * value_size
                  << " time=" << timer().seconds()
                  << " read_count=" << s.get_reads()
                  << " read_vol=" << s.get_read_volume()
                  << " read_time=" << s.get_read_time()
                  << " write_count=" << s.get_writes()
                  << " write_vol=" << s.get_written_volume()
                  << " write_time=" << s.get_write_time()
                  << " ram=" << RAM
                  << " block_size=" << block_size
                  << " bulk_size=" << bulk_size
                  << " num_threads=" << g_max_threads
                  << " single_heap_ram=" << single_heap_ram
                  << " universe_size=" << value_universe_size
                  << std::endl;
    }
};

/*
 * Benchmark Functions
 */

template <typename ContainerType>
void do_insert(ContainerType& c)
{
    scoped_stats stats(c, "push",
                       "Filling " + c.name() + " sequentially");

    for (uint64 i = 0; i < num_elements; i++)
    {
        progress("Inserting element", i, num_elements);
        c.push(value_type(num_elements - i));
    }
}

template <typename ContainerType>
void do_read(ContainerType& c)
{
    scoped_stats timer(c, "pop",
                       "Reading " + c.name());

    STXXL_CHECK_EQUAL(c.size(), num_elements);

    for (uint64 i = 0; i < num_elements; ++i)
    {
        STXXL_CHECK(!c.empty());
        STXXL_CHECK_EQUAL(c.size(), num_elements - i);
        c.pop();
        progress("Popped element", i, num_elements);
    }
}

template <typename ContainerType>
void do_read_check(ContainerType& c)
{
    scoped_stats timer(c, "pop-check",
                       "Reading " + c.name() + " and checking order");

    STXXL_CHECK_EQUAL(c.size(), num_elements);

    for (uint64 i = 0; i < num_elements; ++i)
    {
        STXXL_CHECK(!c.empty());
        STXXL_CHECK_EQUAL(c.size(), num_elements - i);

        value_type top = c.top_pop();

        STXXL_CHECK_EQUAL(top.first, i + 1);
        progress("Popped element", i, num_elements);
    }
}

template <typename ContainerType>
void do_rand_insert(ContainerType& c, unsigned int seed)
{
    scoped_stats timer(c, "push-rand",
                       "Filling " + c.name() + " randomly");

    random_number32_r datarng(seed);

    for (uint64 i = 0; i < num_elements; i++)
    {
        uint64 k = datarng() % value_universe_size;
        progress("Inserting element", i, num_elements);
        c.push(value_type(k));
    }
}

template <typename ContainerType>
void do_rand_read_check(ContainerType& c, unsigned int seed)
{
    stxxl::sorter<uint64, less_min_max<uint64> >
    sorted_vals(less_min_max<uint64>(), RAM / 2);

    {
        scoped_print_timer timer("Filling sorter for comparison");

        random_number32_r datarng(seed);

        for (uint64 i = 0; i < num_elements; ++i)
        {
            uint64 k = datarng() % value_universe_size;
            sorted_vals.push(k);
        }
    }
    sorted_vals.sort();

    {
        scoped_stats timer(c, "pop-check",
                           "Reading " + c.name() + " and check order");

        for (uint64 i = 0; i < num_elements; ++i)
        {
            STXXL_CHECK(!c.empty());
            STXXL_CHECK_EQUAL(c.size(), num_elements - i);
            STXXL_CHECK_EQUAL(c.size(), sorted_vals.size());

            value_type top = c.top_pop();

            STXXL_CHECK_EQUAL(top.first, *sorted_vals);
            progress("Popped element", i, num_elements);
            ++sorted_vals;
        }
    }
}

template <typename ContainerType>
void do_bulk_rand_read_check(ContainerType& c, unsigned int _seed,
                             bool parallel)
{
    stxxl::sorter<uint64, less_min_max<uint64> >
    sorted_vals(less_min_max<uint64>(), RAM / 2);

    {
        scoped_print_timer timer("Filling sorter for comparison",
                                 num_elements * value_size);

        for (uint64_t i = 0; i < num_elements / bulk_size; ++i)
        {
#if STXXL_PARALLEL
#pragma omp parallel if(parallel) num_threads(g_max_threads)
#endif
            {
                const unsigned thread_id =
                    parallel ? get_thread_num() : 0;

                unsigned int seed = static_cast<unsigned>(i) * _seed * thread_id;
                random_number32_r datarng(seed);

#if STXXL_PARALLEL
                #pragma omp for
#endif
                for (uint64 j = 0; j < bulk_size; ++j)
                {
                    uint64 k = datarng() % value_universe_size;
                    #pragma omp critical
                    sorted_vals.push(k);
                }
            }

            progress("Inserting element", i * bulk_size, num_elements);
        }

        STXXL_CHECK_EQUAL(sorted_vals.size(), num_elements - (num_elements % bulk_size));

        uint64 bulk_remain = num_elements % bulk_size;

#if STXXL_PARALLEL
#pragma omp parallel if(parallel)
#endif
        {
            const unsigned thread_id =
                parallel ? get_thread_num() : 0;

            unsigned int seed = static_cast<unsigned>(num_elements / bulk_size) * _seed * thread_id;
            random_number32_r datarng(seed);

#if STXXL_PARALLEL
            #pragma omp for
#endif
            for (uint64 j = 0; j < bulk_remain; ++j)
            {
                uint64 k = datarng() % value_universe_size;
                #pragma omp critical
                sorted_vals.push(k);
            }
        }

        progress("Inserting element", num_elements, num_elements);
    }

    sorted_vals.sort();
    STXXL_CHECK_EQUAL(c.size(), sorted_vals.size());

    {
        scoped_stats timer(c, "pop-check-rand",
                           "Reading " + c.name() + " and check order");

        for (uint64 i = 0; i < num_elements; ++i)
        {
            STXXL_CHECK(!c.empty());
            STXXL_CHECK_EQUAL(c.size(), num_elements - i);
            STXXL_CHECK_EQUAL(c.size(), sorted_vals.size());

            value_type top = c.top_pop();

            //STXXL_MSG("val[" << i << "] = " << top.first << " - " << *sorted_vals);

            STXXL_CHECK_EQUAL(top.first, *sorted_vals);
            progress("Popped element", i, num_elements);
            ++sorted_vals;
        }
    }
}

template <typename ContainerType>
void do_rand_intermixed(ContainerType& c, unsigned int seed, bool filled)
{
    uint64 num_inserts = filled ? num_elements : 0;
    uint64 num_deletes = 0;

    scoped_stats timer(c, "intermix-rand",
                       c.name() + ": Intermixed rand insert and delete",
                       2);

    random_number32_r datarng(seed);

    for (uint64_t i = 0; i < 2 * num_elements; ++i)
    {
        unsigned r = datarng() % 2;

        if (num_deletes < num_inserts &&
            (r > 0 || num_inserts >= (filled ? 2 * num_elements : num_elements)))
        {
            STXXL_CHECK(!c.empty());

            c.pop();

            progress("Deleting / inserting element", i, 2 * num_elements);
            ++num_deletes;
            STXXL_CHECK_EQUAL(c.size(), num_inserts - num_deletes);
        }
        else
        {
            progress("Inserting/deleting element", i, 2 * num_elements);

            uint64 k = datarng() % value_universe_size;
            c.push(value_type(k));

            num_inserts++;
            STXXL_CHECK_EQUAL(c.size(), num_inserts - num_deletes);
        }
    }
}

template <typename ContainerType>
void do_bulk_insert(ContainerType& c, bool parallel)
{
    std::string parallel_str = parallel ? " in parallel" : " sequentially";

    scoped_stats timer(c, "push-bulk",
                       "Filling " + c.name() + " with bulks" + parallel_str);

    for (uint64_t i = 0; i < num_elements / bulk_size; ++i)
    {
        c.bulk_push_begin(bulk_size);

#if STXXL_PARALLEL
#pragma omp parallel if(parallel)
#endif
        {
            const unsigned thread_id =
                parallel ? get_thread_num() : g_rand() % g_max_threads;

#if STXXL_PARALLEL
            #pragma omp for
#endif
            for (uint64 j = 0; j < bulk_size; ++j)
            {
                c.bulk_push(value_type(num_elements - (i * bulk_size + j)), thread_id);
            }
        }

        c.bulk_push_end();

        progress("Inserting element", i * bulk_size, num_elements);
    }

    c.bulk_push_begin(num_elements % bulk_size);

#if STXXL_PARALLEL
#pragma omp parallel if(parallel)
#endif
    {
        const unsigned thread_id =
            parallel ? get_thread_num() : g_rand() % g_max_threads;

#if STXXL_PARALLEL
        #pragma omp for
#endif
        for (uint64 j = 0; j < num_elements % bulk_size; j++)
        {
            c.bulk_push(value_type(num_elements % bulk_size - j), thread_id);
        }
    }

    c.bulk_push_end();

    progress("Inserting element", num_elements, num_elements);

    STXXL_CHECK_EQUAL(c.size(), num_elements);
}

template <typename ContainerType>
void do_bulk_rand_insert(ContainerType& c,
                         unsigned int _seed, bool parallel)
{
    std::string parallel_str = parallel ? " in parallel" : " sequentially";

    scoped_stats timer(c, "push-bulk-rand",
                       "Filling " + c.name() + " with bulks" + parallel_str);

    for (uint64_t i = 0; i < num_elements / bulk_size; ++i)
    {
        c.bulk_push_begin(bulk_size);

#if STXXL_PARALLEL
#pragma omp parallel if(parallel) num_threads(g_max_threads)
#endif
        {
            const unsigned thread_id =
                parallel ? get_thread_num() : 0;

            unsigned int seed = static_cast<unsigned>(i) * _seed * thread_id;
            random_number32_r datarng(seed);

#if STXXL_PARALLEL
            #pragma omp for
#endif
            for (uint64 j = 0; j < bulk_size; ++j)
            {
                uint64 k = datarng() % value_universe_size;
                c.bulk_push(value_type(k), thread_id);
            }
        }

        c.bulk_push_end();

        progress("Inserting element", i * bulk_size, num_elements);
    }

    STXXL_CHECK_EQUAL(c.size(), num_elements - (num_elements % bulk_size));

    uint64 bulk_remain = num_elements % bulk_size;

    c.bulk_push_begin(bulk_remain);

#if STXXL_PARALLEL
#pragma omp parallel if(parallel)
#endif
    {
        const unsigned thread_id =
            parallel ? get_thread_num() : 0;

        unsigned int seed = static_cast<unsigned>(num_elements / bulk_size) * _seed * thread_id;

        random_number32_r datarng(seed);

#if STXXL_PARALLEL
        #pragma omp for
#endif
        for (uint64 j = 0; j < bulk_remain; ++j)
        {
            uint64 k = datarng() % value_universe_size;
            c.bulk_push(value_type(k), thread_id);
        }
    }

    c.bulk_push_end();

    progress("Inserting element", num_elements, num_elements);

    STXXL_CHECK_EQUAL(c.size(), num_elements);
}

template <typename ContainerType>
void do_bulk_rand_intermixed(ContainerType& c,
                             unsigned int _seed, bool filled,
                             bool parallel)
{
    std::string parallel_str = parallel ? " in parallel" : "";

    uint64 num_inserts = filled ? num_elements : 0;
    uint64 num_deletes = 0;

    scoped_stats timer(c, "intermix-bulk-rand",
                       c.name() + ": Intermixed parallel rand bulk insert"
                       + parallel_str + " and delete",
                       (filled ? 3 : 2));

    for (uint64_t i = 0; i < (filled ? 3 : 2) * num_elements; ++i)
    {
        uint64 r = g_rand() % ((filled ? 2 : 1) * bulk_size);

        // probability for an extract is bulk_size times larger than for a
        // bulk_insert.  when already filled with num_elements elements:
        // probability for an extract is 2*bulk_size times larger than for
        // a bulk_insert.
        if (num_deletes < num_inserts &&
            num_deletes < (filled ? 2 : 1) * num_elements &&
            (r > 0 || num_inserts >= (filled ? 2 : 1) * num_elements))
        {
            STXXL_CHECK(!c.empty());

            c.top_pop();

            progress("Deleting / inserting element", i, (filled ? 3 : 2) * num_elements);
            ++num_deletes;
            STXXL_CHECK_EQUAL(c.size(), num_inserts - num_deletes);
        }
        else {
            uint64 this_bulk_size =
                (num_inserts + bulk_size > (filled ? 2 : 1) * num_elements)
                ? num_elements % bulk_size
                : bulk_size;

            c.bulk_push_begin(this_bulk_size);

#if STXXL_PARALLEL
#pragma omp parallel if(parallel)
#endif
            {
                const unsigned int thread_num =
                    parallel ? get_thread_num() : g_rand() % g_max_threads;

                unsigned int seed = thread_num * static_cast<unsigned>(i) * _seed;
                random_number32_r datarng(seed);

#if STXXL_PARALLEL
                #pragma omp for
#endif

                for (uint64 j = 0; j < this_bulk_size; j++)
                {
                    progress("Inserting / deleting element",
                             i + j, (filled ? 3 : 2) * num_elements);

                    uint64 k = datarng() % value_universe_size;
                    c.bulk_push(value_type(k), thread_num);
                }
            }

            c.bulk_push_end();

            num_inserts += this_bulk_size;
            i += this_bulk_size - 1;
            STXXL_CHECK_EQUAL(c.size(), num_inserts - num_deletes);
        }
    }
}

template <typename ContainerType>
void do_bulk_intermixed_check(ContainerType& c, bool parallel)
{
    std::string parallel_str = parallel ? " in parallel" : "";

    uint64 num_inserts = 0, num_deletes = 0, num_failures = 0;
    std::vector<bool> extracted_values(num_elements, false);

    {
        scoped_stats timer(c, "intermix-bulk-check",
                           c.name() + ": Intermixed parallel bulk insert"
                           + parallel_str + " and delete",
                           2);

        for (uint64_t i = 0; i < 2 * num_elements; ++i)
        {
            uint64 r = g_rand() % bulk_size;

            if (num_deletes < num_inserts && num_deletes < num_elements &&
                (r > 0 || num_inserts >= num_elements))
            {
                STXXL_CHECK(!c.empty());

                value_type top = c.top_pop();

                if (top.first < 1 || top.first > num_elements) {
                    std::cout << top.first << " has never been inserted." << std::endl;
                    ++num_failures;
                }
                else if (extracted_values[top.first - 1]) {
                    std::cout << top.first << " has already been extracted." << std::endl;
                    ++num_failures;
                }
                else {
                    extracted_values[top.first - 1] = true;
                }

                progress("Deleting / inserting element", i, 2 * num_elements);
                ++num_deletes;
                STXXL_CHECK_EQUAL(c.size(), num_inserts - num_deletes);
            }
            else
            {
                uint64 this_bulk_size = (num_inserts + bulk_size > num_elements)
                                        ? num_elements % bulk_size
                                        : bulk_size;

                c.bulk_push_begin(this_bulk_size);
#if STXXL_PARALLEL
#pragma omp parallel if(parallel)
#endif
                {
                    const unsigned int thread_num =
                        parallel ? get_thread_num() : g_rand() % g_max_threads;
#if STXXL_PARALLEL
                    #pragma omp for
#endif

                    for (uint64 j = 0; j < this_bulk_size; j++)
                    {
                        uint64 k = num_elements - num_inserts - j;
                        progress("Inserting/deleting element", i + j, 2 * num_elements);

                        c.bulk_push(value_type(k), thread_num);
                    }
                }

                c.bulk_push_end();

                num_inserts += this_bulk_size;
                i += this_bulk_size - 1;
                STXXL_CHECK_EQUAL(c.size(), num_inserts - num_deletes);
            }
        }
    }
    {
        scoped_print_timer timer("Check if all values have been extracted");
        for (uint64 i = 0; i < num_elements; ++i) {
            if (!extracted_values[i]) {
                std::cout << i + 1 << " has never been extracted." << std::endl;
                ++num_failures;
            }
        }
    }
    std::cout << num_failures << " failures." << std::endl;
}

/*
 * Dijkstra Graph SSP Algorithm with different PQs on a random graph.
 */

template <typename ContainerType>
class dijkstra_graph
{
public:
    typedef uint64 size_type;

protected:
    size_type n, m;
    std::vector<size_type> nodes;
    std::vector<bool> node_visited;
    std::vector<size_type> edge_targets;
    std::vector<size_type> edge_lengths;
    std::vector<std::vector<std::pair<size_type, size_type> > > temp_edges;

public:
    //! construct a random graph for applying Dijkstra's SSP algorithm
    dijkstra_graph(size_type n, size_type m)
        : n(n),
          m(m),
          nodes(n + 1),
          node_visited(n, false),
          edge_targets(m),
          edge_lengths(m),
          temp_edges(n)
    {
        unsigned int seed = 12345;
        random_number32_r datarng(seed);

        for (size_type i = 0; i < m; ++i) {
            size_type source = datarng() % n;
            size_type target = datarng() % n;
            size_type length = datarng();
            temp_edges[source].push_back(std::make_pair(target, length));
        }

        size_type offset = 0;

        for (size_type i = 0; i < n; ++i) {
            nodes[i] = offset;

            for (size_type j = 0; j < temp_edges[i].size(); ++j) {
                edge_targets[offset + j] = temp_edges[i][j].first;
                edge_lengths[offset + j] = temp_edges[i][j].second;
            }

            offset += temp_edges[i].size();
        }

        nodes[n] = offset;
        temp_edges.clear();
    }

    //! run using container c
    void run_random_dijkstra(ContainerType& c)
    {
        unsigned int seed = 678910;
        random_number32_r datarng(seed);

        size_type source = datarng() % n;
        c.push(value_type(source, 0));

        while (!c.empty())
        {
            value_type top = c.top_pop();

            size_type target = top.first;
            size_type distance = top.second;

            if (node_visited[target]) {
                continue;
            }
            else {
                node_visited[target] = true;
            }

            size_type edges_begin = nodes[target];
            size_type edges_end = nodes[target + 1];

            size_type num_edges = edges_end - edges_begin;
            c.bulk_push_begin(num_edges);

#if STXXL_PARALLEL
#pragma omp parallel
#endif
            {
                const unsigned thread_num = get_thread_num();
#if STXXL_PARALLEL
                #pragma omp for
#endif
                for (size_type i = edges_begin; i < edges_end; ++i)
                {
                    c.bulk_push(value_type(edge_targets[i], distance + edge_lengths[i]),
                                thread_num);
                }
            }

            c.bulk_push_end();
        }
    }
};

template <typename ContainerType>
void do_dijkstra(ContainerType& c, uint64 num_nodes, uint64 num_edges)
{
    std::cout << "Generating Graph" << std::endl;

    dijkstra_graph<ContainerType> dg(num_nodes, num_edges);

    scoped_stats timer(c, "dijkstra", "Running random dijkstra");
    dg.run_random_dijkstra(c);
}

/*
 * Defining priority queues.
 */

stxxlpq_type* stxxlpq;

ppq_type* ppq;

sorter_type* stxxlsorter;

stlpq_type* stlpq;

/*
 * Print statistics and parameters.
 */

void print_params()
{
    STXXL_MEMDUMP(RAM);
    STXXL_MEMDUMP(volume);
    STXXL_MEMDUMP(block_size);
    STXXL_MEMDUMP(single_heap_ram);
    STXXL_VARDUMP(bulk_size);
    STXXL_MEMDUMP(value_size);
    STXXL_MEMDUMP(extract_buffer_ram);
    STXXL_VARDUMP(num_elements);
    STXXL_VARDUMP(g_max_threads);
    STXXL_VARDUMP(num_write_buffers);
    STXXL_VARDUMP(num_prefetchers);
    STXXL_VARDUMP(num_read_blocks);
}

/*
 * Run the selected benchmark procedure.
 */

bool do_random = false;
bool do_check = false;
bool do_bulk = false;
bool do_intermixed = false;
bool do_parallel = false;
bool do_prefill = false;
bool do_no_read = false;

template <typename ContainerType>
void run_benchmark(ContainerType& c)
{
    scoped_stats timer(c, "overall",
                       "Running selected benchmarks on " + c.name(),
                       (do_prefill ? 4 : 2));

    if (do_bulk) {
        if (do_intermixed) {
            if (do_check) {
                do_bulk_intermixed_check(c, do_parallel);
            }
            else {
                if (do_prefill) {
                    do_bulk_rand_insert(c, seed, do_parallel);
                }
                do_bulk_rand_intermixed(c, seed, do_prefill, do_parallel);
            }
        }
        else {
            if (do_random) {
                do_bulk_rand_insert(c, seed, do_parallel);
            }
            else {
                do_bulk_insert(c, do_parallel);
            }
            c.fill_end();
            if (do_random && do_check) {
                do_bulk_rand_read_check(c, seed, do_parallel);
            }
            else if (do_check) {
                do_read_check(c);
            }
            else if (!do_no_read) {
                do_read(c);
            }
        }
    }
    else {
        if (do_intermixed) {
            if (do_prefill) {
                do_rand_insert(c, seed);
            }
            do_rand_intermixed(c, seed, do_prefill);
        }
        else {
            if (do_random) {
                do_rand_insert(c, seed);
            }
            else {
                do_insert(c);
            }
            c.fill_end();
            if (do_random && do_check) {
                do_rand_read_check(c, seed);
            }
            else if (do_check) {
                do_read_check(c);
            }
            else if (!do_no_read) {
                do_read(c);
            }
        }
    }
    c.print_stats();
}

/*
 * The main procedure.
 */

int benchmark_pqs(int argc, char* argv[])
{
    seed = static_cast<unsigned>(time(NULL));

    /*
     * Parse flags
     */

    bool do_ppq = false;
    bool do_stxxlpq = false;
    bool do_sorter = false;
    bool do_stlpq = false;

    bool do_dijkstra = false;

    stxxl::cmdline_parser cp;
    cp.set_description(description);

    cp.add_flag('n', "ppq", do_ppq, "Benchmark parallel priority queue");
    cp.add_flag('o', "stxxl", do_stxxlpq, "Benchmark stxxl priority queue");
    cp.add_flag('s', "sorter", do_sorter, "Benchmark stxxl::sorter");
    cp.add_flag('l', "stl", do_stlpq, "Benchmark std::priority_queue");

    cp.add_flag('d', "dijkstra", do_dijkstra, "Run dijkstra benchmark");
    cp.add_flag('r', "random", do_random, "Use random insert");
    cp.add_flag('p', "parallel", do_parallel, "Insert bulks in parallel");
    cp.add_flag('c', "check", do_check, "Check results");
    cp.add_flag('b', "bulk", do_bulk, "Use bulk insert");
    cp.add_flag('i', "intermixed", do_intermixed, "Intermixed insert/delete");

    cp.add_flag(0, "prefill", do_prefill,
                "Prefill queue before starting intermixed insert/delete "
                "(only with -i and without -r and -c together)");

    cp.add_flag(0, "no-read", do_no_read,
                "Do not read items from queue after insert");

    cp.add_bytes('v', "volume", volume,
                 "Amount of data to insert in Bytes (not recommended for STXXL PQ)");

    cp.add_bytes('m', "ram", RAM,
                 "Amount of main memory in Bytes (not possible for STXXL PQ)");

    cp.add_bytes('k', "bulk_size", bulk_size,
                 "Number of elements per bulk");

    cp.add_bytes('u', "universe_size", value_universe_size,
                 "Range for random values");

    cp.add_uint('w', "writebuffers", num_write_buffers,
                "Number of buffered blocks when writing to external memory");

    cp.add_bytes('x', "extract_buffer_ram", extract_buffer_ram,
                 "Maximum memory consumption of the extract buffer in Bytes");

    cp.add_bytes('y', "heap_ram", single_heap_ram,
                 "Size of a single insertion heap in Bytes");

    cp.add_uint('z', "prefetchers", num_prefetchers,
                "Number of prefetched blocks for each external array");

    if (!cp.process(argc, argv))
        return -1;

    calculate_depending_parameters();
    print_params();

    if (do_ppq) {
        ppq = new ppq_type(value_type_cmp_greater(),
                           RAM, num_prefetchers, num_write_buffers,
                           g_max_threads, single_heap_ram, extract_buffer_ram);
    }
    else if (do_stxxlpq) {
        stxxlpq = new stxxlpq_type(mem_for_prefetch_pool, mem_for_write_pool);
    }
    else if (do_sorter) {
        stxxlsorter = new sorter_type(value_type_cmp_smaller(), RAM);
    }
    else if (do_stlpq) {
        stlpq = new stlpq_type;
    }

    /*
     * Run benchmarks
     */

    if (do_ppq + do_stxxlpq + do_sorter + do_stlpq == 0) {
        STXXL_MSG("Please choose a conatiner type. Use -h for help.");
        return EXIT_FAILURE;
    }

    stxxl::stats_data stats_begin(*stxxl::stats::get_instance());

#if !STXXL_PARALLEL
    if (do_parallel) {
        STXXL_MSG("STXXL is compiled without STXXL_PARALLEL flag. "
                  "Parallel processing cannot be used.");
        return EXIT_FAILURE;
    }
#endif

    if (do_dijkstra) {
        uint64 n = num_elements / value_size;
        uint64 m = 100 * n;

        if (do_ppq) {
            Container<ppq_type> cppq(*ppq);
            ::do_dijkstra(cppq, n, m);
            cppq.print_stats();
        }
        else if (do_stxxlpq) {
            Container<stxxlpq_type> cstxxlpq(*stxxlpq);
            ::do_dijkstra(cstxxlpq, n, m);
        }
        else if (do_sorter) {
            STXXL_MSG("Sorter not supported.");
        }
        else if (do_stlpq) {
            Container<stlpq_type> cstlpq(*stlpq);
            ::do_dijkstra(cstlpq, n, m);
        }
    }
    else if (do_ppq)
    {
        Container<ppq_type> cppq(*ppq);
        run_benchmark(cppq);
    }
    else if (do_stxxlpq)
    {
        Container<stxxlpq_type> cstxxlpq(*stxxlpq);
        run_benchmark(cstxxlpq);
    }
    else if (do_sorter)
    {
        Container<sorter_type> csorter(*stxxlsorter);
        run_benchmark(csorter);
    }
    else if (do_stlpq)
    {
        Container<stlpq_type> cstlpq(*stlpq);
        run_benchmark(cstlpq);
    }

    if (do_ppq) {
        delete ppq;
    }
    else if (do_stxxlpq) {
        delete stxxlpq;
    }
    else if (do_sorter) {
        delete stxxlsorter;
    }
    else if (do_stlpq) {
        delete stlpq;
    }

    std::cout << stxxl::stats_data(*stxxl::stats::get_instance()) - stats_begin;

    return EXIT_SUCCESS;
}
