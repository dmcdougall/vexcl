#ifndef VEXCL_DEVLIST_HPP
#define VEXCL_DEVLIST_HPP

/*
The MIT License

Copyright (c) 2012-2013 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   devlist.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  OpenCL device enumeration and context initialization.
 */

#include <vector>
#include <functional>
#include <string>
#include <fstream>
#include <limits>
#include <cstdlib>

#include <boost/config.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/filesystem.hpp>

#ifndef __CL_ENABLE_EXCEPTIONS
#  define __CL_ENABLE_EXCEPTIONS
#endif
#include <CL/cl.hpp>

#include <vexcl/util.hpp>

#ifdef __GNUC__
#  ifndef _GLIBCXX_USE_NANOSLEEP
#    define _GLIBCXX_USE_NANOSLEEP
#  endif
#endif

namespace vex {

/// Device filters.
namespace Filter {
    /// Selects any device.
    struct AllFilter {
        AllFilter() {}

        bool operator()(const cl::Device &) const {
            return true;
        }
    };

    const AllFilter All;

    /// Selects devices whose vendor name match given value.
    struct Vendor {
        explicit Vendor(std::string name) : vendor(std::move(name)) {}

        bool operator()(const cl::Device &d) const {
            return d.getInfo<CL_DEVICE_VENDOR>().find(vendor) != std::string::npos;
        }

        private:
            std::string vendor;
    };

    /// Selects devices whose platform name match given value.
    struct Platform {
        explicit Platform(std::string name) : platform(std::move(name)) {}

        bool operator()(const cl::Device &d) const {
            return cl::Platform(d.getInfo<CL_DEVICE_PLATFORM>()).getInfo<CL_PLATFORM_NAME>().find(platform) != std::string::npos;
        }

        private:
            std::string platform;
    };

    /// Selects devices whose names match given value.
    struct Name {
        explicit Name(std::string name) : devname(std::move(name)) {}

        bool operator()(const cl::Device &d) const {
            return d.getInfo<CL_DEVICE_NAME>().find(devname) != std::string::npos;
        }

        private:
            std::string devname;
    };

    /// Selects devices by type.
    struct Type {
        explicit Type(cl_device_type t)    : type(t)              {}
        explicit Type(const std::string t) : type(device_type(t)) {}

        bool operator()(const cl::Device &d) const {
            return d.getInfo<CL_DEVICE_TYPE>() == type;
        }

        private:
            cl_device_type type;

            static cl_device_type device_type(const std::string &t) {
                if (t.find("CPU") != std::string::npos)
                    return CL_DEVICE_TYPE_CPU;

                if (t.find("GPU") != std::string::npos)
                    return CL_DEVICE_TYPE_GPU;

                if (t.find("ACCELERATOR") != std::string::npos)
                    return CL_DEVICE_TYPE_ACCELERATOR;

                return CL_DEVICE_TYPE_ALL;
            }
    };

    /// Selects devices supporting double precision.
    struct DoublePrecisionFilter {
        DoublePrecisionFilter() {}

        bool operator()(const cl::Device &d) const {
            std::string ext = d.getInfo<CL_DEVICE_EXTENSIONS>();
            return (
                    ext.find("cl_khr_fp64") != std::string::npos ||
                    ext.find("cl_amd_fp64") != std::string::npos
                   );
        }
    };

    const DoublePrecisionFilter DoublePrecision;

    /// Selects no more than given number of devices.
    /**
     * \note This filter should be the last in filter expression. In this case,
     * it will be applied only to devices which passed all other filters.
     * Otherwise, you could get less devices than planned (every time this
     * filter is applied, internal counter is decremented).
     */
    struct Count {
        explicit Count(int c) : count(c) {}

        bool operator()(const cl::Device &) const {
            return --count >= 0;
        }

        private:
            mutable int count;
    };

    /// Selects one device at the given position.
    /**
     * Select one device at the given position in the list of devices
     * satisfying previously applied filters.
     */
    struct Position {
        explicit Position(int p) : pos(p) {}

        bool operator()(const cl::Device &) const {
            return 0 == pos--;
        }

        private:
            mutable int pos;
    };

    /// \internal Exclusive access to selected devices.
    class ExclusiveFilter {
        private:
            std::function<bool(const cl::Device&)> filter;

            static std::map<cl_device_id, std::string> get_uids() {
                std::map<cl_device_id, std::string> uids;

                std::vector<cl::Platform> platform;
                cl::Platform::get(&platform);

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4996)
#endif
                const char *lock_dir = getenv("VEXCL_LOCK_DIR");
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

