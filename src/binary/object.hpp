#ifndef OBJECT_HPP
#define OBJECT_HPP

#include "../utils/common.hpp"
#include "../utils/utils.hpp"

#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>

#if IS_LINUX || IS_APPLE
 #include <unistd.h>
 #include <dlfcn.h>
 #if IS_APPLE
  #include "mach-o.hpp"
 #else
  #include "elf.hpp"
 #endif
 #ifdef CPPTRACE_HAS_DL_FIND_OBJECT
  #include <link.h>
 #endif
#elif IS_WINDOWS
 #include <windows.h>
 #include "pe.hpp"
#endif

namespace cpptrace {
namespace detail {
    #if IS_LINUX || IS_APPLE
    #if !IS_APPLE
    inline std::uintptr_t get_module_image_base(const std::string& object_path) {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        static std::unordered_map<std::string, std::uintptr_t> cache;
        auto it = cache.find(object_path);
        if(it == cache.end()) {
            // arguably it'd be better to release the lock while computing this, but also arguably it's good to not
            // have two threads try to do the same computation
            auto base = elf_get_module_image_base(object_path);
            cache.insert(it, {object_path, base});
            return base;
        } else {
            return it->second;
        }
    }
    #else
    inline std::uintptr_t get_module_image_base(const std::string& object_path) {
        // We have to parse the Mach-O to find the offset of the text section.....
        // I don't know how addresses are handled if there is more than one __TEXT load command. I'm assuming for
        // now that there is only one, and I'm using only the first section entry within that load command.
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        static std::unordered_map<std::string, std::uintptr_t> cache;
        auto it = cache.find(object_path);
        if(it == cache.end()) {
            // arguably it'd be better to release the lock while computing this, but also arguably it's good to not
            // have two threads try to do the same computation
            auto base = mach_o(object_path).get_text_vmaddr();
            cache.insert(it, {object_path, base});
            return base;
        } else {
            return it->second;
        }
    }
    #endif
    #ifdef CPPTRACE_HAS_DL_FIND_OBJECT
    inline object_frame get_frame_object_info(frame_ptr address) {
        // Use _dl_find_object when we can, it's orders of magnitude faster
        object_frame frame;
        frame.raw_address = address;
        frame.object_address = 0;
        dl_find_object result;
        if(_dl_find_object(reinterpret_cast<void*>(address), &result) == 0) { // thread safe
            if(result.dlfo_link_map->l_name != nullptr && result.dlfo_link_map->l_name[0] != 0) {
                frame.object_path = result.dlfo_link_map->l_name;
            } else {
                // empty l_name, this means it's the currently running executable
                // TODO: Caching and proper handling
                char buffer[CPPTRACE_PATH_MAX + 1]{};
                auto res = readlink("/proc/self/exe", buffer, CPPTRACE_PATH_MAX);
                if(res == -1) {
                    // error handling?
                } else {
                    frame.object_path = buffer;
                }
            }
            frame.object_address = address
                                    - to_frame_ptr(result.dlfo_link_map->l_addr)
                                    + get_module_image_base(frame.object_path);
        }
        return frame;
    }
    #else
    // dladdr queries are needed to get pre-ASLR addresses and targets to run addr2line on
    inline object_frame get_frame_object_info(frame_ptr address) {
        // reference: https://github.com/bminor/glibc/blob/master/debug/backtracesyms.c
        Dl_info info;
        object_frame frame;
        frame.raw_address = address;
        frame.object_address = 0;
        if(dladdr(reinterpret_cast<void*>(address), &info)) { // thread safe
            frame.object_path = info.dli_fname;
            frame.object_address = address
                                    - reinterpret_cast<std::uintptr_t>(info.dli_fbase)
                                    + get_module_image_base(info.dli_fname);
        }
        return frame;
    }
    #endif
    #else
    inline std::string get_module_name(HMODULE handle) {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        static std::unordered_map<HMODULE, std::string> cache;
        auto it = cache.find(handle);
        if(it == cache.end()) {
            char path[MAX_PATH];
            if(GetModuleFileNameA(handle, path, sizeof(path))) {
                ///std::fprintf(stderr, "path: %s base: %p\n", path, handle);
                cache.insert(it, {handle, path});
                return path;
            } else {
                std::fprintf(stderr, "%s\n", std::system_error(GetLastError(), std::system_category()).what());
                cache.insert(it, {handle, ""});
                return "";
            }
        } else {
            return it->second;
        }
    }

    inline std::uintptr_t get_module_image_base(const std::string& object_path) {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        static std::unordered_map<std::string, std::uintptr_t> cache;
        auto it = cache.find(object_path);
        if(it == cache.end()) {
            // arguably it'd be better to release the lock while computing this, but also arguably it's good to not
            // have two threads try to do the same computation
            auto base = pe_get_module_image_base(object_path);
            cache.insert(it, {object_path, base});
            return base;
        } else {
            return it->second;
        }
    }

    inline object_frame get_frame_object_info(frame_ptr address) {
        object_frame frame;
        frame.raw_address = address;
        frame.object_address = 0;
        HMODULE handle;
        // Multithread safe as long as another thread doesn't come along and free the module
        if(GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<const char*>(address),
            &handle
        )) {
            frame.object_path = get_module_name(handle);
            frame.object_address = address
                                - reinterpret_cast<std::uintptr_t>(handle)
                                + get_module_image_base(frame.object_path);
        } else {
            std::fprintf(stderr, "%s\n", std::system_error(GetLastError(), std::system_category()).what());
        }
        return frame;
    }
    #endif

    inline std::vector<object_frame> get_frames_object_info(const std::vector<frame_ptr>& addresses) {
        std::vector<object_frame> frames;
        frames.reserve(addresses.size());
        for(const frame_ptr address : addresses) {
            frames.push_back(get_frame_object_info(address));
        }
        return frames;
    }

    inline object_frame resolve_safe_object_frame(const safe_object_frame& frame) {
        return {
            frame.raw_address,
            frame.address_relative_to_object_start + get_module_image_base(frame.object_path),
            frame.object_path
        };
    }
}
}

#endif
