#include "vtenc/capabilities.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdexcept>

namespace vtenc {

std::string metal_device_name() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) throw std::runtime_error("MTLCreateSystemDefaultDevice returned no Metal device");
        const char* name = device.name.UTF8String;
        if (!name) throw std::runtime_error("Metal device name is not valid UTF-8");
        return std::string(name);
    }
}

} // namespace vtenc