                for(size_t p_id = 0; p_id < platform.size(); p_id++) {
                    std::vector<cl::Device> device;

                    platform[p_id].getDevices(CL_DEVICE_TYPE_ALL, &device);

                    for(size_t d_id = 0; d_id < device.size(); d_id++) {
                        std::ostringstream id;
#ifdef WIN32
#  ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable: 4996)
#  endif
                        id << (lock_dir ? lock_dir : getenv("TEMP")) << "\\";
#  ifdef _MSC_VER
#    pragma warning(pop)
#  endif
#else
                        id << (lock_dir ? lock_dir : "/tmp") << "/";
#endif
                        id << "vexcl_device_" << p_id << "_" << d_id << ".lock";

                        uids[device[d_id]()] = id.str();
                    }
                }

                return uids;
            }

            struct locker {
                locker(std::string fname) : file(fname)
                {
                    if (!file.is_open() || file.fail()) {
                        std::cerr
                            << "WARNING: failed to open file \"" << fname << "\"\n"
                            << "  Check that target directory is exists and is writable.\n"
                            << "  Exclusive mode is off.\n"
                            << std::endl;
                    } else {
                        flock.reset(new boost::interprocess::file_lock(fname.c_str()));
#if BOOST_VERSION >= 105000
                        // In case we created the lock file,
                        // lets make it writable by others:
                        try {
                            boost::filesystem::permissions(fname, boost::filesystem::all_all);
                        } catch (const boost::filesystem::filesystem_error&) {
                            (void)0;
                        }
#endif
                    }
                }

                bool try_lock() {
                    if (flock) {
                        // Try and lock the file related to compute device.
                        // If the file is locked already, it could mean two
                        // things:
                        // 1. Somebody locked the file, and uses the device.
                        // 2. Somebody locked the file, and is in process of
                        //    checking the device. If device is not good (for
                        //    them) they will release the lock in a few
                        //    moments.
                        // To process case 2 correctly, we use timed_lock().

                        return flock->timed_lock(
                                boost::posix_time::microsec_clock::universal_time() +
                                boost::posix_time::milliseconds(100)
                                );
                    } else return true;
                }

                std::ofstream file;
                std::unique_ptr<boost::interprocess::file_lock> flock;
            };
        public:
            template <class Filter>
            ExclusiveFilter(Filter&& filter)
                : filter(std::forward<Filter>(filter)) {}

            bool operator()(const cl::Device &d) const {
                static std::map<cl_device_id, std::string> dev_uids = get_uids();
                static std::vector<std::unique_ptr<locker>> locks;

                std::unique_ptr<locker> lck(new locker(dev_uids[d()]));

                if (lck->try_lock() && filter(d)) {
                    locks.push_back(std::move(lck));
                    return true;
                }

                return false;
            }

    };

    /// Allows exclusive access to compute devices across several processes.
    /**
     * Returns devices that pass through provided device filter and are not
     * locked.
     *
     * \param filter Compute device filter
     *
     * \note Depends on boost::interprocess library.
     *
     * lock files are created in directory specified in VEXCL_LOCK_DIR
     * environment variable. If the variable does not exist, /tmp is
     * used on Linux and %TMPDIR% on Windows. The lock directory should exist
     * and be writable by the running user.
     */
    template <class Filter>
    ExclusiveFilter Exclusive(Filter&& filter) {
        return ExclusiveFilter(std::forward<Filter>(filter));
    }

    /// \cond INTERNAL

    /// Negation of a filter.
    struct NegateFilter {
        template <class Filter>
        NegateFilter(Filter&& filter)
          : filter(std::forward<Filter>(filter)) {}

        bool operator()(const cl::Device &d) const {
            return !filter(d);
        }

        private:
            std::function<bool(const cl::Device&)> filter;
    };

    /// Filter join operators.
    enum FilterOp {
        FilterAnd, FilterOr
    };

    /// Filter join expression template.
    template <FilterOp op>
    struct FilterBinaryOp {
        template <class LHS, class RHS>
        FilterBinaryOp(LHS&& lhs, RHS&& rhs)
            : lhs(std::forward<LHS>(lhs)), rhs(std::forward<RHS>(rhs)) {}

        bool operator()(const cl::Device &d) const {
            // This could be hidden into FilterOp::apply() call (with FilterOp
            // being a struct instead of enum), but this form allows to rely on
            // short-circuit evaluation (important for mutable filters as Count
            // or Position).
            switch (op) {
                case FilterOr:
                    return lhs(d) || rhs(d);
                case FilterAnd:
                default:
                    return lhs(d) && rhs(d);
            }
        }

        private:
            std::function<bool(const cl::Device&)> lhs;
            std::function<bool(const cl::Device&)> rhs;
    };

    /// \endcond

    /// Join two filters with AND operator.
    template <class LHS, class RHS>
    FilterBinaryOp<FilterAnd> operator&&(LHS&& lhs, RHS&& rhs)
    {
        return FilterBinaryOp<FilterAnd>(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
    }

    /// Join two filters with OR operator.
    template <class LHS, class RHS>
    FilterBinaryOp<FilterOr> operator||(LHS&& lhs, RHS&& rhs)
    {
        return FilterBinaryOp<FilterOr>(std::forward<LHS>(lhs), std::forward<RHS>(rhs));
    }

    /// Negate a filter.
    template <class Filter>
    NegateFilter operator!(Filter&& filter) {
        return NegateFilter(std::forward<Filter>(filter));
    }

    /// Runtime filter holder.
    /**
     * The filter can be changed at runtime as in:
     * \code
     * vex::Filter::General f = vex::Filter::Env;
     * if (need_double) f = f && vex::Filter::DoublePrecision;
     * \endcode
     */
    struct General {
        template<class Filter>
        General(Filter filter) : filter(filter) { }

        bool operator()(const cl::Device &d) const {
            return filter(d);
        }

        private:
            std::function<bool(const cl::Device&)> filter;
    };

    /// Environment filter
    /**
     * Selects devices with respect to environment variables. Recognized
     * variables are:
     *
     * \li OCL_PLATFORM -- platform name;
     * \li OCL_VENDOR   -- device vendor;
     * \li OCL_DEVICE   -- device name;
     * \li OCL_TYPE     -- device type (CPU, GPU, ACCELERATOR);
     * \li OCL_MAX_DEVICES -- maximum number of devices to use.
     * \li OCL_POSITION -- devices position in the device list.
     *
     * \note Since this filter possibly counts passed devices, it should be the
     * last in filter expression. Same reasoning applies as in case of
     * Filter::Count.
     */
    struct EnvFilter {
        EnvFilter() : filter(All) {
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4996)
#endif
            const char *platform = getenv("OCL_PLATFORM");
            const char *vendor   = getenv("OCL_VENDOR");
            const char *name     = getenv("OCL_DEVICE");
            const char *devtype  = getenv("OCL_TYPE");
            const char *maxdev   = getenv("OCL_MAX_DEVICES");
            const char *position = getenv("OCL_POSITION");
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

            if (platform) filter = filter && Platform(platform);
            if (vendor)   filter = filter && Vendor(vendor);
            if (name)     filter = filter && Name(name);
            if (devtype)  filter = filter && Type(devtype);
            if (maxdev)   filter = filter && Count(std::stoi(maxdev));
            if (position) filter = filter && Position(std::stoi(position));
        }

        bool operator()(const cl::Device &d) const {
            return filter(d);
        }

        private:
            General filter;
    };

    const EnvFilter Env;

} // namespace Filter

/// Select devices by given criteria.
/**
 * \param filter  Device filter functor. Functors may be combined with logical
 *                operators.
 * \returns list of devices satisfying the provided filter.
 *
 * This example selects any GPU which supports double precision arithmetic:
 * \code
 * auto devices = device_list(
 *          Filter::Type(CL_DEVICE_TYPE_GPU) && Filter::DoublePrecision
 *          );
 * \endcode
 */
template<class DevFilter>
std::vector<cl::Device> device_list(DevFilter&& filter) {
    std::vector<cl::Device> device;

    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);

    for(auto p = platforms.begin(); p != platforms.end(); p++) {
        std::vector<cl::Device> dev_list;

        p->getDevices(CL_DEVICE_TYPE_ALL, &dev_list);

        for(auto d = dev_list.begin(); d != dev_list.end(); d++) {
            if (!d->getInfo<CL_DEVICE_AVAILABLE>()) continue;
            if (!filter(*d)) continue;

            device.push_back(*d);
        }
    }

    return device;
}

/// Create command queues on devices by given criteria.
/**
 * \param filter  Device filter functor. Functors may be combined with logical
 *                operators.
 * \param properties Command queue properties.
 *
 * \returns list of queues accociated with selected devices.
 * \see device_list
 */
template<class DevFilter>
std::pair<std::vector<cl::Context>, std::vector<cl::CommandQueue>>
queue_list(DevFilter &&filter, cl_command_queue_properties properties = 0) {
    std::vector<cl::Context>      context;
    std::vector<cl::CommandQueue> queue;

    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);

    for(auto p = platforms.begin(); p != platforms.end(); p++) {
        std::vector<cl::Device> device;
        std::vector<cl::Device> dev_list;

        p->getDevices(CL_DEVICE_TYPE_ALL, &dev_list);

        for(auto d = dev_list.begin(); d != dev_list.end(); d++) {
            if (!d->getInfo<CL_DEVICE_AVAILABLE>()) continue;
            if (!filter(*d)) continue;

            device.push_back(*d);
        }

        if (device.empty()) continue;

        for(auto d = device.begin(); d != device.end(); d++)
            try {
                context.push_back(cl::Context(std::vector<cl::Device>(1, *d)));
                queue.push_back(cl::CommandQueue(context.back(), *d, properties));
            } catch(const cl::Error&) {
                // Something bad happened. Better skip this device.
            }
    }

    return std::make_pair(context, queue);
}

class Context;

template <bool dummy = true>
class StaticContext {
    static_assert(dummy, "dummy parameter should be true");

    public:
        static void set(Context &ctx) {
            instance = &ctx;
        }

        static const Context& get() {
            precondition(instance, "Uninitialized static context");
            return *instance;
        }
    private:
        static Context *instance;
};

template <bool dummy>
Context* StaticContext<dummy>::instance = 0;

/// Returns reference to the latest instance of vex::Context.
inline const Context& current_context() {
    return StaticContext<>::get();
}

/// VexCL context holder.
/**
 * Holds vectors of cl::Contexts and cl::CommandQueues returned by queue_list.
 */
class Context {
    public:
        /// Initialize context from a device filter.
        template <class DevFilter>
        explicit Context(
                DevFilter&& filter, cl_command_queue_properties properties = 0
                )
        {
            std::tie(c, q) = queue_list(std::forward<DevFilter>(filter), properties);

#ifdef VEXCL_THROW_ON_EMPTY_CONTEXT
            precondition(!q.empty(), "No compute devices found");
#endif

            StaticContext<>::set(*this);
        }

        /// Initializes context from user-supplied list of cl::Contexts and cl::CommandQueues.
        Context(const std::vector<std::pair<cl::Context, cl::CommandQueue>> &user_ctx) {
            c.reserve(user_ctx.size());
            q.reserve(user_ctx.size());
            for(auto u = user_ctx.begin(); u != user_ctx.end(); u++) {
                c.push_back(u->first);
                q.push_back(u->second);
            }

            StaticContext<>::set(*this);
        }

        const std::vector<cl::Context>& context() const {
            return c;
        }

        const cl::Context& context(unsigned d) const {
            return c[d];
        }

        const std::vector<cl::CommandQueue>& queue() const {
            return q;
        }

        operator const std::vector<cl::CommandQueue>&() const {
            return q;
        }

        const cl::CommandQueue& queue(unsigned d) const {
            return q[d];
        }

        cl::Device device(unsigned d) const {
            return qdev(q[d]);
        }

        size_t size() const {
            return q.size();
        }

        bool empty() const {
            return q.empty();
        }

        operator bool() const {
            return !empty();
        }

        void finish() const {
            for(auto queue = q.begin(); queue != q.end(); ++queue)
                queue->finish();
        }
    private:
        std::vector<cl::Context>      c;
        std::vector<cl::CommandQueue> q;
};

} // namespace vex

/// Output device name to stream.
inline std::ostream& operator<<(std::ostream &os, const cl::Device &device) {
    return os << device.getInfo<CL_DEVICE_NAME>() << " ("
              << cl::Platform(device.getInfo<CL_DEVICE_PLATFORM>()).getInfo<CL_PLATFORM_NAME>()
              << ")";
}

/// Output list of devices to stream.
inline std::ostream& operator<<(std::ostream &os, const std::vector<cl::Device> &device) {
    unsigned p = 1;

    for(auto d = device.begin(); d != device.end(); d++)
        os << p++ << ". " << *d << std::endl;

    return os;
}

/// Output list of devices to stream.
inline std::ostream& operator<<(std::ostream &os, const std::vector<cl::CommandQueue> &queue) {
    unsigned p = 1;

    for(auto q = queue.begin(); q != queue.end(); q++)
        os << p++ << ". " << vex::qdev(*q) << std::endl;

    return os;
}

/// Output list of devices to stream.
inline std::ostream& operator<<(std::ostream &os, const vex::Context &ctx) {
    return os << ctx.queue();
}

#endif
